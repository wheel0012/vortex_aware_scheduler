// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <string.h>
#include <assert.h>
#include <util.h>
#include "types.h"
#include "arch.h"
#include "mem.h"
#include "core.h"
#include "debug.h"
#include "constants.h"
#include "warp_sched_trace.h"

using namespace vortex;

namespace {

#ifndef SIMX_ICACHE_REQ_LATENCY
#define SIMX_ICACHE_REQ_LATENCY 2
#endif

#ifndef SIMX_OPERANDS_LATENCY
#define SIMX_OPERANDS_LATENCY 1
#endif

#ifndef SIMX_DISPATCH_LATENCY
#define SIMX_DISPATCH_LATENCY 2
#endif

uint64_t parse_u64_env(const char* name, uint64_t default_value) {
  auto value = std::getenv(name);
  if (value == nullptr || value[0] == '\0')
    return default_value;
  char* end = nullptr;
  auto parsed = std::strtoull(value, &end, 0);
  return (end != value) ? parsed : default_value;
}

uint64_t abs_diff_u64(uint64_t lhs, uint64_t rhs) {
  return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

uint64_t wid_bit(uint32_t wid) {
  return wid < 64 ? (1ull << wid) : 0;
}

uint32_t pct_u64(uint64_t value, uint64_t total) {
  return total ? static_cast<uint32_t>((100ull * value) / total) : 0;
}

bool env_enabled(const char* name) {
  auto value = std::getenv(name);
  return value && value[0] != '\0' && std::string(value) != "0" && std::string(value) != "false";
}

bool userpc_feature_wg_id(uint64_t addr,
                          uint64_t feature_base,
                          uint64_t npoints,
                          uint64_t nfeatures,
                          uint64_t wg_size,
                          uint64_t* wg_id) {
  if (npoints == 0 || nfeatures == 0 || wg_size == 0)
    return false;
  uint64_t feature_bytes = npoints * nfeatures * sizeof(float);
  if (addr < feature_base || addr >= feature_base + feature_bytes)
    return false;
  uint64_t element = (addr - feature_base) / sizeof(float);
  uint64_t point_id = element % npoints;
  *wg_id = point_id / wg_size;
  return true;
}

std::string to_hex_string(uint64_t value) {
  std::ostringstream os;
  os << "0x" << std::hex << value;
  return os.str();
}

enum UserPCReadLocality : uint32_t {
  UserPCReadLocalityCold = 0,
  UserPCReadLocalityThreadLocal = 1,
  UserPCReadLocalityIntraWarp = 2,
  UserPCReadLocalityInterWarp = 3
};

std::string trace_inst_type(const instr_trace_t* trace) {
  std::ostringstream os;
  std::visit([&](auto&& op_type) {
    os << op_type;
  }, trace->op_type);
  return os.str();
}

std::string trace_score_vector(const std::vector<uint64_t>& scores) {
  std::ostringstream os;
  for (uint32_t i = 0, n = scores.size(); i < n; ++i) {
    if (i) {
      os << '|';
    }
    os << scores.at(i);
  }
  return os.str();
}

void write_warp_sched_trace(uint32_t core_id,
                            uint32_t issue_slot,
                            bool issued,
                            int preferred_wid,
                            int intended_wid,
                            int actual_wid,
                            const instr_trace_t* trace,
                            const std::string& score,
                            const std::string& score_vector,
                            uint64_t candidate_mask,
                            uint64_t ready_mask,
                            uint64_t ibuffer_empty_mask,
                            bool preferred_blocked,
                            const std::string& preferred_block_reason,
                            const std::string& stall_reason,
                            const std::string& mismatch_reason,
                            bool userpc_relevant) {
  if (!WarpSchedTrace::enabled())
    return;
  if (WarpSchedTrace::userpc_only() && !userpc_relevant)
    return;

  WarpSchedTrace::Row row;
  row.cycle = SimPlatform::instance().cycles();
  row.core_id = core_id;
  row.issue_slot = issue_slot;
  row.issued = issued;
  row.preferred_wid = preferred_wid;
  row.intended_wid = intended_wid;
  row.actual_wid = actual_wid;
  row.selected_wid = actual_wid;
  if (trace) {
    row.pc = to_hex_string(trace->PC);
    row.inst_type = trace_inst_type(trace);
  }
  row.score = score;
  row.score_vector = score_vector;
  row.candidate_mask = to_hex_string(candidate_mask);
  row.ready_mask = to_hex_string(ready_mask);
  row.ibuffer_empty_mask = to_hex_string(ibuffer_empty_mask);
  row.ibuffer_empty = actual_wid >= 0 ? false : (candidate_mask == 0);
  row.preferred_blocked = preferred_blocked;
  row.preferred_block_reason = preferred_blocked ? preferred_block_reason : "none";
  row.not_ready_fallback = preferred_blocked;
  row.fallback = issued && intended_wid >= 0 && actual_wid >= 0 && intended_wid != actual_wid;
  row.stall_reason = stall_reason;
  row.mismatch = row.fallback;
  row.mismatch_reason = row.mismatch ? mismatch_reason : "none";
  WarpSchedTrace::write_issue(row);
}

} // namespace

Core::Core(const SimContext& ctx,
           uint32_t core_id,
           Socket* socket,
           const Arch &arch,
           const DCRS &dcrs
           )
  : SimObject(ctx, StrFormat("core%d", core_id))
  , icache_req_ports(1, this)
  , icache_rsp_ports(1, this)
  , dcache_req_ports(DCACHE_NUM_REQS, this)
  , dcache_rsp_ports(DCACHE_NUM_REQS, this)
  , core_id_(core_id)
  , socket_(socket)
  , arch_(arch)
#ifdef EXT_TCU_ENABLE
  , tensor_unit_(TensorUnit::Create("tcu", arch, this))
#endif
#ifdef EXT_V_ENABLE
  , vec_unit_(VecUnit::Create("vpu", arch, this))
#endif
  , sched_criticality_(arch.num_warps(), 0)
  , emulator_(arch, dcrs, this)
  , ibuffers_(arch.num_warps(), IBUF_SIZE)
  , scoreboard_(arch_)
  , operands_(ISSUE_WIDTH)
  , dispatchers_((uint32_t)FUType::Count)
  , func_units_((uint32_t)FUType::Count)
  , lmem_switch_(NUM_LSU_BLOCKS)
  , mem_coalescers_(NUM_LSU_BLOCKS)
  , pending_icache_(arch_.num_warps())
  , commit_arbs_(ISSUE_WIDTH)
  , ibuffer_spawn_times_(ISSUE_WIDTH, std::vector<uint64_t>(PER_ISSUE_WARPS, 0))
  , ibuffer_criticality_(ISSUE_WIDTH, std::vector<uint64_t>(PER_ISSUE_WARPS, 0))
  , ibuffer_block_ids_(ISSUE_WIDTH, std::vector<uint64_t>(PER_ISSUE_WARPS, 0))
  , ibuffer_arbs_(ISSUE_WIDTH)
  , cpl_inst_pending_(arch_.num_warps(), 0)
  , cpl_stall_cycles_(arch_.num_warps(), 0)
  , cpl_committed_instrs_(arch_.num_warps(), 0)
  , cpl_last_issue_cycle_(arch_.num_warps(), std::numeric_limits<uint64_t>::max())
  , cpl_arbitration_loss_(arch_.num_warps(), 0)
  , cpl_max_committed_(0)
  , dbg_grant_count_(arch_.num_warps(), 0)
  , dbg_last_grant_(ISSUE_WIDTH, uint32_t(-1))
  , dbg_stick_count_(ISSUE_WIDTH, 0)
  , dbg_swap_count_(ISSUE_WIDTH, 0)
  , dbg_warp_ibuf_empty_(arch_.num_warps(), 0)
  , dbg_slot_all_empty_(ISSUE_WIDTH, 0)
  , dbg_slot_scrb_block_(ISSUE_WIDTH, 0)
  , dbg_slot_issued_(ISSUE_WIDTH, 0)
  , dbg_warp_scrb_block_(arch_.num_warps(), 0)
  , dbg_ready_size_hist_(ISSUE_WIDTH, std::array<uint64_t, 5>{0, 0, 0, 0, 0})
  , dbg_pick_same_as_rr_(ISSUE_WIDTH, 0)
  , dbg_pick_diff_from_rr_(ISSUE_WIDTH, 0)
  , dbg_crit_snap_last_cycle_(0)
{
  char sname[100];

  // Populate per-slot block_id table.  Each global wid is mapped to a
  // thread-block index via wid / WSPAWN_WARPS_PER_BLOCK.  If the macro is
  // zero (legacy), every warp ends up in block 0 → gCAWS block-local filter
  // becomes a no-op, preserving the original cross-core comparison.
#ifndef WSPAWN_WARPS_PER_BLOCK
#define WSPAWN_WARPS_PER_BLOCK 0
#endif
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    for (uint32_t w = 0; w < PER_ISSUE_WARPS; ++w) {
      uint32_t wid = w * ISSUE_WIDTH + iw;
      ibuffer_block_ids_.at(iw).at(w) =
          (WSPAWN_WARPS_PER_BLOCK > 0) ? (wid / WSPAWN_WARPS_PER_BLOCK) : 0;
    }
  }
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    operands_.at(iw) = Operands::Create(this);
    ibuffer_arbs_.at(iw) = Arbiter(configured_issue_arbiter(),
                                   PER_ISSUE_WARPS,
                                   &ibuffer_spawn_times_.at(iw),
                                   &ibuffer_criticality_.at(iw),
                                   &ibuffer_block_ids_.at(iw));
    dbg_shadow_rr_.emplace_back(ArbiterType::RoundRobin, PER_ISSUE_WARPS);
  }

  // create the memory coalescer
  for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
    snprintf(sname, 100, "%s-coalescer%d", this->name().c_str(), b);
    mem_coalescers_.at(b) = MemCoalescer::Create(sname, LSU_CHANNELS, DCACHE_CHANNELS, DCACHE_WORD_SIZE, LSUQ_OUT_SIZE, 1);
  }

  // create local memory
  snprintf(sname, 100, "%s-lmem", this->name().c_str());
  local_mem_ = LocalMem::Create(sname, LocalMem::Config{
    (1 << LMEM_LOG_SIZE),
    LSU_WORD_SIZE,
    LSU_CHANNELS,
    log2ceil(LMEM_NUM_BANKS),
    false
  });

  // create lmem switch
  for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
    snprintf(sname, 100, "%s-lmem_switch%d", this->name().c_str(), b);
    lmem_switch_.at(b) = LocalMemSwitch::Create(sname, 1);
  }

  // create dcache adapter
  std::vector<LsuMemAdapter::Ptr> lsu_dcache_adapter(NUM_LSU_BLOCKS);
  for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
    snprintf(sname, 100, "%s-lsu_dcache_adapter%d", this->name().c_str(), b);
    lsu_dcache_adapter.at(b) = LsuMemAdapter::Create(sname, DCACHE_CHANNELS, 1);
  }

  // create lmem arbiter
  snprintf(sname, 100, "%s-lmem_arb", this->name().c_str());
  auto lmem_arb = LsuArbiter::Create(sname, ArbiterType::RoundRobin, NUM_LSU_BLOCKS, 1);

  // create lmem adapter
  snprintf(sname, 100, "%s-lsu_lmem_adapter", this->name().c_str());
  auto lsu_lmem_adapter = LsuMemAdapter::Create(sname, LSU_CHANNELS, 1);

  // connect lmem switch
  for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
    lmem_switch_.at(b)->ReqDC.bind(&mem_coalescers_.at(b)->ReqIn);
    lmem_switch_.at(b)->ReqLmem.bind(&lmem_arb->ReqIn.at(b));

    mem_coalescers_.at(b)->RspIn.bind(&lmem_switch_.at(b)->RspDC);
    lmem_arb->RspIn.at(b).bind(&lmem_switch_.at(b)->RspLmem);
  }

  // connect lmem arbiter
  lmem_arb->ReqOut.at(0).bind(&lsu_lmem_adapter->ReqIn);
  lsu_lmem_adapter->RspIn.bind(&lmem_arb->RspOut.at(0));

  // connect lmem adapter
  for (uint32_t c = 0; c < LSU_CHANNELS; ++c) {
    lsu_lmem_adapter->ReqOut.at(c).bind(&local_mem_->Inputs.at(c));
    local_mem_->Outputs.at(c).bind(&lsu_lmem_adapter->RspOut.at(c));
  }

  // connect dcache coalescer
  for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
    mem_coalescers_.at(b)->ReqOut.bind(&lsu_dcache_adapter.at(b)->ReqIn);
    lsu_dcache_adapter.at(b)->RspIn.bind(&mem_coalescers_.at(b)->RspOut);
  }

  // connect dcache adapter
  for (uint32_t b = 0; b < NUM_LSU_BLOCKS; ++b) {
    for (uint32_t c = 0; c < DCACHE_CHANNELS; ++c) {
      uint32_t p = b * DCACHE_CHANNELS + c;
      lsu_dcache_adapter.at(b)->ReqOut.at(c).bind(&dcache_req_ports.at(p));
      dcache_rsp_ports.at(p).bind(&lsu_dcache_adapter.at(b)->RspOut.at(c));
    }
  }

  for (auto& req_port : dcache_req_ports) {
    req_port.tx_callback([this](const MemReq& req, uint64_t cycle) {
      __unused(cycle);
      if (!req.userpc && !userpc_lsu_uuids_.count(req.uuid))
        return;
      userpc_perf_.dcache_reads += !req.write;
      userpc_perf_.dcache_writes += req.write;
      userpc_perf_.dcache_pending_reads += !req.write;
      if (!req.write) {
        constexpr uint64_t line_size = L1_LINE_SIZE;
        constexpr uint64_t raw_num_sets = DCACHE_SIZE / (L1_LINE_SIZE * DCACHE_NUM_WAYS);
        constexpr uint64_t num_sets = raw_num_sets ? raw_num_sets : 1;
        auto line = req.addr / line_size;
        auto lane_pending_it = userpc_lane_dcache_pending_groups_.find(req.uuid);
        UserPCLaneDCacheReadGroup lane_group;
        if (lane_pending_it != userpc_lane_dcache_pending_groups_.end()) {
          auto& groups = lane_pending_it->second;
          auto group_it = std::find_if(groups.begin(), groups.end(), [line](const auto& entry) {
            return entry.line == line;
          });
          if (group_it != groups.end()) {
            lane_group = group_it->group;
            groups.erase(group_it);
          }
          if (groups.empty()) {
            userpc_lane_dcache_pending_groups_.erase(lane_pending_it);
          }
        }
        userpc_lane_dcache_rsp_groups_[req.uuid].push_back(lane_group);
        auto set = line % num_sets;
        auto tag = line / num_sets;
        uint64_t wg_id = 0;
        bool has_wg_id =
            userpc_dcache_locality_wg_enabled_
            && userpc_feature_wg_id(req.addr,
                                    userpc_dcache_locality_feature_base_,
                                    userpc_dcache_locality_npoints_,
                                    userpc_dcache_locality_nfeatures_,
                                    userpc_dcache_locality_wg_size_,
                                    &wg_id);
        ++userpc_perf_.dcache_read_access_index;
        auto seen_line = !userpc_dcache_read_lines_.insert(line).second;
        userpc_dcache_pending_read_cold_[req.uuid].push_back(!seen_line);
        auto owner_it = userpc_dcache_last_read_owner_.find(line);
        uint32_t locality = UserPCReadLocalityCold;
        if (owner_it != userpc_dcache_last_read_owner_.end()) {
          bool same_wg =
              !has_wg_id
              || !owner_it->second.has_wg_id
              || owner_it->second.wg_id == wg_id;
          if (same_wg && owner_it->second.wid == req.wid) {
            locality = (owner_it->second.tid == req.tid)
                         ? UserPCReadLocalityThreadLocal
                         : UserPCReadLocalityIntraWarp;
          } else {
            locality = UserPCReadLocalityInterWarp;
          }
        }
        userpc_dcache_pending_read_locality_[req.uuid].push_back(locality);
        switch (locality) {
        case UserPCReadLocalityThreadLocal:
          ++userpc_perf_.dcache_read_locality_thread_local_accesses;
          break;
        case UserPCReadLocalityIntraWarp:
          ++userpc_perf_.dcache_read_locality_intra_warp_accesses;
          break;
        case UserPCReadLocalityInterWarp:
          ++userpc_perf_.dcache_read_locality_inter_warp_accesses;
          break;
        default:
          ++userpc_perf_.dcache_read_locality_cold_accesses;
          break;
        }
        if (seen_line) {
          ++userpc_perf_.dcache_read_reuse_accesses;
          auto last_it = userpc_dcache_last_read_access_.find(line);
          if (last_it != userpc_dcache_last_read_access_.end()) {
            auto gap = userpc_perf_.dcache_read_access_index - last_it->second;
            userpc_perf_.dcache_read_reuse_distance_sum += gap;
            if (gap <= 4) {
              ++userpc_perf_.dcache_read_reuse_gap_le4;
            } else if (gap <= 16) {
              ++userpc_perf_.dcache_read_reuse_gap_le16;
            } else if (gap <= 64) {
              ++userpc_perf_.dcache_read_reuse_gap_le64;
            } else if (gap <= 256) {
              ++userpc_perf_.dcache_read_reuse_gap_le256;
            } else {
              ++userpc_perf_.dcache_read_reuse_gap_gt256;
            }
          }
        } else {
          ++userpc_perf_.dcache_read_cold_accesses;
        }
        userpc_dcache_last_read_access_[line] = userpc_perf_.dcache_read_access_index;
        userpc_dcache_last_read_owner_[line] = UserPCDCacheReadOwner{req.wid, req.tid, req.uuid, wg_id, has_wg_id};
        userpc_dcache_tags_by_set_[set].insert(tag);
        if (userpc_perf_.dcache_set_valid
            && userpc_perf_.dcache_last_read_set == set
            && userpc_perf_.dcache_last_read_tag != tag) {
          ++userpc_perf_.dcache_read_same_set_tag_changes;
        }
        userpc_perf_.dcache_last_read_set = set;
        userpc_perf_.dcache_last_read_tag = tag;
        userpc_perf_.dcache_set_valid = true;
        if (userpc_perf_.dcache_line_stride_valid) {
          auto line_stride = abs_diff_u64(line, userpc_perf_.dcache_last_read_line);
          userpc_perf_.dcache_read_line_stride_sum += line_stride;
          ++userpc_perf_.dcache_read_line_stride_count;
          if (line_stride == 0) {
            ++userpc_perf_.dcache_read_line_stride_0;
          } else if (line_stride == 1) {
            ++userpc_perf_.dcache_read_line_stride_1;
          } else if (line_stride < 4) {
            ++userpc_perf_.dcache_read_line_stride_2_3;
          } else if (line_stride < 16) {
            ++userpc_perf_.dcache_read_line_stride_4_15;
          } else if (line_stride < 64) {
            ++userpc_perf_.dcache_read_line_stride_16_63;
          } else {
            ++userpc_perf_.dcache_read_line_stride_64_plus;
          }
        }
        userpc_perf_.dcache_last_read_line = line;
        userpc_perf_.dcache_line_stride_valid = true;
        if (userpc_perf_.dcache_stride_valid) {
          auto stride = abs_diff_u64(req.addr, userpc_perf_.dcache_last_read_addr);
          userpc_perf_.dcache_read_stride_sum += stride;
          userpc_perf_.dcache_read_stride_capped_4k_sum += std::min<uint64_t>(stride, 4096);
          ++userpc_perf_.dcache_read_stride_count;
          if (stride == 0) {
            ++userpc_perf_.dcache_read_stride_0;
          } else if (stride < 64) {
            ++userpc_perf_.dcache_read_stride_1_63;
          } else if (stride < 256) {
            ++userpc_perf_.dcache_read_stride_64_255;
          } else if (stride < 1024) {
            ++userpc_perf_.dcache_read_stride_256_1023;
          } else if (stride < 4096) {
            ++userpc_perf_.dcache_read_stride_1k_4k;
          } else {
            ++userpc_perf_.dcache_read_stride_4k_plus;
          }
        }
        userpc_perf_.dcache_last_read_addr = req.addr;
        userpc_perf_.dcache_stride_valid = true;
      }
    });
  }
  for (auto& rsp_port : dcache_rsp_ports) {
    rsp_port.tx_callback([this](const MemRsp& rsp, uint64_t cycle) {
      __unused(cycle);
      if (!rsp.userpc && !userpc_lsu_uuids_.count(rsp.uuid))
        return;
      if (userpc_perf_.dcache_pending_reads != 0)
        --userpc_perf_.dcache_pending_reads;
      bool read_cold = false;
      bool have_read_cold = false;
      uint32_t locality = UserPCReadLocalityCold;
      bool have_locality = false;
      UserPCLaneDCacheReadGroup lane_group;
      bool have_lane_group = false;
      if (!rsp.write) {
        auto it = userpc_dcache_pending_read_cold_.find(rsp.uuid);
        if (it != userpc_dcache_pending_read_cold_.end() && !it->second.empty()) {
          read_cold = it->second.front();
          have_read_cold = true;
          it->second.pop_front();
          if (it->second.empty())
            userpc_dcache_pending_read_cold_.erase(it);
        }
        auto loc_it = userpc_dcache_pending_read_locality_.find(rsp.uuid);
        if (loc_it != userpc_dcache_pending_read_locality_.end() && !loc_it->second.empty()) {
          locality = loc_it->second.front();
          have_locality = true;
          loc_it->second.pop_front();
          if (loc_it->second.empty())
            userpc_dcache_pending_read_locality_.erase(loc_it);
        }
        auto lane_it = userpc_lane_dcache_rsp_groups_.find(rsp.uuid);
        if (lane_it != userpc_lane_dcache_rsp_groups_.end() && !lane_it->second.empty()) {
          lane_group = lane_it->second.front();
          have_lane_group = true;
          lane_it->second.pop_front();
          if (lane_it->second.empty())
            userpc_lane_dcache_rsp_groups_.erase(lane_it);
        }
      }
      if (!rsp.write) {
        bool l1_hit = (rsp.cache_hit_mask & MemCacheLevelL1) || !rsp.miss;
        bool l2_hit = (rsp.cache_hit_mask & MemCacheLevelL2) != 0;
        if (l1_hit) {
          ++userpc_perf_.dcache_read_l1_hits;
        } else if (l2_hit) {
          ++userpc_perf_.dcache_read_l2_hits;
        } else {
          ++userpc_perf_.dcache_read_memory_misses;
        }
      }
      if (rsp.miss) {
        if (rsp.write) {
          ++userpc_perf_.dcache_write_misses;
        } else {
          ++userpc_perf_.dcache_read_misses;
          if (have_read_cold) {
            if (read_cold) {
              ++userpc_perf_.dcache_read_cold_misses;
            } else {
              ++userpc_perf_.dcache_read_non_cold_misses;
            }
          }
          if (have_locality) {
            switch (locality) {
            case UserPCReadLocalityThreadLocal:
              ++userpc_perf_.dcache_read_locality_thread_local_misses;
              break;
            case UserPCReadLocalityIntraWarp:
              ++userpc_perf_.dcache_read_locality_intra_warp_misses;
              break;
            case UserPCReadLocalityInterWarp:
              ++userpc_perf_.dcache_read_locality_inter_warp_misses;
              break;
            default:
              ++userpc_perf_.dcache_read_locality_cold_misses;
              break;
            }
          }
          if (have_lane_group) {
            bool l2_hit = (rsp.cache_hit_mask & MemCacheLevelL2) != 0;
            if (l2_hit) {
              userpc_perf_.lane_dcache_read_l2_hit_accesses += lane_group.total();
              userpc_perf_.lane_dcache_read_l2_cold_hits += lane_group.cold;
              userpc_perf_.lane_dcache_read_l2_same_inst_hits += lane_group.same_inst;
              userpc_perf_.lane_dcache_read_l2_thread_local_hits += lane_group.thread_local_count;
              userpc_perf_.lane_dcache_read_l2_intra_warp_hits += lane_group.intra_warp;
              userpc_perf_.lane_dcache_read_l2_inter_warp_hits += lane_group.inter_warp;
            } else {
              userpc_perf_.lane_dcache_read_memory_miss_accesses += lane_group.total();
              userpc_perf_.lane_dcache_read_memory_cold_misses += lane_group.cold;
              userpc_perf_.lane_dcache_read_memory_same_inst_misses += lane_group.same_inst;
              userpc_perf_.lane_dcache_read_memory_thread_local_misses += lane_group.thread_local_count;
              userpc_perf_.lane_dcache_read_memory_intra_warp_misses += lane_group.intra_warp;
              userpc_perf_.lane_dcache_read_memory_inter_warp_misses += lane_group.inter_warp;
            }
          }
        }
      } else if (!rsp.write && have_lane_group) {
        userpc_perf_.lane_dcache_read_hit_accesses += lane_group.total();
        userpc_perf_.lane_dcache_read_cold_hits += lane_group.cold;
        userpc_perf_.lane_dcache_read_same_inst_hits += lane_group.same_inst;
        userpc_perf_.lane_dcache_read_thread_local_hits += lane_group.thread_local_count;
        userpc_perf_.lane_dcache_read_intra_warp_hits += lane_group.intra_warp;
        userpc_perf_.lane_dcache_read_inter_warp_hits += lane_group.inter_warp;
      }
    });
  }

  // initialize dispatchers
  dispatchers_.at((int)FUType::ALU) = SimPlatform::instance().create_object<Dispatcher>(this, 2, NUM_ALU_BLOCKS, NUM_ALU_LANES);
  dispatchers_.at((int)FUType::FPU) = SimPlatform::instance().create_object<Dispatcher>(this, 2, NUM_FPU_BLOCKS, NUM_FPU_LANES);
  dispatchers_.at((int)FUType::LSU) = SimPlatform::instance().create_object<Dispatcher>(this, 2, NUM_LSU_BLOCKS, NUM_LSU_LANES);
  dispatchers_.at((int)FUType::SFU) = SimPlatform::instance().create_object<Dispatcher>(this, 2, NUM_SFU_BLOCKS, NUM_SFU_LANES);
#ifdef EXT_V_ENABLE
  dispatchers_.at((int)FUType::VPU) = SimPlatform::instance().create_object<Dispatcher>(this, 2, NUM_VPU_BLOCKS, NUM_VPU_LANES);
#endif
#ifdef EXT_TCU_ENABLE
  dispatchers_.at((int)FUType::TCU) = SimPlatform::instance().create_object<Dispatcher>(this, 2, NUM_TCU_BLOCKS, NUM_TCU_LANES);
#endif

  // initialize execute units
  func_units_.at((int)FUType::ALU) = SimPlatform::instance().create_object<AluUnit>(this);
  func_units_.at((int)FUType::FPU) = SimPlatform::instance().create_object<FpuUnit>(this);
  func_units_.at((int)FUType::LSU) = SimPlatform::instance().create_object<LsuUnit>(this);
  func_units_.at((int)FUType::SFU) = SimPlatform::instance().create_object<SfuUnit>(this);
#ifdef EXT_V_ENABLE
  func_units_.at((int)FUType::VPU) = SimPlatform::instance().create_object<VpuUnit>(this);
#endif
#ifdef EXT_TCU_ENABLE
  func_units_.at((int)FUType::TCU) = SimPlatform::instance().create_object<TcuUnit>(this);
#endif

  // bind commit arbiters
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    snprintf(sname, 100, "%s-commit-arb%d", this->name().c_str(), iw);
    auto arbiter = TraceArbiter::Create(sname, ArbiterType::RoundRobin, (uint32_t)FUType::Count, 1);
    for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
      func_units_.at(fu)->Outputs.at(iw).bind(&arbiter->Inputs.at(fu));
    }
    commit_arbs_.at(iw) = arbiter;
  }

  this->reset();
}

Core::~Core() {
  if (env_enabled("VX_CPL_DUMP")) {
    this->dump_cpl_stats();
  }
  this->dump_userpc_perf();
}

Core::UserPCPerfStats::UserPCPerfStats()
  : configured(false)
  , enabled(false)
  , pc_base(0x80000000)
  , pc_from(0)
  , pc_to(std::numeric_limits<uint64_t>::max())
  , first_cycle(std::numeric_limits<uint64_t>::max())
  , last_cycle(0)
  , last_counted_issue_cycle(std::numeric_limits<uint64_t>::max())
  , issued(0)
  , instrs(0)
  , ifetches(0)
  , ifetch_latency(0)
  , issue_cycles(0)
  , candidate_checks(0)
  , ready_checks(0)
  , candidate_sum(0)
  , ready_sum(0)
  , not_ready_fallbacks(0)
  , preferred_blocked(0)
  , issue_streak_next_checks(0)
  , same_wid_consecutive_issues(0)
  , wid_switches(0)
  , ibuf_stalls(0)
  , scrb_stalls(0)
  , scrb_blocked(0)
  , scrb_alu(0)
  , scrb_fpu(0)
  , scrb_lsu(0)
  , scrb_sfu(0)
  , scrb_csrs(0)
  , scrb_wctl(0)
#ifdef EXT_V_ENABLE
  , scrb_vpu(0)
#endif
#ifdef EXT_TCU_ENABLE
  , scrb_tcu(0)
#endif
  , loads(0)
  , stores(0)
  , load_latency(0)
  , dcache_reads(0)
  , dcache_writes(0)
  , dcache_read_misses(0)
  , dcache_write_misses(0)
  , dcache_read_l1_hits(0)
  , dcache_read_l2_hits(0)
  , dcache_read_memory_misses(0)
  , dcache_read_latency(0)
  , dcache_pending_reads(0)
  , dcache_stride_valid(false)
  , dcache_last_read_addr(0)
  , dcache_read_stride_sum(0)
  , dcache_read_stride_capped_4k_sum(0)
  , dcache_read_stride_count(0)
  , dcache_read_stride_0(0)
  , dcache_read_stride_1_63(0)
  , dcache_read_stride_64_255(0)
  , dcache_read_stride_256_1023(0)
  , dcache_read_stride_1k_4k(0)
  , dcache_read_stride_4k_plus(0)
  , dcache_read_cold_accesses(0)
  , dcache_read_reuse_accesses(0)
  , dcache_read_cold_misses(0)
  , dcache_read_non_cold_misses(0)
  , dcache_read_access_index(0)
  , dcache_read_reuse_distance_sum(0)
  , dcache_read_reuse_gap_le4(0)
  , dcache_read_reuse_gap_le16(0)
  , dcache_read_reuse_gap_le64(0)
  , dcache_read_reuse_gap_le256(0)
  , dcache_read_reuse_gap_gt256(0)
  , dcache_read_locality_cold_accesses(0)
  , dcache_read_locality_thread_local_accesses(0)
  , dcache_read_locality_intra_warp_accesses(0)
  , dcache_read_locality_inter_warp_accesses(0)
  , dcache_read_locality_cold_misses(0)
  , dcache_read_locality_thread_local_misses(0)
  , dcache_read_locality_intra_warp_misses(0)
  , dcache_read_locality_inter_warp_misses(0)
  , lane_dcache_read_accesses(0)
  , lane_dcache_read_cold_accesses(0)
  , lane_dcache_read_same_inst_accesses(0)
  , lane_dcache_read_thread_local_accesses(0)
  , lane_dcache_read_intra_warp_accesses(0)
  , lane_dcache_read_inter_warp_accesses(0)
  , lane_dcache_read_hit_accesses(0)
  , lane_dcache_read_cold_hits(0)
  , lane_dcache_read_same_inst_hits(0)
  , lane_dcache_read_thread_local_hits(0)
  , lane_dcache_read_intra_warp_hits(0)
  , lane_dcache_read_inter_warp_hits(0)
  , lane_dcache_read_l2_hit_accesses(0)
  , lane_dcache_read_l2_cold_hits(0)
  , lane_dcache_read_l2_same_inst_hits(0)
  , lane_dcache_read_l2_thread_local_hits(0)
  , lane_dcache_read_l2_intra_warp_hits(0)
  , lane_dcache_read_l2_inter_warp_hits(0)
  , lane_dcache_read_memory_miss_accesses(0)
  , lane_dcache_read_memory_cold_misses(0)
  , lane_dcache_read_memory_same_inst_misses(0)
  , lane_dcache_read_memory_thread_local_misses(0)
  , lane_dcache_read_memory_intra_warp_misses(0)
  , lane_dcache_read_memory_inter_warp_misses(0)
  , lane_dcache_read_reuse_accesses(0)
  , lane_dcache_read_reuse_distance_sum(0)
  , dcache_line_stride_valid(false)
  , dcache_last_read_line(0)
  , dcache_read_line_stride_sum(0)
  , dcache_read_line_stride_count(0)
  , dcache_read_line_stride_0(0)
  , dcache_read_line_stride_1(0)
  , dcache_read_line_stride_2_3(0)
  , dcache_read_line_stride_4_15(0)
  , dcache_read_line_stride_16_63(0)
  , dcache_read_line_stride_64_plus(0)
  , dcache_set_valid(false)
  , dcache_last_read_set(0)
  , dcache_last_read_tag(0)
  , dcache_read_same_set_tag_changes(0)
  , alu_issues(0)
  , fpu_issues(0)
  , lsu_issues(0)
  , sfu_issues(0)
#ifdef EXT_V_ENABLE
  , vpu_issues(0)
#endif
#ifdef EXT_TCU_ENABLE
  , tcu_issues(0)
#endif
{}

void Core::userpc_init() {
  userpc_perf_ = UserPCPerfStats();
  userpc_lsu_uuids_.clear();
  userpc_dcache_read_lines_.clear();
  userpc_dcache_last_read_access_.clear();
  userpc_dcache_pending_read_cold_.clear();
  userpc_dcache_pending_read_locality_.clear();
  userpc_dcache_last_read_owner_.clear();
  userpc_dcache_tags_by_set_.clear();
  userpc_lane_dcache_read_lines_.clear();
  userpc_lane_dcache_last_read_access_.clear();
  userpc_lane_dcache_last_read_owner_.clear();
  userpc_lane_dcache_pending_groups_.clear();
  userpc_lane_dcache_rsp_groups_.clear();
  userpc_dcache_locality_feature_base_ = parse_u64_env("VX_USERPC_LOCALITY_FEATURE_BASE", 0);
  userpc_dcache_locality_npoints_ = parse_u64_env("VX_USERPC_LOCALITY_NPOINTS", 0);
  userpc_dcache_locality_nfeatures_ = parse_u64_env("VX_USERPC_LOCALITY_NFEATURES", 0);
  userpc_dcache_locality_wg_size_ = parse_u64_env("VX_USERPC_LOCALITY_WG_SIZE", 0);
  userpc_dcache_locality_wg_enabled_ =
      userpc_dcache_locality_wg_size_ != 0
      && userpc_dcache_locality_npoints_ != 0
      && userpc_dcache_locality_nfeatures_ != 0;
  userpc_perf_.pc_base = parse_u64_env("VX_USER_PC_BASE", userpc_perf_.pc_base);
  auto from_env = std::getenv("VX_USER_PC_FROM");
  auto to_env = std::getenv("VX_USER_PC_TO");
  bool has_from = from_env != nullptr && from_env[0] != '\0';
  bool has_to = to_env != nullptr && to_env[0] != '\0';
  if (!has_from && !has_to)
    return;

  userpc_perf_.configured = true;
  auto from = parse_u64_env("VX_USER_PC_FROM", 0);
  auto to = parse_u64_env("VX_USER_PC_TO", std::numeric_limits<uint64_t>::max());
  if (from < userpc_perf_.pc_base)
    from += userpc_perf_.pc_base;
  if (to < userpc_perf_.pc_base)
    to += userpc_perf_.pc_base;
  userpc_perf_.pc_from = from;
  userpc_perf_.pc_to = to;
  userpc_perf_.enabled = from <= to;
  userpc_perf_.per_warp_issues.assign(arch_.num_warps(), 0);
  userpc_perf_.per_warp_first.assign(arch_.num_warps(), std::numeric_limits<uint64_t>::max());
  userpc_perf_.per_warp_last.assign(arch_.num_warps(), 0);
  userpc_perf_.last_issue_wid_by_slot.assign(ISSUE_WIDTH, -1);
  userpc_perf_.current_wid_streak_by_slot.assign(ISSUE_WIDTH, 0);
}

bool Core::userpc_contains(uint64_t pc) const {
  return userpc_perf_.enabled && pc >= userpc_perf_.pc_from && pc <= userpc_perf_.pc_to;
}

void Core::userpc_mark(instr_trace_t* trace) const {
  trace->userpc_marked = this->userpc_contains(trace->PC);
}

void Core::userpc_count_lane_dcache_read(const instr_trace_t* trace, uint32_t tid, uint64_t addr) {
  if (!this->userpc_contains(trace->PC))
    return;

  constexpr uint64_t line_size = L1_LINE_SIZE;
  auto line = addr / line_size;
  uint64_t wg_id = 0;
  bool has_wg_id =
      userpc_dcache_locality_wg_enabled_
      && userpc_feature_wg_id(addr,
                              userpc_dcache_locality_feature_base_,
                              userpc_dcache_locality_npoints_,
                              userpc_dcache_locality_nfeatures_,
                              userpc_dcache_locality_wg_size_,
                              &wg_id);

  ++userpc_perf_.lane_dcache_read_accesses;
  auto seen_line = !userpc_lane_dcache_read_lines_.insert(line).second;
  auto owner_it = userpc_lane_dcache_last_read_owner_.find(line);
  uint32_t locality = UserPCReadLocalityCold;
  bool same_inst = false;
  if (owner_it != userpc_lane_dcache_last_read_owner_.end()) {
    bool same_wg =
        !has_wg_id
        || !owner_it->second.has_wg_id
        || owner_it->second.wg_id == wg_id;
    if (owner_it->second.uuid == trace->uuid) {
      same_inst = true;
    } else if (same_wg && owner_it->second.wid == trace->wid) {
      locality = (owner_it->second.tid == tid)
                   ? UserPCReadLocalityThreadLocal
                   : UserPCReadLocalityIntraWarp;
    } else {
      locality = UserPCReadLocalityInterWarp;
    }
  }

  auto& pending = userpc_lane_dcache_pending_groups_[trace->uuid];
  auto group_it = std::find_if(pending.begin(), pending.end(), [line](const auto& entry) {
    return entry.line == line;
  });
  if (group_it == pending.end()) {
    pending.push_back(UserPCLaneDCachePendingReadGroup{line, UserPCLaneDCacheReadGroup{}});
    group_it = std::prev(pending.end());
  }

  if (same_inst) {
    ++userpc_perf_.lane_dcache_read_same_inst_accesses;
    ++group_it->group.same_inst;
  } else switch (locality) {
  case UserPCReadLocalityThreadLocal:
    ++userpc_perf_.lane_dcache_read_thread_local_accesses;
    ++group_it->group.thread_local_count;
    break;
  case UserPCReadLocalityIntraWarp:
    ++userpc_perf_.lane_dcache_read_intra_warp_accesses;
    ++group_it->group.intra_warp;
    break;
  case UserPCReadLocalityInterWarp:
    ++userpc_perf_.lane_dcache_read_inter_warp_accesses;
    ++group_it->group.inter_warp;
    break;
  default:
    ++userpc_perf_.lane_dcache_read_cold_accesses;
    ++group_it->group.cold;
    break;
  }

  if (seen_line) {
    ++userpc_perf_.lane_dcache_read_reuse_accesses;
    auto last_it = userpc_lane_dcache_last_read_access_.find(line);
    if (last_it != userpc_lane_dcache_last_read_access_.end()) {
      userpc_perf_.lane_dcache_read_reuse_distance_sum +=
          userpc_perf_.lane_dcache_read_accesses - last_it->second;
    }
  }
  userpc_lane_dcache_last_read_access_[line] = userpc_perf_.lane_dcache_read_accesses;
  userpc_lane_dcache_last_read_owner_[line] = UserPCDCacheReadOwner{trace->wid, tid, trace->uuid, wg_id, has_wg_id};
}

void Core::userpc_count_scoreboard(const instr_trace_t* trace, const std::vector<Scoreboard::reg_use_t>& uses) {
  if (!trace->userpc_marked)
    return;
  ++userpc_perf_.candidate_checks;
  ++userpc_perf_.scrb_blocked;
  for (auto& use : uses) {
    switch (use.fu_type) {
    case FUType::ALU: ++userpc_perf_.scrb_alu; break;
    case FUType::FPU: ++userpc_perf_.scrb_fpu; break;
    case FUType::LSU: ++userpc_perf_.scrb_lsu; break;
    case FUType::SFU: {
      ++userpc_perf_.scrb_sfu;
      if (std::get_if<WctlType>(&use.op_type)) {
        ++userpc_perf_.scrb_wctl;
      } else if (std::get_if<CsrType>(&use.op_type)) {
        ++userpc_perf_.scrb_csrs;
      }
    } break;
  #ifdef EXT_V_ENABLE
    case FUType::VPU: ++userpc_perf_.scrb_vpu; break;
  #endif
  #ifdef EXT_TCU_ENABLE
    case FUType::TCU: ++userpc_perf_.scrb_tcu; break;
  #endif
    default: break;
    }
  }
}

void Core::userpc_count_lsu(const instr_trace_t* trace, bool is_write, uint32_t count) {
  if (!trace->userpc_marked)
    return;
  userpc_lsu_uuids_.insert(trace->uuid);
  if (is_write) {
    userpc_perf_.stores += count;
  } else {
    userpc_perf_.loads += count;
  }
}

void Core::userpc_add_load_latency(uint64_t pending_loads) {
  if (!userpc_perf_.enabled)
    return;
  userpc_perf_.load_latency += pending_loads;
}

void Core::userpc_add_dcache_latency(uint64_t pending_reads) {
  if (!userpc_perf_.enabled)
    return;
  userpc_perf_.dcache_read_latency += pending_reads;
}

void Core::dump_cpl_stats() const {
  const uint32_t nw = arch_.num_warps();
  // Per-warp dump: grant count (issue freq), committed, pending, stall, criticality.
  std::cerr << "[CPL_DUMP core=" << core_id_ << "] arbiter=" << configured_issue_arbiter() << "\n";
  std::cerr << "[CPL_DUMP wid] grant_count  committed   pending  stall   crit\n";
  uint64_t g_min = std::numeric_limits<uint64_t>::max(), g_max = 0, g_sum = 0;
  uint64_t c_min = std::numeric_limits<uint64_t>::max(), c_max = 0, c_sum = 0;
  uint32_t alive = 0;
  for (uint32_t wid = 0; wid < nw; ++wid) {
    uint64_t g = dbg_grant_count_.at(wid);
    uint64_t c = cpl_committed_instrs_.at(wid);
    uint64_t p = cpl_inst_pending_.at(wid);
    uint64_t s = cpl_stall_cycles_.at(wid);
    uint32_t iw = wid % ISSUE_WIDTH;
    uint32_t w  = wid / ISSUE_WIDTH;
    uint64_t k = ibuffer_criticality_.at(iw).at(w);
    std::cerr << "[CPL_DUMP "
              << std::setw(3) << wid << "] "
              << std::setw(10) << g << "  "
              << std::setw(10) << c << "  "
              << std::setw(8) << p << "  "
              << std::setw(6) << s << "  "
              << std::setw(10) << k << "\n";
    if (c > 0) {  // only consider warps that actually ran
      ++alive;
      if (g < g_min) g_min = g;
      if (g > g_max) g_max = g;
      g_sum += g;
      if (c < c_min) c_min = c;
      if (c > c_max) c_max = c;
      c_sum += c;
    }
  }
  if (alive > 0) {
    double g_mean = double(g_sum) / alive;
    double c_mean = double(c_sum) / alive;
    double g_skew = (g_mean > 0) ? double(g_max) / g_mean : 0.0;
    double c_skew = (c_mean > 0) ? double(c_max) / c_mean : 0.0;
    std::cerr << "[CPL_DUMP STATS core=" << core_id_ << "] alive=" << alive
              << "  grant: min=" << g_min << " max=" << g_max
              << " mean=" << std::fixed << std::setprecision(1) << g_mean
              << " max/mean=" << std::setprecision(2) << g_skew
              << "  committed: min=" << c_min << " max=" << c_max
              << " mean=" << std::setprecision(1) << c_mean
              << " max/mean=" << std::setprecision(2) << c_skew << "\n";
  }
  // Per-slot arbiter stick/swap ratio
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    uint64_t st = dbg_stick_count_.at(iw);
    uint64_t sw = dbg_swap_count_.at(iw);
    uint64_t total = st + sw;
    double stick_pct = (total > 0) ? (100.0 * st / total) : 0.0;
    std::cerr << "[CPL_DUMP ARB core=" << core_id_ << " slot=" << iw << "] "
              << "stick=" << st << " swap=" << sw
              << " total=" << total
              << " stick%=" << std::fixed << std::setprecision(1) << stick_pct
              << "\n";
  }
  // Per-slot cycle classification: all_empty / scrb_blocked / issued
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    uint64_t ae = dbg_slot_all_empty_.at(iw);
    uint64_t sb = dbg_slot_scrb_block_.at(iw);
    uint64_t is = dbg_slot_issued_.at(iw);
    uint64_t total = ae + sb + is;
    if (total == 0) continue;
    double ae_pct = 100.0 * ae / total;
    double sb_pct = 100.0 * sb / total;
    double is_pct = 100.0 * is / total;
    std::cerr << "[CPL_DUMP SLOT core=" << core_id_ << " slot=" << iw << "] "
              << "all_empty=" << ae << " (" << std::fixed << std::setprecision(1) << ae_pct << "%)"
              << "  scrb_block=" << sb << " (" << sb_pct << "%)"
              << "  issued=" << is << " (" << is_pct << "%)"
              << "  total=" << total << "\n";
  }
  // Per-warp ibuffer empty count (sorted by wid)
  std::cerr << "[CPL_DUMP IBUF_EMPTY core=" << core_id_ << "] per-warp ibuffer empty cycles (issue() checks):\n";
  for (uint32_t wid = 0; wid < nw; ++wid) {
    std::cerr << "[CPL_DUMP IBUF_EMPTY " << std::setw(3) << wid << "] "
              << std::setw(10) << dbg_warp_ibuf_empty_.at(wid) << "\n";
  }
  // Per-warp scrb_block + mean issue-gap.  Asymmetry across warps here is the
  // *prerequisite* for criticality differentiation; if all warps see the same
  // scrb_block & gap, the workload is fundamentally lockstep and no scheduler
  // can produce per-warp differentiation.
  std::cerr << "[CPL_DUMP WARP_DIST core=" << core_id_
            << "] per-wid {scrb_block, mean_issue_gap, committed}:\n";
  uint64_t sb_min = std::numeric_limits<uint64_t>::max(), sb_max = 0;
  long double sb_sum = 0;
  uint32_t alive_w = 0;
  for (uint32_t wid = 0; wid < nw; ++wid) {
    uint64_t sb = dbg_warp_scrb_block_.at(wid);
    uint64_t grants = dbg_grant_count_.at(wid);
    uint64_t stalls = cpl_stall_cycles_.at(wid);
    uint64_t commit = cpl_committed_instrs_.at(wid);
    uint64_t gap = (grants > 0) ? stalls / grants : 0;
    std::cerr << "[CPL_DUMP WARP_DIST " << std::setw(3) << wid << "] "
              << std::setw(10) << sb << "  "
              << std::setw(8) << gap << "  "
              << std::setw(8) << commit << "\n";
    if (commit > 0) {
      sb_sum += sb;
      if (sb < sb_min) sb_min = sb;
      if (sb > sb_max) sb_max = sb;
      ++alive_w;
    }
  }
  if (alive_w > 0) {
    long double sb_mean = sb_sum / alive_w;
    double sb_skew = (sb_mean > 0) ? double(sb_max) / double(sb_mean) : 0.0;
    std::cerr << "[CPL_DUMP WARP_DIST STATS core=" << core_id_ << "] alive=" << alive_w
              << "  scrb_block: min=" << sb_min << " max=" << sb_max
              << " mean=" << std::fixed << std::setprecision(0) << double(sb_mean)
              << " max/mean=" << std::setprecision(3) << sb_skew
              << "\n";
  }
  // Ready-set size histogram per slot — tells us how often the policy
  // actually has a CHOICE.  ready_set size 1 ⇒ no choice, only size ≥ 2
  // matters for differentiation.
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    auto& h = dbg_ready_size_hist_.at(iw);
    uint64_t total = h[0] + h[1] + h[2] + h[3] + h[4];
    if (total == 0) continue;
    auto pct = [&](uint64_t v) {
      return std::to_string(int(100.0 * double(v) / double(total) + 0.5));
    };
    std::cerr << "[CPL_DUMP READY_HIST core=" << core_id_ << " slot=" << iw << "] "
              << "size0=" << h[0] << "(" << pct(h[0]) << "%) "
              << "size1=" << h[1] << "(" << pct(h[1]) << "%) "
              << "size2-3=" << h[2] << "(" << pct(h[2]) << "%) "
              << "size4-7=" << h[3] << "(" << pct(h[3]) << "%) "
              << "size8+=" << h[4] << "(" << pct(h[4]) << "%) "
              << "total=" << total << "\n";
  }
  // RR-vs-current-policy divergence: smoking-gun metric.  If diff% == 0 then
  // the policy reached the exact same decisions as RR for this workload.
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    uint64_t same = dbg_pick_same_as_rr_.at(iw);
    uint64_t diff = dbg_pick_diff_from_rr_.at(iw);
    uint64_t tot = same + diff;
    if (tot == 0) continue;
    std::cerr << "[CPL_DUMP DIVERGE core=" << core_id_ << " slot=" << iw << "] "
              << "same_as_rr=" << same << " diff_from_rr=" << diff
              << " total=" << tot
              << " diff%=" << std::fixed << std::setprecision(2)
              << (100.0 * diff / tot) << "\n";
  }
}

void Core::cpl_snap() const {
  // Periodic per-warp criticality snapshot.  alive = warps with committed>0.
  // spread = max/mean (1.0 ⇒ perfectly flat).  std = stddev across alive warps.
  const uint32_t nw = arch_.num_warps();
  uint64_t mn = std::numeric_limits<uint64_t>::max();
  uint64_t mx = 0;
  long double sum = 0;
  long double sqsum = 0;
  uint32_t alive = 0;
  for (uint32_t wid = 0; wid < nw; ++wid) {
    if (cpl_committed_instrs_.at(wid) == 0) continue;
    uint32_t iw = wid % ISSUE_WIDTH;
    uint32_t w  = wid / ISSUE_WIDTH;
    uint64_t k = ibuffer_criticality_.at(iw).at(w);
    if (k < mn) mn = k;
    if (k > mx) mx = k;
    sum += k;
    sqsum += (long double)k * k;
    ++alive;
  }
  if (alive == 0) return;
  long double mean = sum / alive;
  long double var = sqsum / alive - mean * mean;
  if (var < 0) var = 0;
  long double sd = std::sqrt((double)var);
  double spread = (mean > 0) ? double(mx) / double(mean) : 0.0;
  std::cerr << "[CRIT_SNAP cycle=" << perf_stats_.cycles
            << " core=" << core_id_
            << " alive=" << alive
            << " min=" << mn
            << " max=" << mx
            << " mean=" << std::fixed << std::setprecision(0) << double(mean)
            << " std=" << std::setprecision(0) << double(sd)
            << " spread=" << std::setprecision(3) << spread
            << "]" << std::endl;
}

void Core::dump_userpc_perf() const {
  if (!userpc_perf_.configured)
    return;

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "enabled=" << (userpc_perf_.enabled ? "true" : "false")
            << " pc_base=0x" << std::hex << userpc_perf_.pc_base
            << " pc_from=0x" << userpc_perf_.pc_from
            << " pc_to=0x" << userpc_perf_.pc_to << std::dec << "\n";

  if (!userpc_perf_.enabled)
    return;

  auto span = (userpc_perf_.issued && userpc_perf_.first_cycle <= userpc_perf_.last_cycle)
                ? (userpc_perf_.last_cycle - userpc_perf_.first_cycle + 1)
                : 0;
  auto avg_ready = userpc_perf_.issued ? double(userpc_perf_.ready_sum) / userpc_perf_.issued : 0.0;
  auto avg_candidate = userpc_perf_.issued ? double(userpc_perf_.candidate_sum) / userpc_perf_.issued : 0.0;
  auto issue_density = span ? double(userpc_perf_.issued) / span : 0.0;
  auto not_ready_rate = userpc_perf_.issued ? 100.0 * userpc_perf_.not_ready_fallbacks / userpc_perf_.issued : 0.0;
  auto ready_hit_ratio = userpc_perf_.candidate_checks ? 100.0 * userpc_perf_.ready_checks / userpc_perf_.candidate_checks : 0.0;
  auto scrb_block_ratio = userpc_perf_.candidate_checks ? 100.0 * userpc_perf_.scrb_blocked / userpc_perf_.candidate_checks : 0.0;
  auto idle_cycles = span > userpc_perf_.issue_cycles ? span - userpc_perf_.issue_cycles : 0;
  auto scrb_dep_total = userpc_perf_.scrb_alu + userpc_perf_.scrb_fpu + userpc_perf_.scrb_lsu
                      + userpc_perf_.scrb_sfu + userpc_perf_.scrb_csrs + userpc_perf_.scrb_wctl
                    #ifdef EXT_V_ENABLE
                      + userpc_perf_.scrb_vpu
                    #endif
                    #ifdef EXT_TCU_ENABLE
                      + userpc_perf_.scrb_tcu
                    #endif
                      ;
  auto ifetch_avg_lat = userpc_perf_.ifetches ? userpc_perf_.ifetch_latency / userpc_perf_.ifetches : 0;
  auto load_avg_lat = userpc_perf_.loads ? userpc_perf_.load_latency / userpc_perf_.loads : 0;
  auto dcache_read_avg_lat = userpc_perf_.dcache_reads ? userpc_perf_.dcache_read_latency / userpc_perf_.dcache_reads : 0;
  auto dcache_read_avg_stride = userpc_perf_.dcache_read_stride_count
                                  ? userpc_perf_.dcache_read_stride_sum / userpc_perf_.dcache_read_stride_count
                                  : 0;
  auto dcache_read_avg_stride_capped_4k = userpc_perf_.dcache_read_stride_count
                                            ? userpc_perf_.dcache_read_stride_capped_4k_sum / userpc_perf_.dcache_read_stride_count
                                            : 0;
  auto dcache_read_avg_line_stride = userpc_perf_.dcache_read_line_stride_count
                                      ? userpc_perf_.dcache_read_line_stride_sum / userpc_perf_.dcache_read_line_stride_count
                                      : 0;
  uint64_t dcache_read_unique_tag_sets = userpc_dcache_tags_by_set_.size();
  uint64_t dcache_read_total_unique_tags = 0;
  uint64_t dcache_read_max_unique_tags_per_set = 0;
  uint64_t dcache_read_set_overflow_sets = 0;
  for (auto& entry : userpc_dcache_tags_by_set_) {
    auto tags = entry.second.size();
    dcache_read_total_unique_tags += tags;
    dcache_read_max_unique_tags_per_set = std::max<uint64_t>(dcache_read_max_unique_tags_per_set, tags);
    if (tags > DCACHE_NUM_WAYS)
      ++dcache_read_set_overflow_sets;
  }
  auto dcache_read_avg_unique_tags_per_set = dcache_read_unique_tag_sets
                                               ? dcache_read_total_unique_tags / dcache_read_unique_tag_sets
                                               : 0;
  auto dcache_read_avg_reuse_distance = userpc_perf_.dcache_read_reuse_accesses
                                          ? userpc_perf_.dcache_read_reuse_distance_sum / userpc_perf_.dcache_read_reuse_accesses
                                          : 0;
  auto dcache_read_reuse_hits =
      userpc_perf_.dcache_read_reuse_accesses > userpc_perf_.dcache_read_non_cold_misses
        ? userpc_perf_.dcache_read_reuse_accesses - userpc_perf_.dcache_read_non_cold_misses
        : 0;
  auto dcache_read_thread_local_hits =
      userpc_perf_.dcache_read_locality_thread_local_accesses > userpc_perf_.dcache_read_locality_thread_local_misses
        ? userpc_perf_.dcache_read_locality_thread_local_accesses - userpc_perf_.dcache_read_locality_thread_local_misses
        : 0;
  auto dcache_read_intra_warp_hits =
      userpc_perf_.dcache_read_locality_intra_warp_accesses > userpc_perf_.dcache_read_locality_intra_warp_misses
        ? userpc_perf_.dcache_read_locality_intra_warp_accesses - userpc_perf_.dcache_read_locality_intra_warp_misses
        : 0;
  auto dcache_read_inter_warp_hits =
      userpc_perf_.dcache_read_locality_inter_warp_accesses > userpc_perf_.dcache_read_locality_inter_warp_misses
        ? userpc_perf_.dcache_read_locality_inter_warp_accesses - userpc_perf_.dcache_read_locality_inter_warp_misses
        : 0;
  auto dcache_read_cold_hits =
      userpc_perf_.dcache_read_locality_cold_accesses > userpc_perf_.dcache_read_locality_cold_misses
        ? userpc_perf_.dcache_read_locality_cold_accesses - userpc_perf_.dcache_read_locality_cold_misses
        : 0;
  auto dcache_read_locality_hits =
      dcache_read_cold_hits
    + dcache_read_thread_local_hits
    + dcache_read_intra_warp_hits
    + dcache_read_inter_warp_hits;
  auto dcache_read_non_cold_locality_hits =
      dcache_read_thread_local_hits
    + dcache_read_intra_warp_hits
    + dcache_read_inter_warp_hits;
  auto lane_dcache_read_avg_reuse_distance = userpc_perf_.lane_dcache_read_reuse_accesses
                                             ? userpc_perf_.lane_dcache_read_reuse_distance_sum / userpc_perf_.lane_dcache_read_reuse_accesses
                                             : 0;
  auto lane_dcache_read_non_cold_accesses =
      userpc_perf_.lane_dcache_read_same_inst_accesses
    + userpc_perf_.lane_dcache_read_thread_local_accesses
    + userpc_perf_.lane_dcache_read_intra_warp_accesses
    + userpc_perf_.lane_dcache_read_inter_warp_accesses;
  auto lane_dcache_read_non_cold_hits =
      userpc_perf_.lane_dcache_read_same_inst_hits
    + userpc_perf_.lane_dcache_read_thread_local_hits
    + userpc_perf_.lane_dcache_read_intra_warp_hits
    + userpc_perf_.lane_dcache_read_inter_warp_hits;
  auto lane_dcache_read_same_warp_reuse_accesses =
      userpc_perf_.lane_dcache_read_thread_local_accesses
    + userpc_perf_.lane_dcache_read_intra_warp_accesses;
  auto lane_dcache_read_same_warp_reuse_hits =
      userpc_perf_.lane_dcache_read_thread_local_hits
    + userpc_perf_.lane_dcache_read_intra_warp_hits;
  auto lane_dcache_read_inter_warp_reuse_accesses =
      userpc_perf_.lane_dcache_read_inter_warp_accesses;
  auto lane_dcache_read_inter_warp_reuse_hits =
      userpc_perf_.lane_dcache_read_inter_warp_hits;
  auto lane_dcache_read_l2_same_warp_reuse_hits =
      userpc_perf_.lane_dcache_read_l2_thread_local_hits
    + userpc_perf_.lane_dcache_read_l2_intra_warp_hits;
  auto lane_dcache_read_l2_inter_warp_reuse_hits =
      userpc_perf_.lane_dcache_read_l2_inter_warp_hits;
  auto lane_dcache_read_memory_same_warp_reuse_misses =
      userpc_perf_.lane_dcache_read_memory_thread_local_misses
    + userpc_perf_.lane_dcache_read_memory_intra_warp_misses;
  auto lane_dcache_read_memory_inter_warp_reuse_misses =
      userpc_perf_.lane_dcache_read_memory_inter_warp_misses;
  auto lane_dcache_read_l2_same_warp_reuse_requests =
      lane_dcache_read_l2_same_warp_reuse_hits
    + lane_dcache_read_memory_same_warp_reuse_misses;
  auto lane_dcache_read_l2_thread_local_requests =
      userpc_perf_.lane_dcache_read_l2_thread_local_hits
    + userpc_perf_.lane_dcache_read_memory_thread_local_misses;
  auto lane_dcache_read_l2_intra_warp_requests =
      userpc_perf_.lane_dcache_read_l2_intra_warp_hits
    + userpc_perf_.lane_dcache_read_memory_intra_warp_misses;
  auto lane_dcache_read_l2_inter_warp_requests =
      userpc_perf_.lane_dcache_read_l2_inter_warp_hits
    + userpc_perf_.lane_dcache_read_memory_inter_warp_misses;
  auto dcache_read_locality_accesses =
      userpc_perf_.dcache_read_locality_cold_accesses
    + userpc_perf_.dcache_read_locality_thread_local_accesses
    + userpc_perf_.dcache_read_locality_intra_warp_accesses
    + userpc_perf_.dcache_read_locality_inter_warp_accesses;
  auto ipc = span ? double(userpc_perf_.instrs) / span : 0.0;
  auto streaks = userpc_perf_.same_wid_streaks;
  for (auto current_streak : userpc_perf_.current_wid_streak_by_slot) {
    if (current_streak != 0)
      streaks.push_back(current_streak);
  }
  std::sort(streaks.begin(), streaks.end());
  uint64_t streak_sum = 0;
  for (auto streak : streaks) {
    streak_sum += streak;
  }
  auto streak_percentile = [&streaks](uint32_t percentile) -> uint64_t {
    if (streaks.empty())
      return 0;
    auto index = ((streaks.size() - 1) * percentile + 99) / 100;
    return streaks.at(index);
  };
  auto same_wid_streak_avg = streaks.empty() ? 0.0 : double(streak_sum) / streaks.size();
  auto same_wid_streak_p50 = streak_percentile(50);
  auto same_wid_streak_p90 = streak_percentile(90);
  auto same_wid_streak_max = streaks.empty() ? 0 : streaks.back();
  auto same_wid_issue_rate = userpc_perf_.issue_streak_next_checks
                               ? 100.0 * userpc_perf_.same_wid_consecutive_issues
                                   / userpc_perf_.issue_streak_next_checks
                               : 0.0;
  auto wid_switch_rate = userpc_perf_.issue_streak_next_checks
                           ? 100.0 * userpc_perf_.wid_switches / userpc_perf_.issue_streak_next_checks
                           : 0.0;

  std::cerr << "PERF: userpc pc_from=0x" << std::hex << userpc_perf_.pc_from
            << " pc_to=0x" << userpc_perf_.pc_to << std::dec << "\n";
  std::cerr << "PERF: userpc dcache locality workgroup"
            << " enabled=" << (userpc_dcache_locality_wg_enabled_ ? 1 : 0)
            << " feature_base=0x" << std::hex << userpc_dcache_locality_feature_base_ << std::dec
            << " npoints=" << userpc_dcache_locality_npoints_
            << " nfeatures=" << userpc_dcache_locality_nfeatures_
            << " wg_size=" << userpc_dcache_locality_wg_size_
            << "\n";
  std::cerr << "PERF: userpc scheduler idle=" << idle_cycles
            << " (" << pct_u64(idle_cycles, span) << "%)\n";
  std::cerr << "PERF: userpc scheduler stalls=0 (0%)\n";
  std::cerr << "PERF: userpc ibuffer stalls=" << userpc_perf_.ibuf_stalls
            << " (" << pct_u64(userpc_perf_.ibuf_stalls, span) << "%)\n";
  std::cerr << "PERF: userpc scoreboard stalls=" << userpc_perf_.scrb_stalls
            << " (" << pct_u64(userpc_perf_.scrb_stalls, span) << "%)"
            << " (alu=" << pct_u64(userpc_perf_.scrb_alu, scrb_dep_total) << "%"
            << ", lsu=" << pct_u64(userpc_perf_.scrb_lsu, scrb_dep_total) << "%"
            << ", csrs=" << pct_u64(userpc_perf_.scrb_csrs, scrb_dep_total) << "%"
            << ", wctl=" << pct_u64(userpc_perf_.scrb_wctl, scrb_dep_total) << "%"
            << ", fpu=" << pct_u64(userpc_perf_.scrb_fpu, scrb_dep_total) << "%"
          #ifdef EXT_V_ENABLE
            << ", vpu=" << pct_u64(userpc_perf_.scrb_vpu, scrb_dep_total) << "%"
          #endif
          #ifdef EXT_TCU_ENABLE
            << ", tcu=" << pct_u64(userpc_perf_.scrb_tcu, scrb_dep_total) << "%"
          #endif
            << ")\n";
  std::cerr << "PERF: userpc ready checks=" << userpc_perf_.ready_checks
            << " / candidate checks=" << userpc_perf_.candidate_checks
            << " (hit ratio=" << pct_u64(userpc_perf_.ready_checks, userpc_perf_.candidate_checks) << "%)\n";
  std::cerr << "PERF: userpc issue streak"
            << " count=" << streaks.size()
            << " avg=" << std::fixed << std::setprecision(2) << same_wid_streak_avg
            << " p50=" << same_wid_streak_p50
            << " p90=" << same_wid_streak_p90
            << " max=" << same_wid_streak_max
            << " same_wid_next=" << userpc_perf_.same_wid_consecutive_issues
            << " switches=" << userpc_perf_.wid_switches
            << " next_checks=" << userpc_perf_.issue_streak_next_checks
            << " same_wid_issue_rate=" << same_wid_issue_rate << "%"
            << " wid_switch_rate=" << wid_switch_rate << "%"
            << "\n";
  std::cerr << "PERF: userpc ifetches=" << userpc_perf_.ifetches << "\n";
  std::cerr << "PERF: userpc loads=" << userpc_perf_.loads << "\n";
  std::cerr << "PERF: userpc stores=" << userpc_perf_.stores << "\n";
  std::cerr << "PERF: userpc ifetch latency=" << ifetch_avg_lat << " cycles\n";
  std::cerr << "PERF: userpc load latency=" << load_avg_lat << " cycles\n";
  std::cerr << "PERF: userpc dcache requests=" << (userpc_perf_.dcache_reads + userpc_perf_.dcache_writes)
            << " (reads=" << userpc_perf_.dcache_reads
            << ", writes=" << userpc_perf_.dcache_writes << ")\n";
  std::cerr << "PERF: userpc dcache read misses=" << userpc_perf_.dcache_read_misses
            << " (hit ratio=" << pct_u64(userpc_perf_.dcache_reads - userpc_perf_.dcache_read_misses, userpc_perf_.dcache_reads) << "%)\n";
  std::cerr << "PERF: userpc dcache read levels"
            << " reads=" << userpc_perf_.dcache_reads
            << " l1_hits=" << userpc_perf_.dcache_read_l1_hits
            << " l2_hits=" << userpc_perf_.dcache_read_l2_hits
            << " memory_misses=" << userpc_perf_.dcache_read_memory_misses
            << " l1_hit_rate=" << pct_u64(userpc_perf_.dcache_read_l1_hits, userpc_perf_.dcache_reads) << "%"
            << " l2_hit_rate=" << pct_u64(userpc_perf_.dcache_read_l2_hits, userpc_perf_.dcache_reads) << "%"
            << " memory_miss_rate=" << pct_u64(userpc_perf_.dcache_read_memory_misses, userpc_perf_.dcache_reads) << "%"
            << "\n";
  std::cerr << "PERF: userpc dcache write misses=" << userpc_perf_.dcache_write_misses
            << " (hit ratio=" << pct_u64(userpc_perf_.dcache_writes - userpc_perf_.dcache_write_misses, userpc_perf_.dcache_writes) << "%)\n";
  std::cerr << "PERF: userpc dcache read stride avg=" << dcache_read_avg_stride
            << " capped_4k_avg=" << dcache_read_avg_stride_capped_4k
            << " count=" << userpc_perf_.dcache_read_stride_count
            << " buckets(0=" << userpc_perf_.dcache_read_stride_0
            << ", 1_63=" << userpc_perf_.dcache_read_stride_1_63
            << ", 64_255=" << userpc_perf_.dcache_read_stride_64_255
            << ", 256_1023=" << userpc_perf_.dcache_read_stride_256_1023
            << ", 1k_4k=" << userpc_perf_.dcache_read_stride_1k_4k
            << ", 4k_plus=" << userpc_perf_.dcache_read_stride_4k_plus
            << ")\n";
  std::cerr << "PERF: userpc dcache read line stride avg=" << dcache_read_avg_line_stride
            << " count=" << userpc_perf_.dcache_read_line_stride_count
            << " buckets(same=" << userpc_perf_.dcache_read_line_stride_0
            << ", adjacent=" << userpc_perf_.dcache_read_line_stride_1
            << ", 2_3=" << userpc_perf_.dcache_read_line_stride_2_3
            << ", 4_15=" << userpc_perf_.dcache_read_line_stride_4_15
            << ", 16_63=" << userpc_perf_.dcache_read_line_stride_16_63
            << ", 64_plus=" << userpc_perf_.dcache_read_line_stride_64_plus
            << ")\n";
  std::cerr << "PERF: userpc dcache read temporal unique_lines=" << userpc_dcache_read_lines_.size()
            << " cold_accesses=" << userpc_perf_.dcache_read_cold_accesses
            << " reuse_accesses=" << userpc_perf_.dcache_read_reuse_accesses
            << " avg_reuse_distance=" << dcache_read_avg_reuse_distance
            << " cold_misses=" << userpc_perf_.dcache_read_cold_misses
            << " non_cold_misses=" << userpc_perf_.dcache_read_non_cold_misses
            << " reuse_hit_ratio=" << pct_u64(dcache_read_reuse_hits, userpc_perf_.dcache_read_reuse_accesses)
            << "%\n";
  std::cerr << "PERF: userpc dcache read locality accesses"
            << " cold=" << userpc_perf_.dcache_read_locality_cold_accesses
            << " thread_local=" << userpc_perf_.dcache_read_locality_thread_local_accesses
            << " intra_warp=" << userpc_perf_.dcache_read_locality_intra_warp_accesses
            << " inter_warp=" << userpc_perf_.dcache_read_locality_inter_warp_accesses
            << "\n";
  std::cerr << "PERF: userpc dcache read locality misses"
            << " cold=" << userpc_perf_.dcache_read_locality_cold_misses
            << " thread_local=" << userpc_perf_.dcache_read_locality_thread_local_misses
            << " intra_warp=" << userpc_perf_.dcache_read_locality_intra_warp_misses
            << " inter_warp=" << userpc_perf_.dcache_read_locality_inter_warp_misses
            << "\n";
  std::cerr << "PERF: userpc dcache read locality hits"
            << " cold=" << dcache_read_cold_hits
            << " thread_local=" << dcache_read_thread_local_hits
            << " intra_warp=" << dcache_read_intra_warp_hits
            << " inter_warp=" << dcache_read_inter_warp_hits
            << "\n";
  std::cerr << "PERF: userpc dcache read locality access ratios"
            << " cold=" << pct_u64(userpc_perf_.dcache_read_locality_cold_accesses, dcache_read_locality_accesses) << "%"
            << " thread_local=" << pct_u64(userpc_perf_.dcache_read_locality_thread_local_accesses, dcache_read_locality_accesses) << "%"
            << " intra_warp=" << pct_u64(userpc_perf_.dcache_read_locality_intra_warp_accesses, dcache_read_locality_accesses) << "%"
            << " inter_warp=" << pct_u64(userpc_perf_.dcache_read_locality_inter_warp_accesses, dcache_read_locality_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc dcache read locality hit ratios"
            << " thread_local=" << pct_u64(dcache_read_thread_local_hits, userpc_perf_.dcache_read_locality_thread_local_accesses) << "%"
            << " intra_warp=" << pct_u64(dcache_read_intra_warp_hits, userpc_perf_.dcache_read_locality_intra_warp_accesses) << "%"
            << " inter_warp=" << pct_u64(dcache_read_inter_warp_hits, userpc_perf_.dcache_read_locality_inter_warp_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc dcache read hit composition"
            << " total_hits=" << dcache_read_locality_hits
            << " cold=" << pct_u64(dcache_read_cold_hits, dcache_read_locality_hits) << "%"
            << " thread_local=" << pct_u64(dcache_read_thread_local_hits, dcache_read_locality_hits) << "%"
            << " intra_warp=" << pct_u64(dcache_read_intra_warp_hits, dcache_read_locality_hits) << "%"
            << " inter_warp=" << pct_u64(dcache_read_inter_warp_hits, dcache_read_locality_hits) << "%"
            << "\n";
  std::cerr << "PERF: userpc dcache read non-cold hit composition"
            << " total_hits=" << dcache_read_non_cold_locality_hits
            << " thread_local=" << pct_u64(dcache_read_thread_local_hits, dcache_read_non_cold_locality_hits) << "%"
            << " intra_warp=" << pct_u64(dcache_read_intra_warp_hits, dcache_read_non_cold_locality_hits) << "%"
            << " inter_warp=" << pct_u64(dcache_read_inter_warp_hits, dcache_read_non_cold_locality_hits) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache read locality accesses"
            << " total=" << userpc_perf_.lane_dcache_read_accesses
            << " cold=" << userpc_perf_.lane_dcache_read_cold_accesses
            << " same_inst=" << userpc_perf_.lane_dcache_read_same_inst_accesses
            << " thread_local=" << userpc_perf_.lane_dcache_read_thread_local_accesses
            << " intra_warp_temporal=" << userpc_perf_.lane_dcache_read_intra_warp_accesses
            << " inter_warp=" << userpc_perf_.lane_dcache_read_inter_warp_accesses
            << " reuse=" << userpc_perf_.lane_dcache_read_reuse_accesses
            << " avg_reuse_distance=" << lane_dcache_read_avg_reuse_distance
            << "\n";
  std::cerr << "PERF: userpc lane dcache read locality access ratios"
            << " cold=" << pct_u64(userpc_perf_.lane_dcache_read_cold_accesses, userpc_perf_.lane_dcache_read_accesses) << "%"
            << " same_inst=" << pct_u64(userpc_perf_.lane_dcache_read_same_inst_accesses, userpc_perf_.lane_dcache_read_accesses) << "%"
            << " thread_local=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_accesses, userpc_perf_.lane_dcache_read_accesses) << "%"
            << " intra_warp_temporal=" << pct_u64(userpc_perf_.lane_dcache_read_intra_warp_accesses, userpc_perf_.lane_dcache_read_accesses) << "%"
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_accesses, userpc_perf_.lane_dcache_read_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache read locality hits"
            << " total_hits=" << userpc_perf_.lane_dcache_read_hit_accesses
            << " cold=" << userpc_perf_.lane_dcache_read_cold_hits
            << " same_inst=" << userpc_perf_.lane_dcache_read_same_inst_hits
            << " thread_local=" << userpc_perf_.lane_dcache_read_thread_local_hits
            << " intra_warp_temporal=" << userpc_perf_.lane_dcache_read_intra_warp_hits
            << " inter_warp=" << userpc_perf_.lane_dcache_read_inter_warp_hits
            << "\n";
  std::cerr << "PERF: userpc lane dcache read hit composition"
            << " cold=" << pct_u64(userpc_perf_.lane_dcache_read_cold_hits, userpc_perf_.lane_dcache_read_hit_accesses) << "%"
            << " same_inst=" << pct_u64(userpc_perf_.lane_dcache_read_same_inst_hits, userpc_perf_.lane_dcache_read_hit_accesses) << "%"
            << " thread_local=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_hits, userpc_perf_.lane_dcache_read_hit_accesses) << "%"
            << " intra_warp_temporal=" << pct_u64(userpc_perf_.lane_dcache_read_intra_warp_hits, userpc_perf_.lane_dcache_read_hit_accesses) << "%"
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_hits, userpc_perf_.lane_dcache_read_hit_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache read non-cold hit composition"
            << " total_hits=" << lane_dcache_read_non_cold_hits
            << " same_inst=" << pct_u64(userpc_perf_.lane_dcache_read_same_inst_hits, lane_dcache_read_non_cold_hits) << "%"
            << " thread_local=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_hits, lane_dcache_read_non_cold_hits) << "%"
            << " intra_warp_temporal=" << pct_u64(userpc_perf_.lane_dcache_read_intra_warp_hits, lane_dcache_read_non_cold_hits) << "%"
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_hits, lane_dcache_read_non_cold_hits) << "%"
            << " noncold_hit_rate=" << pct_u64(lane_dcache_read_non_cold_hits, lane_dcache_read_non_cold_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache temporal locality hit composition"
            << " total_hits=" << (userpc_perf_.lane_dcache_read_thread_local_hits
                                + userpc_perf_.lane_dcache_read_intra_warp_hits
                                + userpc_perf_.lane_dcache_read_inter_warp_hits)
            << " same_warp=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_hits
                                       + userpc_perf_.lane_dcache_read_intra_warp_hits,
                                       userpc_perf_.lane_dcache_read_thread_local_hits
                                     + userpc_perf_.lane_dcache_read_intra_warp_hits
                                     + userpc_perf_.lane_dcache_read_inter_warp_hits) << "%"
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_hits,
                                         userpc_perf_.lane_dcache_read_thread_local_hits
                                       + userpc_perf_.lane_dcache_read_intra_warp_hits
                                       + userpc_perf_.lane_dcache_read_inter_warp_hits) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache temporal reuse hit rates"
            << " same_warp_accesses=" << lane_dcache_read_same_warp_reuse_accesses
            << " same_warp_hits=" << lane_dcache_read_same_warp_reuse_hits
            << " same_warp_hit_rate=" << pct_u64(lane_dcache_read_same_warp_reuse_hits,
                                                 lane_dcache_read_same_warp_reuse_accesses) << "%"
            << " inter_warp_accesses=" << lane_dcache_read_inter_warp_reuse_accesses
            << " inter_warp_hits=" << lane_dcache_read_inter_warp_reuse_hits
            << " inter_warp_hit_rate=" << pct_u64(lane_dcache_read_inter_warp_reuse_hits,
                                                  lane_dcache_read_inter_warp_reuse_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache l2 temporal reuse hit rates"
            << " same_warp_accesses=" << lane_dcache_read_same_warp_reuse_accesses
            << " same_warp_hits=" << lane_dcache_read_l2_same_warp_reuse_hits
            << " same_warp_hit_rate=" << pct_u64(lane_dcache_read_l2_same_warp_reuse_hits,
                                                 lane_dcache_read_same_warp_reuse_accesses) << "%"
            << " inter_warp_accesses=" << lane_dcache_read_inter_warp_reuse_accesses
            << " inter_warp_hits=" << lane_dcache_read_l2_inter_warp_reuse_hits
            << " inter_warp_hit_rate=" << pct_u64(lane_dcache_read_l2_inter_warp_reuse_hits,
                                                  lane_dcache_read_inter_warp_reuse_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache l2 temporal locality hit rates"
            << " thread_local_accesses=" << userpc_perf_.lane_dcache_read_thread_local_accesses
            << " thread_local_hits=" << userpc_perf_.lane_dcache_read_l2_thread_local_hits
            << " thread_local_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_thread_local_hits,
                                                    userpc_perf_.lane_dcache_read_thread_local_accesses) << "%"
            << " intra_warp_accesses=" << userpc_perf_.lane_dcache_read_intra_warp_accesses
            << " intra_warp_hits=" << userpc_perf_.lane_dcache_read_l2_intra_warp_hits
            << " intra_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_intra_warp_hits,
                                                  userpc_perf_.lane_dcache_read_intra_warp_accesses) << "%"
            << " inter_warp_accesses=" << userpc_perf_.lane_dcache_read_inter_warp_accesses
            << " inter_warp_hits=" << userpc_perf_.lane_dcache_read_l2_inter_warp_hits
            << " inter_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_inter_warp_hits,
                                                  userpc_perf_.lane_dcache_read_inter_warp_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache l2 temporal locality requests"
            << " l2_same_warp_requests=" << lane_dcache_read_l2_same_warp_reuse_requests
            << " l2_same_warp_misses=" << lane_dcache_read_memory_same_warp_reuse_misses
            << " l2_same_warp_hit_rate=" << pct_u64(lane_dcache_read_l2_same_warp_reuse_hits,
                                                 lane_dcache_read_l2_same_warp_reuse_requests) << "%"
            << " l2_thread_local_requests=" << lane_dcache_read_l2_thread_local_requests
            << " l2_thread_local_misses=" << userpc_perf_.lane_dcache_read_memory_thread_local_misses
            << " l2_thread_local_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_thread_local_hits,
                                                    lane_dcache_read_l2_thread_local_requests) << "%"
            << " l2_intra_warp_requests=" << lane_dcache_read_l2_intra_warp_requests
            << " l2_intra_warp_misses=" << userpc_perf_.lane_dcache_read_memory_intra_warp_misses
            << " l2_intra_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_intra_warp_hits,
                                                  lane_dcache_read_l2_intra_warp_requests) << "%"
            << " l2_inter_warp_requests=" << lane_dcache_read_l2_inter_warp_requests
            << " l2_inter_warp_misses=" << userpc_perf_.lane_dcache_read_memory_inter_warp_misses
            << " l2_inter_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_inter_warp_hits,
                                                  lane_dcache_read_l2_inter_warp_requests) << "%"
            << "\n";
  std::cerr << "PERF: userpc lane dcache memory temporal reuse miss rates"
            << " same_warp_accesses=" << lane_dcache_read_same_warp_reuse_accesses
            << " same_warp_misses=" << lane_dcache_read_memory_same_warp_reuse_misses
            << " same_warp_miss_rate=" << pct_u64(lane_dcache_read_memory_same_warp_reuse_misses,
                                                  lane_dcache_read_same_warp_reuse_accesses) << "%"
            << " inter_warp_accesses=" << lane_dcache_read_inter_warp_reuse_accesses
            << " inter_warp_misses=" << lane_dcache_read_memory_inter_warp_reuse_misses
            << " inter_warp_miss_rate=" << pct_u64(lane_dcache_read_memory_inter_warp_reuse_misses,
                                                   lane_dcache_read_inter_warp_reuse_accesses) << "%"
            << "\n";
  std::cerr << "PERF: userpc dcache read reuse gap buckets(le4=" << userpc_perf_.dcache_read_reuse_gap_le4
            << ", le16=" << userpc_perf_.dcache_read_reuse_gap_le16
            << ", le64=" << userpc_perf_.dcache_read_reuse_gap_le64
            << ", le256=" << userpc_perf_.dcache_read_reuse_gap_le256
            << ", gt256=" << userpc_perf_.dcache_read_reuse_gap_gt256
            << ")\n";
  std::cerr << "PERF: userpc dcache read set pressure sets_touched=" << dcache_read_unique_tag_sets
            << " avg_unique_tags_per_set=" << dcache_read_avg_unique_tags_per_set
            << " max_unique_tags_per_set=" << dcache_read_max_unique_tags_per_set
            << " overflow_sets=" << dcache_read_set_overflow_sets
            << " same_set_tag_changes=" << userpc_perf_.dcache_read_same_set_tag_changes
            << "\n";
  std::cerr << "PERF: userpc dcache read latency=" << dcache_read_avg_lat << " cycles\n";
  std::cerr << "PERF: userpc instrs=" << userpc_perf_.instrs
            << ", cycles=" << span
            << ", IPC=" << std::fixed << std::setprecision(6) << ipc << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "issued=" << userpc_perf_.issued
            << " issue_cycles=" << userpc_perf_.issue_cycles
            << " first_cycle=" << (userpc_perf_.issued ? std::to_string(userpc_perf_.first_cycle) : "n/a")
            << " last_cycle=" << (userpc_perf_.issued ? std::to_string(userpc_perf_.last_cycle) : "n/a")
            << " span=" << span
            << " issue_density=" << std::fixed << std::setprecision(4) << issue_density
            << " avg_ready=" << std::setprecision(2) << avg_ready
            << " avg_candidate=" << std::setprecision(2) << avg_candidate
            << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "candidate_checks=" << userpc_perf_.candidate_checks
            << " ready_checks=" << userpc_perf_.ready_checks
            << " ready_hit_ratio=" << std::fixed << std::setprecision(2) << ready_hit_ratio << "%"
            << " scoreboard_block_ratio=" << std::fixed << std::setprecision(2) << scrb_block_ratio << "%"
            << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "not_ready_fallback=" << userpc_perf_.not_ready_fallbacks
            << " rate=" << std::fixed << std::setprecision(2) << not_ready_rate << "%"
            << " preferred_blocked=" << userpc_perf_.preferred_blocked
            << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "issue_streak"
            << " count=" << streaks.size()
            << " avg=" << std::fixed << std::setprecision(2) << same_wid_streak_avg
            << " p50=" << same_wid_streak_p50
            << " p90=" << same_wid_streak_p90
            << " max=" << same_wid_streak_max
            << " same_wid_next=" << userpc_perf_.same_wid_consecutive_issues
            << " switches=" << userpc_perf_.wid_switches
            << " next_checks=" << userpc_perf_.issue_streak_next_checks
            << " same_wid_issue_rate=" << std::fixed << std::setprecision(2) << same_wid_issue_rate << "%"
            << " wid_switch_rate=" << std::fixed << std::setprecision(2) << wid_switch_rate << "%"
            << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "issues_by_fu"
            << " alu=" << userpc_perf_.alu_issues
            << " fpu=" << userpc_perf_.fpu_issues
            << " lsu=" << userpc_perf_.lsu_issues
            << " sfu=" << userpc_perf_.sfu_issues
          #ifdef EXT_V_ENABLE
            << " vpu=" << userpc_perf_.vpu_issues
          #endif
          #ifdef EXT_TCU_ENABLE
            << " tcu=" << userpc_perf_.tcu_issues
          #endif
            << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "scoreboard_blocked=" << userpc_perf_.scrb_blocked
            << " alu=" << userpc_perf_.scrb_alu
            << " fpu=" << userpc_perf_.scrb_fpu
            << " lsu=" << userpc_perf_.scrb_lsu
            << " sfu=" << userpc_perf_.scrb_sfu
            << " csrs=" << userpc_perf_.scrb_csrs
            << " wctl=" << userpc_perf_.scrb_wctl
          #ifdef EXT_V_ENABLE
            << " vpu=" << userpc_perf_.scrb_vpu
          #endif
          #ifdef EXT_TCU_ENABLE
            << " tcu=" << userpc_perf_.scrb_tcu
          #endif
            << "\n";

  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lsu_ops loads=" << userpc_perf_.loads
            << " stores=" << userpc_perf_.stores << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache"
            << " reads=" << userpc_perf_.dcache_reads
            << " writes=" << userpc_perf_.dcache_writes
            << " read_misses=" << userpc_perf_.dcache_read_misses
            << " write_misses=" << userpc_perf_.dcache_write_misses
            << " read_latency=" << userpc_perf_.dcache_read_latency
            << " avg_read_latency=" << dcache_read_avg_lat
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache_level"
            << " reads=" << userpc_perf_.dcache_reads
            << " l1_hits=" << userpc_perf_.dcache_read_l1_hits
            << " l2_hits=" << userpc_perf_.dcache_read_l2_hits
            << " memory_misses=" << userpc_perf_.dcache_read_memory_misses
            << " l1_hit_rate=" << pct_u64(userpc_perf_.dcache_read_l1_hits, userpc_perf_.dcache_reads)
            << " l2_hit_rate=" << pct_u64(userpc_perf_.dcache_read_l2_hits, userpc_perf_.dcache_reads)
            << " memory_miss_rate=" << pct_u64(userpc_perf_.dcache_read_memory_misses, userpc_perf_.dcache_reads)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache_stride"
            << " avg_read_stride=" << dcache_read_avg_stride
            << " avg_read_stride_capped_4k=" << dcache_read_avg_stride_capped_4k
            << " stride_count=" << userpc_perf_.dcache_read_stride_count
            << " avg_read_line_stride=" << dcache_read_avg_line_stride
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache_temporal"
            << " unique_lines=" << userpc_dcache_read_lines_.size()
            << " cold_misses=" << userpc_perf_.dcache_read_cold_misses
            << " non_cold_misses=" << userpc_perf_.dcache_read_non_cold_misses
            << " reuse_accesses=" << userpc_perf_.dcache_read_reuse_accesses
            << " avg_reuse_distance=" << dcache_read_avg_reuse_distance
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache_locality_hit_ratio"
            << " locality_thread_local_hit_ratio=" << pct_u64(dcache_read_thread_local_hits, userpc_perf_.dcache_read_locality_thread_local_accesses)
            << " locality_intra_warp_hit_ratio=" << pct_u64(dcache_read_intra_warp_hits, userpc_perf_.dcache_read_locality_intra_warp_accesses)
            << " locality_inter_warp_hit_ratio=" << pct_u64(dcache_read_inter_warp_hits, userpc_perf_.dcache_read_locality_inter_warp_accesses)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache_hit_comp"
            << " hit_comp_thread_local=" << pct_u64(dcache_read_thread_local_hits, dcache_read_locality_hits)
            << " hit_comp_intra_warp=" << pct_u64(dcache_read_intra_warp_hits, dcache_read_locality_hits)
            << " hit_comp_inter_warp=" << pct_u64(dcache_read_inter_warp_hits, dcache_read_locality_hits)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "dcache_noncold_hit_comp"
            << " noncold_hit_comp_thread_local=" << pct_u64(dcache_read_thread_local_hits, dcache_read_non_cold_locality_hits)
            << " noncold_hit_comp_intra_warp=" << pct_u64(dcache_read_intra_warp_hits, dcache_read_non_cold_locality_hits)
            << " noncold_hit_comp_inter_warp=" << pct_u64(dcache_read_inter_warp_hits, dcache_read_non_cold_locality_hits)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache"
            << " reads=" << userpc_perf_.lane_dcache_read_accesses
            << " hits=" << userpc_perf_.lane_dcache_read_hit_accesses
            << " reuse_accesses=" << userpc_perf_.lane_dcache_read_reuse_accesses
            << " avg_reuse_distance=" << lane_dcache_read_avg_reuse_distance
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_access_comp"
            << " cold=" << pct_u64(userpc_perf_.lane_dcache_read_cold_accesses, userpc_perf_.lane_dcache_read_accesses)
            << " same_inst=" << pct_u64(userpc_perf_.lane_dcache_read_same_inst_accesses, userpc_perf_.lane_dcache_read_accesses)
            << " thread_local=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_accesses, userpc_perf_.lane_dcache_read_accesses)
            << " intra_warp_temporal=" << pct_u64(userpc_perf_.lane_dcache_read_intra_warp_accesses, userpc_perf_.lane_dcache_read_accesses)
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_accesses, userpc_perf_.lane_dcache_read_accesses)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_hit_comp"
            << " cold=" << pct_u64(userpc_perf_.lane_dcache_read_cold_hits, userpc_perf_.lane_dcache_read_hit_accesses)
            << " same_inst=" << pct_u64(userpc_perf_.lane_dcache_read_same_inst_hits, userpc_perf_.lane_dcache_read_hit_accesses)
            << " thread_local=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_hits, userpc_perf_.lane_dcache_read_hit_accesses)
            << " intra_warp_temporal=" << pct_u64(userpc_perf_.lane_dcache_read_intra_warp_hits, userpc_perf_.lane_dcache_read_hit_accesses)
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_hits, userpc_perf_.lane_dcache_read_hit_accesses)
            << "\n";
  auto lane_temporal_hits =
      userpc_perf_.lane_dcache_read_thread_local_hits
    + userpc_perf_.lane_dcache_read_intra_warp_hits
    + userpc_perf_.lane_dcache_read_inter_warp_hits;
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_temporal_hit_comp"
            << " same_warp=" << pct_u64(userpc_perf_.lane_dcache_read_thread_local_hits
                                      + userpc_perf_.lane_dcache_read_intra_warp_hits,
                                      lane_temporal_hits)
            << " inter_warp=" << pct_u64(userpc_perf_.lane_dcache_read_inter_warp_hits, lane_temporal_hits)
            << " hits=" << lane_temporal_hits
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_temporal_reuse_hit_rate"
            << " same_warp_accesses=" << lane_dcache_read_same_warp_reuse_accesses
            << " same_warp_hits=" << lane_dcache_read_same_warp_reuse_hits
            << " same_warp_hit_rate=" << pct_u64(lane_dcache_read_same_warp_reuse_hits,
                                                 lane_dcache_read_same_warp_reuse_accesses)
            << " inter_warp_accesses=" << lane_dcache_read_inter_warp_reuse_accesses
            << " inter_warp_hits=" << lane_dcache_read_inter_warp_reuse_hits
            << " inter_warp_hit_rate=" << pct_u64(lane_dcache_read_inter_warp_reuse_hits,
                                                  lane_dcache_read_inter_warp_reuse_accesses)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_l2_temporal_reuse_hit_rate"
            << " same_warp_accesses=" << lane_dcache_read_same_warp_reuse_accesses
            << " same_warp_hits=" << lane_dcache_read_l2_same_warp_reuse_hits
            << " same_warp_hit_rate=" << pct_u64(lane_dcache_read_l2_same_warp_reuse_hits,
                                                 lane_dcache_read_same_warp_reuse_accesses)
            << " inter_warp_accesses=" << lane_dcache_read_inter_warp_reuse_accesses
            << " inter_warp_hits=" << lane_dcache_read_l2_inter_warp_reuse_hits
            << " inter_warp_hit_rate=" << pct_u64(lane_dcache_read_l2_inter_warp_reuse_hits,
                                                  lane_dcache_read_inter_warp_reuse_accesses)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_l2_temporal_locality_hit_rate"
            << " thread_local_accesses=" << userpc_perf_.lane_dcache_read_thread_local_accesses
            << " thread_local_hits=" << userpc_perf_.lane_dcache_read_l2_thread_local_hits
            << " thread_local_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_thread_local_hits,
                                                    userpc_perf_.lane_dcache_read_thread_local_accesses)
            << " intra_warp_accesses=" << userpc_perf_.lane_dcache_read_intra_warp_accesses
            << " intra_warp_hits=" << userpc_perf_.lane_dcache_read_l2_intra_warp_hits
            << " intra_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_intra_warp_hits,
                                                  userpc_perf_.lane_dcache_read_intra_warp_accesses)
            << " inter_warp_accesses=" << userpc_perf_.lane_dcache_read_inter_warp_accesses
            << " inter_warp_hits=" << userpc_perf_.lane_dcache_read_l2_inter_warp_hits
            << " inter_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_inter_warp_hits,
                                                  userpc_perf_.lane_dcache_read_inter_warp_accesses)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_l2_temporal_locality_requests"
            << " l2_same_warp_requests=" << lane_dcache_read_l2_same_warp_reuse_requests
            << " l2_same_warp_misses=" << lane_dcache_read_memory_same_warp_reuse_misses
            << " l2_same_warp_hit_rate=" << pct_u64(lane_dcache_read_l2_same_warp_reuse_hits,
                                                 lane_dcache_read_l2_same_warp_reuse_requests)
            << " l2_thread_local_requests=" << lane_dcache_read_l2_thread_local_requests
            << " l2_thread_local_misses=" << userpc_perf_.lane_dcache_read_memory_thread_local_misses
            << " l2_thread_local_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_thread_local_hits,
                                                    lane_dcache_read_l2_thread_local_requests)
            << " l2_intra_warp_requests=" << lane_dcache_read_l2_intra_warp_requests
            << " l2_intra_warp_misses=" << userpc_perf_.lane_dcache_read_memory_intra_warp_misses
            << " l2_intra_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_intra_warp_hits,
                                                  lane_dcache_read_l2_intra_warp_requests)
            << " l2_inter_warp_requests=" << lane_dcache_read_l2_inter_warp_requests
            << " l2_inter_warp_misses=" << userpc_perf_.lane_dcache_read_memory_inter_warp_misses
            << " l2_inter_warp_hit_rate=" << pct_u64(userpc_perf_.lane_dcache_read_l2_inter_warp_hits,
                                                  lane_dcache_read_l2_inter_warp_requests)
            << "\n";
  std::cerr << "[USERPC_PERF core=" << core_id_ << "] "
            << "lane_dcache_memory_temporal_reuse_miss_rate"
            << " same_warp_accesses=" << lane_dcache_read_same_warp_reuse_accesses
            << " same_warp_misses=" << lane_dcache_read_memory_same_warp_reuse_misses
            << " same_warp_miss_rate=" << pct_u64(lane_dcache_read_memory_same_warp_reuse_misses,
                                                  lane_dcache_read_same_warp_reuse_accesses)
            << " inter_warp_accesses=" << lane_dcache_read_inter_warp_reuse_accesses
            << " inter_warp_misses=" << lane_dcache_read_memory_inter_warp_reuse_misses
            << " inter_warp_miss_rate=" << pct_u64(lane_dcache_read_memory_inter_warp_reuse_misses,
                                                   lane_dcache_read_inter_warp_reuse_accesses)
            << "\n";

  std::cerr << "[USERPC_PERF WARP core=" << core_id_ << "] wid issues first_cycle last_cycle\n";
  for (uint32_t wid = 0; wid < userpc_perf_.per_warp_issues.size(); ++wid) {
    auto issues = userpc_perf_.per_warp_issues.at(wid);
    if (issues == 0)
      continue;
    std::cerr << "[USERPC_PERF WARP " << std::setw(3) << wid << "] "
              << std::setw(6) << issues << " "
              << std::setw(10) << userpc_perf_.per_warp_first.at(wid) << " "
              << std::setw(10) << userpc_perf_.per_warp_last.at(wid) << "\n";
  }
}

void Core::reset() {

  emulator_.reset();

  for (auto& commit_arb : commit_arbs_) {
    commit_arb->reset();
  }

  for (auto& ibuf : ibuffers_) {
    ibuf.reset();
  }

  scoreboard_.reset();
  fetch_latch_.reset();
  decode_latch_.reset();
  pending_icache_.clear();

  for (auto& arb : ibuffer_arbs_) {
    arb.reset();
  }
  for (auto& counters : ibuffer_criticality_) {
    std::fill(counters.begin(), counters.end(), 0);
  }
  std::fill(cpl_inst_pending_.begin(), cpl_inst_pending_.end(), 0);
  std::fill(cpl_stall_cycles_.begin(), cpl_stall_cycles_.end(), 0);
  std::fill(cpl_committed_instrs_.begin(), cpl_committed_instrs_.end(), 0);
  std::fill(cpl_last_issue_cycle_.begin(), cpl_last_issue_cycle_.end(), std::numeric_limits<uint64_t>::max());
  std::fill(cpl_arbitration_loss_.begin(), cpl_arbitration_loss_.end(), 0);
  std::fill(sched_criticality_.begin(), sched_criticality_.end(), 0);
  cpl_max_committed_ = 0;

  pending_instrs_.clear();
  pending_ifetches_ = 0;
  pending_userpc_ifetches_ = 0;

  perf_stats_ = PerfStats();
  this->userpc_init();
}

void Core::tick() {
  this->userpc_add_dcache_latency(userpc_perf_.dcache_pending_reads);
  this->commit();
  this->execute();
  this->issue();
  this->decode();
  this->fetch();
  this->schedule();

  ++perf_stats_.cycles;
  // Periodic criticality-distribution snapshot for offline analysis.
  if (env_enabled("VX_CPL_DUMP") && perf_stats_.cycles - dbg_crit_snap_last_cycle_ >= 10000) {
    dbg_crit_snap_last_cycle_ = perf_stats_.cycles;
    this->cpl_snap();
  }
  DPN(2, std::flush);
}

void Core::schedule() {
  auto trace = emulator_.step();
  if (trace == nullptr) {
    ++perf_stats_.sched_idle;
    return;
  }

  // suspend warp until decode
  emulator_.suspend(trace->wid);

  DT(3, "pipeline-schedule: " << *trace);

  // advance to fetch stage
  fetch_latch_.push(trace);
  pending_instrs_.push_back(trace);
}

void Core::fetch() {
  perf_stats_.ifetch_latency += pending_ifetches_;
  userpc_perf_.ifetch_latency += pending_userpc_ifetches_;

  // handle icache response
  auto& icache_rsp_port = icache_rsp_ports.at(0);
  if (!icache_rsp_port.empty()){
    auto& mem_rsp = icache_rsp_port.front();
    auto trace = pending_icache_.at(mem_rsp.tag);
    decode_latch_.push(trace);
    DT(3, "icache-rsp: addr=0x" << std::hex << trace->PC << ", tag=0x" << mem_rsp.tag << std::dec << ", " << *trace);
    pending_icache_.release(mem_rsp.tag);
    icache_rsp_port.pop();
    --pending_ifetches_;
    if (this->userpc_contains(trace->PC)) {
      --pending_userpc_ifetches_;
    }
  }

  // send icache request
  if (fetch_latch_.empty())
    return;
  auto trace = fetch_latch_.front();
  MemReq mem_req;
  mem_req.addr  = trace->PC;
  mem_req.write = false;
  mem_req.tag   = pending_icache_.allocate(trace);
  mem_req.cid   = trace->cid;
  mem_req.uuid  = trace->uuid;
  icache_req_ports.at(0).push(mem_req, SIMX_ICACHE_REQ_LATENCY);
  DT(3, "icache-req: addr=0x" << std::hex << mem_req.addr << ", tag=0x" << mem_req.tag << std::dec << ", " << *trace);
  fetch_latch_.pop();
  ++perf_stats_.ifetches;
  ++pending_ifetches_;
  if (this->userpc_contains(trace->PC)) {
    ++userpc_perf_.ifetches;
    ++pending_userpc_ifetches_;
  }
}

void Core::decode() {
  if (decode_latch_.empty())
    return;

  auto trace = decode_latch_.front();
  this->userpc_mark(trace);

  // check ibuffer capacity
  auto& ibuffer = ibuffers_.at(trace->wid);
  if (ibuffer.full()) {
    if (!trace->log_once(true)) {
      DT(4, "*** ibuffer-stall: " << *trace);
    }
    ++perf_stats_.ibuf_stalls;
    if (trace->userpc_marked) {
      ++userpc_perf_.ibuf_stalls;
    }
    return;
  } else {
    trace->log_once(false);
  }

  // release warp
  if (!trace->fetch_stall) {
    emulator_.resume(trace->wid);
  }

  DT(3, "pipeline-decode: " << *trace);

  // insert to ibuffer
  ibuffer.push(trace);
  if (trace->cpl_inst_delta != 0) {
    cpl_inst_pending_.at(trace->wid) += trace->cpl_inst_delta;
    this->cpl_update_score(trace->wid);
  }

  decode_latch_.pop();
}

void Core::reset_warp_cpl(uint32_t wid) {
  // Clear per-warp CPL accumulators on wspawn (kernel boundary). Per paper
  // Algorithm 1-3 intent ("identify critical warp within current thread
  // block"), state from the previous kernel is not meaningful to the newly
  // re-activated warp that reuses the same wid.
  cpl_inst_pending_.at(wid) = 0;
  cpl_stall_cycles_.at(wid) = 0;
  cpl_committed_instrs_.at(wid) = 0;
  cpl_last_issue_cycle_.at(wid) = std::numeric_limits<uint64_t>::max();
  cpl_arbitration_loss_.at(wid) = 0;
  sched_criticality_.at(wid) = 0;
  uint32_t iw = wid % ISSUE_WIDTH;
  uint32_t w  = wid / ISSUE_WIDTH;
  ibuffer_criticality_.at(iw).at(w) = 0;
  ibuffer_spawn_times_.at(iw).at(w) = SimPlatform::instance().cycles();
}

// VX_NSTALL_MODE selects which counter feeds the nStall term of criticality:
//   0 = gap-since-last-issue (legacy, paper-faithful but includes memory stall)
//   1 = arbitration-loss only (paper §2.2.4 "scheduler-induced" stall, pure)
#ifndef VX_NSTALL_MODE
#define VX_NSTALL_MODE 0
#endif

void Core::cpl_update_score(uint32_t wid) {
  uint32_t iw = wid % ISSUE_WIDTH;
  uint32_t w = wid / ISSUE_WIDTH;
  ibuffer_spawn_times_.at(iw).at(w) = emulator_.get_warp(wid).spawn_time;
  auto committed = cpl_committed_instrs_.at(wid);
  auto elapsed = SimPlatform::instance().cycles() - emulator_.get_warp(wid).spawn_time + 1;
  auto cpi_avg = committed ? std::max<uint64_t>(1, elapsed / committed) : uint64_t(1);
  // New nInst signal: "this warp's lag behind the most-committed warp".
  // Always non-zero when warps progress at different rates; replaces the
  // branch-delta accumulator (cpl_inst_pending_) which collapsed to 0 for
  // most workloads in our Vortex environment.
  uint64_t nInst = (cpl_max_committed_ > committed) ? (cpl_max_committed_ - committed) : 0;
#if VX_NSTALL_MODE == 1
  uint64_t nstall_term = cpl_arbitration_loss_.at(wid);
#else
  uint64_t nstall_term = cpl_stall_cycles_.at(wid);
#endif
  auto crit = nInst * cpi_avg + nstall_term;
  ibuffer_criticality_.at(iw).at(w) = crit;
  sched_criticality_.at(wid) = crit;
}

void Core::issue() {
  // dispatch operands
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    auto& operand = operands_.at(iw);
    if (operand->Output.empty())
      continue;
    auto trace = operand->Output.front();
    dispatchers_.at((int)trace->fu_type)->Inputs.at(iw).push(trace);
    operand->Output.pop();
  }

  // issue ibuffer instructions
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    bool has_instrs = false;
    bool userpc_has_instrs = false;
    bool userpc_ready_any = false;
    BitVector<> candidate_set(PER_ISSUE_WARPS);
    BitVector<> ready_set(PER_ISSUE_WARPS);
    uint64_t candidate_mask = 0;
    uint64_t ready_mask = 0;
    uint64_t ibuffer_empty_mask = 0;
    for (uint32_t w = 0; w < PER_ISSUE_WARPS; ++w) {
      uint32_t wid = w * ISSUE_WIDTH + iw;
      this->cpl_update_score(wid);
      auto& ibuffer = ibuffers_.at(wid);
      if (ibuffer.empty()) {
        ibuffer_empty_mask |= wid_bit(wid);
        ++dbg_warp_ibuf_empty_.at(wid);   // dbg: this warp had no work this cycle
        continue;
      }
      // check scoreboard
      has_instrs = true;
      candidate_set.set(w);
      candidate_mask |= wid_bit(wid);
      auto trace = ibuffer.top();
      if (trace->userpc_marked) {
        userpc_has_instrs = true;
      }
      if (scoreboard_.in_use(trace)) {
        // per-wid scrb_block counter: drives the asymmetry diagnostic
        ++dbg_warp_scrb_block_.at(wid);
        auto uses = scoreboard_.get_uses(trace);
        this->userpc_count_scoreboard(trace, uses);
        if (!trace->log_once(true)) {
          DTH(4, "*** scoreboard-stall: dependents={");
          for (uint32_t j = 0, n = uses.size(); j < n; ++j) {
            auto& use = uses.at(j);
            __unused (use);
            if (j) DTN(4, ", ");
            DTN(4, use.reg_type << use.reg_id << " (#" << use.uuid << ")");
          }
          DTN(4, "}, " << *trace << std::endl);
        }
        for (uint32_t j = 0, n = uses.size(); j < n; ++j) {
          auto& use = uses.at(j);
          switch (use.fu_type) {
          case FUType::ALU: ++perf_stats_.scrb_alu; break;
          case FUType::FPU: ++perf_stats_.scrb_fpu; break;
          case FUType::LSU: ++perf_stats_.scrb_lsu; break;
          case FUType::SFU: {
            ++perf_stats_.scrb_sfu;
            if (std::get_if<WctlType>(&use.op_type)) {
              ++perf_stats_.scrb_wctl;
            } else if (std::get_if<CsrType>(&use.op_type)) {
              ++perf_stats_.scrb_csrs;
            }
          } break;
        #ifdef EXT_V_ENABLE
          case FUType::VPU: ++perf_stats_.scrb_vpu; break;
        #endif
        #ifdef EXT_TCU_ENABLE
          case FUType::TCU: ++perf_stats_.scrb_tcu; break;
        #endif
          default: assert(false);
          }
        }
      } else {
        if (trace->userpc_marked) {
          ++userpc_perf_.candidate_checks;
          ++userpc_perf_.ready_checks;
          userpc_ready_any = true;
        }
        trace->log_once(false);
        ready_set.set(w); // mark instruction as ready
        ready_mask |= wid_bit(wid);
      }
    }
    auto score_vector = trace_score_vector(ibuffer_criticality_.at(iw));

    int preferred_wid = -1;
    if (candidate_set.any()) {
      auto preferred_w = ibuffer_arbs_.at(iw).peek(candidate_set);
      if (preferred_w != uint32_t(-1)) {
        preferred_wid = static_cast<int>(preferred_w * ISSUE_WIDTH + iw);
      }
    }

    int intended_wid = -1;
    if (ready_set.any()) {
      auto intended_w = ibuffer_arbs_.at(iw).peek(ready_set);
      if (intended_w != uint32_t(-1)) {
        intended_wid = static_cast<int>(intended_w * ISSUE_WIDTH + iw);
      }
    }

    if (ready_set.any()) {
      bool strict_preferred_blocked =
        configured_issue_arbiter() == ArbiterType::GTOStrict
        && preferred_wid >= 0
        && (ready_mask & wid_bit(preferred_wid)) == 0;
      if (strict_preferred_blocked) {
        write_warp_sched_trace(core_id_,
                               iw,
                               false,
                               preferred_wid,
                               intended_wid,
                               -1,
                               nullptr,
                               "",
                               score_vector,
                               candidate_mask,
                               ready_mask,
                               ibuffer_empty_mask,
                               true,
                               "preferred_warp_not_ready",
                               "strict_preferred_not_ready",
                               "strict_preferred_not_ready",
                               userpc_has_instrs);
        continue;
      }

      // select one instruction from ready set
      auto w = ibuffer_arbs_.at(iw).grant(ready_set);
      // Per-warp arbitration loss: every warp that was ready but lost the
      // arbiter pick this cycle accumulates +1.  This is the pure
      // scheduler-induced delay (paper §2.2.4) — distinct from memory/HW
      // stall.  Used by VX_NSTALL_MODE=1 in cpl_update_score.
      for (uint32_t other_w = 0; other_w < PER_ISSUE_WARPS; ++other_w) {
        if (other_w == w) continue;
        if (!ready_set.test(other_w)) continue;
        ++cpl_arbitration_loss_.at(other_w * ISSUE_WIDTH + iw);
      }
      // shadow-RR diagnostic: what would a plain RR have picked, given the
      // same ready set?  Cumulative diff% measures the actual policy's
      // behavioural divergence from RR (0% ⇒ policy ≡ RR for this workload).
      auto rr_pick = dbg_shadow_rr_.at(iw).grant(ready_set);
      if (rr_pick == w) {
        ++dbg_pick_same_as_rr_.at(iw);
      } else {
        ++dbg_pick_diff_from_rr_.at(iw);
      }
      uint32_t wid = w * ISSUE_WIDTH + iw;
      // dbg: track per-warp grant frequency and arbiter stick/swap behavior
      ++dbg_grant_count_.at(wid);
      if (w == dbg_last_grant_.at(iw)) {
        ++dbg_stick_count_.at(iw);
      } else {
        ++dbg_swap_count_.at(iw);
        dbg_last_grant_.at(iw) = w;
      }
      auto& ibuffer = ibuffers_.at(wid);
      auto trace = ibuffer.top();
      auto& last_issue_cycle = cpl_last_issue_cycle_.at(wid);
      auto curr_cycle = SimPlatform::instance().cycles();
      if (last_issue_cycle != std::numeric_limits<uint64_t>::max()) {
        cpl_stall_cycles_.at(wid) += curr_cycle - last_issue_cycle - 1;
      }
      last_issue_cycle = curr_cycle;
      this->cpl_update_score(wid);
      auto mismatch_reason = std::string("none");
      if (intended_wid >= 0 && intended_wid != static_cast<int>(wid)) {
        if ((ready_mask & wid_bit(intended_wid)) == 0) {
          mismatch_reason = "intended_warp_not_ready";
        } else {
          mismatch_reason = "fallback_path";
        }
      }
      bool preferred_blocked = false;
      std::string preferred_block_reason = "none";
      if (preferred_wid >= 0 && preferred_wid != static_cast<int>(wid)) {
        if ((ready_mask & wid_bit(preferred_wid)) == 0) {
          preferred_blocked = true;
          preferred_block_reason = "preferred_warp_not_ready";
        }
      }
      if (trace->userpc_marked) {
        auto curr_cycle = SimPlatform::instance().cycles();
        ++userpc_perf_.issued;
        if (userpc_perf_.last_counted_issue_cycle != curr_cycle) {
          ++userpc_perf_.issue_cycles;
          userpc_perf_.last_counted_issue_cycle = curr_cycle;
        }
        userpc_perf_.first_cycle = std::min(userpc_perf_.first_cycle, curr_cycle);
        userpc_perf_.last_cycle = std::max(userpc_perf_.last_cycle, curr_cycle);
        userpc_perf_.candidate_sum += candidate_set.count();
        userpc_perf_.ready_sum += ready_set.count();
        if (preferred_blocked) {
          ++userpc_perf_.not_ready_fallbacks;
          ++userpc_perf_.preferred_blocked;
        }
        if (iw < userpc_perf_.last_issue_wid_by_slot.size()) {
          auto& last_wid = userpc_perf_.last_issue_wid_by_slot.at(iw);
          auto& current_streak = userpc_perf_.current_wid_streak_by_slot.at(iw);
          if (last_wid >= 0) {
            ++userpc_perf_.issue_streak_next_checks;
            if (last_wid == static_cast<int>(wid)) {
              ++userpc_perf_.same_wid_consecutive_issues;
              ++current_streak;
            } else {
              ++userpc_perf_.wid_switches;
              if (current_streak != 0)
                userpc_perf_.same_wid_streaks.push_back(current_streak);
              current_streak = 1;
            }
          } else {
            current_streak = 1;
          }
          last_wid = static_cast<int>(wid);
        }
        if (wid < userpc_perf_.per_warp_issues.size()) {
          ++userpc_perf_.per_warp_issues.at(wid);
          userpc_perf_.per_warp_first.at(wid) = std::min(userpc_perf_.per_warp_first.at(wid), curr_cycle);
          userpc_perf_.per_warp_last.at(wid) = std::max(userpc_perf_.per_warp_last.at(wid), curr_cycle);
        }
        switch (trace->fu_type) {
        case FUType::ALU: ++userpc_perf_.alu_issues; break;
        case FUType::FPU: ++userpc_perf_.fpu_issues; break;
        case FUType::LSU: ++userpc_perf_.lsu_issues; break;
        case FUType::SFU: ++userpc_perf_.sfu_issues; break;
      #ifdef EXT_V_ENABLE
        case FUType::VPU: ++userpc_perf_.vpu_issues; break;
      #endif
      #ifdef EXT_TCU_ENABLE
        case FUType::TCU: ++userpc_perf_.tcu_issues; break;
      #endif
        default: break;
        }
      }
      // update scoreboard
      DT(3, "pipeline-ibuffer: " << *trace);
      if (trace->wb) {
        scoreboard_.reserve(trace);
      }
      write_warp_sched_trace(core_id_,
                             iw,
                             true,
                             preferred_wid,
                             intended_wid,
                             wid,
                             trace,
                             std::to_string(ibuffer_criticality_.at(iw).at(w)),
                             score_vector,
                             candidate_mask,
                             ready_mask,
                             ibuffer_empty_mask,
                             preferred_blocked,
                             preferred_block_reason,
                             mismatch_reason == "none" ? "none" : mismatch_reason,
                             mismatch_reason,
                             trace->userpc_marked);
      // to operand stage
      operands_.at(iw)->Input.push(trace, SIMX_OPERANDS_LATENCY);
      ibuffer.pop();
    } else {
      write_warp_sched_trace(core_id_,
                             iw,
                             false,
                             preferred_wid,
                             intended_wid,
                             -1,
                             nullptr,
                             "",
                             score_vector,
                             candidate_mask,
                             ready_mask,
                             ibuffer_empty_mask,
                             false,
                             "none",
                             has_instrs ? "operand_not_ready" : "ibuffer_empty",
                             "none",
                             userpc_has_instrs);
    }

    // track scoreboard stalls
    if (has_instrs && !ready_set.any()) {
      ++perf_stats_.scrb_stalls;
    }
    if (userpc_has_instrs && !userpc_ready_any) {
      ++userpc_perf_.scrb_stalls;
    }

    // dbg: per-slot classification of this cycle
    if (!has_instrs) {
      ++dbg_slot_all_empty_.at(iw);          // all 16 warps in this slot had empty ibuffer
    } else if (!ready_set.any()) {
      ++dbg_slot_scrb_block_.at(iw);         // had instrs but all blocked by scoreboard
    } else {
      ++dbg_slot_issued_.at(iw);             // grant happened
    }
    // dbg: ready_set size histogram.  bucket 0 = no ready, 1 = exactly 1
    // (policy has no choice), 2 = size 2-3, 3 = size 4-7, 4 = size 8+.
    {
      auto sz = ready_set.count();
      uint32_t bucket;
      if (sz == 0)        bucket = 0;
      else if (sz == 1)   bucket = 1;
      else if (sz <= 3)   bucket = 2;
      else if (sz <= 7)   bucket = 3;
      else                bucket = 4;
      ++dbg_ready_size_hist_.at(iw)[bucket];
    }
  }
}

void Core::execute() {
  for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
    auto& dispatch = dispatchers_.at(fu);
    auto& func_unit = func_units_.at(fu);
    for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
      if (dispatch->Outputs.at(iw).empty())
        continue;
      auto trace = dispatch->Outputs.at(iw).front();
      func_unit->Inputs.at(iw).push(trace, SIMX_DISPATCH_LATENCY);
      dispatch->Outputs.at(iw).pop();
    }
  }
}

void Core::commit() {
  // process completed instructions
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    auto& commit_arb = commit_arbs_.at(iw);
    if (commit_arb->Outputs.at(0).empty())
      continue;
    auto trace = commit_arb->Outputs.at(0).front().data;

    // advance to commit stage
    DT(3, "pipeline-commit: " << *trace);
    assert(trace->cid == core_id_);

    // update scoreboard
    if (trace->eop) {
      if (trace->wb) {
        operands_.at(iw)->writeback(trace);
        scoreboard_.release(trace);
      }
      auto orig_size = pending_instrs_.size();
      pending_instrs_.remove(trace);
      if (pending_instrs_.size() != orig_size) {
        perf_stats_.instrs += trace->tmask.count();
        if (trace->userpc_marked) {
          userpc_perf_.instrs += trace->tmask.count();
        }
        ++cpl_committed_instrs_.at(trace->wid);
        // Maintain running max-committed for the gap-from-leader nInst signal
        // used by cpl_update_score().
        if (cpl_committed_instrs_.at(trace->wid) > cpl_max_committed_) {
          cpl_max_committed_ = cpl_committed_instrs_.at(trace->wid);
        }
        // CAWA pending decrement (paper Algorithm 2: nInst -= 1 per commit).
        // Vortex SIMT extension: while this warp is in a divergent region
        // (ipdom_stack non-empty after a vx_split), freeze the decrement —
        // the other path is still owed, so the warp is not yet "done" with
        // one logical instruction's worth of work. Decrement resumes after
        // the matching vx_join pops the ipdom entry.
        bool warp_divergent = !emulator_.get_warp(trace->wid).ipdom_stack.empty();
        if (!warp_divergent && cpl_inst_pending_.at(trace->wid) != 0) {
          --cpl_inst_pending_.at(trace->wid);
        }
        this->cpl_update_score(trace->wid);
      #ifdef EXT_V_ENABLE
        if (std::get_if<VsetType>(&trace->op_type)
         || std::get_if<VlsType>(&trace->op_type)
         || std::get_if<VopType>(&trace->op_type)) {
          perf_stats_.vinstrs += trace->tmask.count();
        }
      #endif
      }
    }

    // delete the trace
    trace_pool_.deallocate(trace, 1);

    commit_arb->Outputs.at(0).pop();
  }
}

int Core::get_exitcode() const {
  return emulator_.get_exitcode();
}

bool Core::running() const {
  if (emulator_.running() || !pending_instrs_.empty()) {
  #ifndef NDEBUG
    for (auto& trace : pending_instrs_) {
      DT(5, "pipeline-pending: " << *trace);
    }
  #endif
    return true;
  }
  return false;
}

void Core::resume(uint32_t wid) {
  emulator_.resume(wid);
}

bool Core::barrier(uint32_t bar_id, uint32_t count, uint32_t wid) {
  return emulator_.barrier(bar_id, count, wid);
}

bool Core::wspawn(uint32_t num_warps, Word nextPC) {
  return emulator_.wspawn(num_warps, nextPC);
}

void Core::attach_ram(RAM* ram) {
  emulator_.attach_ram(ram);
}

const Core::PerfStats& Core::perf_stats() const {
  perf_stats_.opds_stalls = 0;
  for (uint32_t iw = 0; iw < ISSUE_WIDTH; ++iw) {
    perf_stats_.opds_stalls += operands_.at(iw)->total_stalls();
  }
  return perf_stats_;
}

#ifdef VM_ENABLE
void Core::set_satp(uint64_t satp) {
  emulator_.set_satp(satp); //JAEWON wit, tid???
  // emulator_.set_csr(VX_CSR_SATP,satp,0,0); //JAEWON wit, tid???
}
#endif
