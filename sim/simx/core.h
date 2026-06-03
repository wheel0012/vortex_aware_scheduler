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

#pragma once

#include <vector>
#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <simobject.h>
#include "types.h"
#include "emulator.h"
#include "pipeline.h"
#include "cache_sim.h"
#include "local_mem.h"
#include "ibuffer.h"
#include "scoreboard.h"

#ifdef EXT_V_ENABLE
#include "voperands.h"
#include "vec_unit.h"
#else
#include "operands.h"
#endif

#include "dispatcher.h"
#include "func_unit.h"
#include "mem_coalescer.h"
#include "VX_config.h"

namespace vortex {

class Socket;
class Arch;
class DCRS;

class Core : public SimObject<Core> {
public:
  struct PerfStats {
    uint64_t cycles;
    uint64_t instrs;
    uint64_t sched_idle;
    uint64_t sched_stalls;
    uint64_t ibuf_stalls;
    uint64_t scrb_stalls;
    uint64_t opds_stalls;
    uint64_t scrb_alu;
    uint64_t scrb_fpu;
    uint64_t scrb_lsu;
    uint64_t scrb_sfu;
    uint64_t scrb_csrs;
    uint64_t scrb_wctl;
  #ifdef EXT_V_ENABLE
    uint64_t vinstrs;
    uint64_t scrb_vpu;
  #endif
  #ifdef EXT_TCU_ENABLE
    uint64_t scrb_tcu;
  #endif
    uint64_t ifetches;
    uint64_t loads;
    uint64_t stores;
    uint64_t ifetch_latency;
    uint64_t load_latency;

    PerfStats()
      : cycles(0)
      , instrs(0)
      , sched_idle(0)
      , sched_stalls(0)
      , ibuf_stalls(0)
      , scrb_stalls(0)
      , opds_stalls(0)
      , scrb_alu(0)
      , scrb_fpu(0)
      , scrb_lsu(0)
      , scrb_sfu(0)
      , scrb_csrs(0)
      , scrb_wctl(0)
    #ifdef EXT_V_ENABLE
      , vinstrs(0)
      , scrb_vpu(0)
    #endif
    #ifdef EXT_TCU_ENABLE
      , scrb_tcu(0)
    #endif
      , ifetches(0)
      , loads(0)
      , stores(0)
      , ifetch_latency(0)
      , load_latency(0)
    {}
  };

  std::vector<SimPort<MemReq>> icache_req_ports;
  std::vector<SimPort<MemRsp>> icache_rsp_ports;

  std::vector<SimPort<MemReq>> dcache_req_ports;
  std::vector<SimPort<MemRsp>> dcache_rsp_ports;

  Core(const SimContext& ctx,
       uint32_t core_id,
       Socket* socket,
       const Arch &arch,
       const DCRS &dcrs
  );

  ~Core();

  void reset();

  void tick();

  void attach_ram(RAM* ram);
#ifdef VM_ENABLE
  void set_satp(uint64_t satp);
#endif

  bool running() const;

  void resume(uint32_t wid);

  bool barrier(uint32_t bar_id, uint32_t count, uint32_t wid);

  bool wspawn(uint32_t num_warps, Word nextPC);

  uint32_t id() const {
    return core_id_;
  }

  const Arch& arch() const {
    return arch_;
  }

  Socket* socket() const {
    return socket_;
  }

  const LocalMem::Ptr& local_mem() const {
    return local_mem_;
  }

  const MemCoalescer::Ptr& mem_coalescer(uint32_t idx) const {
    return mem_coalescers_.at(idx);
  }

  void dcache_read(void* data, uint64_t addr, uint32_t size) {
    return emulator_.dcache_read(data, addr, size);
  }

  void dcache_write(const void* data, uint64_t addr, uint32_t size) {
    return emulator_.dcache_write(data, addr, size);
  }

#ifdef EXT_TCU_ENABLE
  TensorUnit::Ptr& tensor_unit() {
    return tensor_unit_;
  }
#endif

#ifdef EXT_V_ENABLE
  VecUnit::Ptr& vec_unit() {
    return vec_unit_;
  }
#endif

  auto& trace_pool() {
    return trace_pool_;
  }

  const PerfStats& perf_stats() const;

  void userpc_count_lsu(const instr_trace_t* trace, bool is_write, uint32_t count);
  void userpc_count_lane_dcache_read(const instr_trace_t* trace, uint32_t tid, uint64_t addr);
  void userpc_add_load_latency(uint64_t pending_loads);
  void userpc_add_dcache_latency(uint64_t pending_reads);

  int get_exitcode() const;

  // Reset per-warp CPL state at kernel-launch (wspawn) boundary.
  // Without this, cumulative criticality from prior kernel leaks into the
  // newly-spawned warp that reuses the same wid — leads to misleading
  // priority decisions and warp 0 cpi_avg overflow.
  void reset_warp_cpl(uint32_t wid);

  // Clear the running max-committed value at a kernel boundary.  Called
  // from emulator.cpp's wspawn handler AFTER all warps have been reset, so
  // the gap signal restarts from zero each kernel launch.
  void reset_max_committed() { cpl_max_committed_ = 0; }

  // Read-only view of per-wid criticality for the schedule-stage policy.
  // Updated in cpl_update_score() whenever the issue stage refreshes the
  // per-slot criticality vector.
  const std::vector<uint64_t>& sched_criticality() const {
    return sched_criticality_;
  }

private:

  void schedule();
  void fetch();
  void decode();
  void issue();
  void execute();
  void commit();
  void cpl_update_score(uint32_t wid);
  void userpc_init();
  void userpc_mark(instr_trace_t* trace) const;
  bool userpc_contains(uint64_t pc) const;
  void userpc_count_scoreboard(const instr_trace_t* trace, const std::vector<Scoreboard::reg_use_t>& uses);
  void dump_userpc_perf() const;

  struct UserPCPerfStats {
    bool configured;
    bool enabled;
    uint64_t pc_base;
    uint64_t pc_from;
    uint64_t pc_to;
    uint64_t first_cycle;
    uint64_t last_cycle;
    uint64_t last_counted_issue_cycle;
    uint64_t issued;
    uint64_t instrs;
    uint64_t ifetches;
    uint64_t ifetch_latency;
    uint64_t issue_cycles;
    uint64_t candidate_checks;
    uint64_t ready_checks;
    uint64_t candidate_sum;
    uint64_t ready_sum;
    uint64_t not_ready_fallbacks;
    uint64_t preferred_blocked;
    uint64_t issue_streak_next_checks;
    uint64_t same_wid_consecutive_issues;
    uint64_t wid_switches;
    uint64_t ibuf_stalls;
    uint64_t scrb_stalls;
    uint64_t scrb_blocked;
    uint64_t scrb_alu;
    uint64_t scrb_fpu;
    uint64_t scrb_lsu;
    uint64_t scrb_sfu;
    uint64_t scrb_csrs;
    uint64_t scrb_wctl;
  #ifdef EXT_V_ENABLE
    uint64_t scrb_vpu;
  #endif
  #ifdef EXT_TCU_ENABLE
    uint64_t scrb_tcu;
  #endif
    uint64_t loads;
    uint64_t stores;
    uint64_t load_latency;
    uint64_t dcache_reads;
    uint64_t dcache_writes;
    uint64_t dcache_read_misses;
    uint64_t dcache_write_misses;
    uint64_t dcache_read_latency;
    uint64_t dcache_pending_reads;
    bool dcache_stride_valid;
    uint64_t dcache_last_read_addr;
    uint64_t dcache_read_stride_sum;
    uint64_t dcache_read_stride_capped_4k_sum;
    uint64_t dcache_read_stride_count;
    uint64_t dcache_read_stride_0;
    uint64_t dcache_read_stride_1_63;
    uint64_t dcache_read_stride_64_255;
    uint64_t dcache_read_stride_256_1023;
    uint64_t dcache_read_stride_1k_4k;
    uint64_t dcache_read_stride_4k_plus;
    uint64_t dcache_read_cold_accesses;
    uint64_t dcache_read_reuse_accesses;
    uint64_t dcache_read_cold_misses;
    uint64_t dcache_read_non_cold_misses;
    uint64_t dcache_read_access_index;
    uint64_t dcache_read_reuse_distance_sum;
    uint64_t dcache_read_reuse_gap_le4;
    uint64_t dcache_read_reuse_gap_le16;
    uint64_t dcache_read_reuse_gap_le64;
    uint64_t dcache_read_reuse_gap_le256;
    uint64_t dcache_read_reuse_gap_gt256;
    uint64_t dcache_read_locality_cold_accesses;
    uint64_t dcache_read_locality_thread_local_accesses;
    uint64_t dcache_read_locality_intra_warp_accesses;
    uint64_t dcache_read_locality_inter_warp_accesses;
    uint64_t dcache_read_locality_cold_misses;
    uint64_t dcache_read_locality_thread_local_misses;
    uint64_t dcache_read_locality_intra_warp_misses;
    uint64_t dcache_read_locality_inter_warp_misses;
    uint64_t lane_dcache_read_accesses;
    uint64_t lane_dcache_read_cold_accesses;
    uint64_t lane_dcache_read_same_inst_accesses;
    uint64_t lane_dcache_read_thread_local_accesses;
    uint64_t lane_dcache_read_intra_warp_accesses;
    uint64_t lane_dcache_read_inter_warp_accesses;
    uint64_t lane_dcache_read_hit_accesses;
    uint64_t lane_dcache_read_cold_hits;
    uint64_t lane_dcache_read_same_inst_hits;
    uint64_t lane_dcache_read_thread_local_hits;
    uint64_t lane_dcache_read_intra_warp_hits;
    uint64_t lane_dcache_read_inter_warp_hits;
    uint64_t lane_dcache_read_reuse_accesses;
    uint64_t lane_dcache_read_reuse_distance_sum;
    bool dcache_line_stride_valid;
    uint64_t dcache_last_read_line;
    uint64_t dcache_read_line_stride_sum;
    uint64_t dcache_read_line_stride_count;
    uint64_t dcache_read_line_stride_0;
    uint64_t dcache_read_line_stride_1;
    uint64_t dcache_read_line_stride_2_3;
    uint64_t dcache_read_line_stride_4_15;
    uint64_t dcache_read_line_stride_16_63;
    uint64_t dcache_read_line_stride_64_plus;
    bool dcache_set_valid;
    uint64_t dcache_last_read_set;
    uint64_t dcache_last_read_tag;
    uint64_t dcache_read_same_set_tag_changes;
    uint64_t alu_issues;
    uint64_t fpu_issues;
    uint64_t lsu_issues;
    uint64_t sfu_issues;
  #ifdef EXT_V_ENABLE
    uint64_t vpu_issues;
  #endif
  #ifdef EXT_TCU_ENABLE
    uint64_t tcu_issues;
  #endif
    std::vector<uint64_t> per_warp_issues;
    std::vector<uint64_t> per_warp_first;
    std::vector<uint64_t> per_warp_last;
    std::vector<int> last_issue_wid_by_slot;
    std::vector<uint64_t> current_wid_streak_by_slot;
    std::vector<uint64_t> same_wid_streaks;

    UserPCPerfStats();
  };

  struct UserPCDCacheReadOwner {
    uint32_t wid;
    uint32_t tid;
    uint64_t uuid;
    uint64_t wg_id;
    bool has_wg_id;
  };

  struct UserPCLaneDCacheReadGroup {
    uint64_t cold;
    uint64_t same_inst;
    uint64_t thread_local_count;
    uint64_t intra_warp;
    uint64_t inter_warp;

    UserPCLaneDCacheReadGroup()
      : cold(0)
      , same_inst(0)
      , thread_local_count(0)
      , intra_warp(0)
      , inter_warp(0)
    {}

    uint64_t total() const {
      return cold + same_inst + thread_local_count + intra_warp + inter_warp;
    }
  };

  struct UserPCLaneDCachePendingReadGroup {
    uint64_t line;
    UserPCLaneDCacheReadGroup group;
  };

  uint32_t core_id_;
  Socket* socket_;
  const Arch& arch_;

#ifdef EXT_TCU_ENABLE
  TensorUnit::Ptr tensor_unit_;
#endif

#ifdef EXT_V_ENABLE
  VecUnit::Ptr vec_unit_;
#endif

  // Per-wid flat criticality mirror — must be declared (and constructed)
  // BEFORE emulator_, because Emulator's sched_policy_ Arbiter holds a
  // pointer to this vector and asserts its size at ctor time.
  std::vector<uint64_t> sched_criticality_;

  Emulator emulator_;

  std::vector<IBuffer> ibuffers_;
  Scoreboard scoreboard_;
  std::vector<Operands::Ptr> operands_;
  std::vector<Dispatcher::Ptr> dispatchers_;
  std::vector<FuncUnit::Ptr> func_units_;
  LocalMem::Ptr local_mem_;
  std::vector<LocalMemSwitch::Ptr> lmem_switch_;
  std::vector<MemCoalescer::Ptr> mem_coalescers_;

  PipelineLatch fetch_latch_;
  PipelineLatch decode_latch_;

  HashTable<instr_trace_t*> pending_icache_;
  std::list<instr_trace_t*, PoolAllocator<instr_trace_t*, 64>> pending_instrs_;

  uint64_t pending_ifetches_;
  uint64_t pending_userpc_ifetches_;

  mutable PerfStats perf_stats_;
  UserPCPerfStats userpc_perf_;
  std::unordered_set<uint64_t> userpc_lsu_uuids_;
  std::unordered_set<uint64_t> userpc_dcache_read_lines_;
  std::unordered_map<uint64_t, uint64_t> userpc_dcache_last_read_access_;
  std::unordered_map<uint64_t, std::deque<bool>> userpc_dcache_pending_read_cold_;
  std::unordered_map<uint64_t, std::deque<uint32_t>> userpc_dcache_pending_read_locality_;
  std::unordered_map<uint64_t, UserPCDCacheReadOwner> userpc_dcache_last_read_owner_;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> userpc_dcache_tags_by_set_;
  std::unordered_set<uint64_t> userpc_lane_dcache_read_lines_;
  std::unordered_map<uint64_t, uint64_t> userpc_lane_dcache_last_read_access_;
  std::unordered_map<uint64_t, UserPCDCacheReadOwner> userpc_lane_dcache_last_read_owner_;
  std::unordered_map<uint64_t, std::deque<UserPCLaneDCachePendingReadGroup>> userpc_lane_dcache_pending_groups_;
  std::unordered_map<uint64_t, std::deque<UserPCLaneDCacheReadGroup>> userpc_lane_dcache_rsp_groups_;
  bool userpc_dcache_locality_wg_enabled_;
  uint64_t userpc_dcache_locality_feature_base_;
  uint64_t userpc_dcache_locality_npoints_;
  uint64_t userpc_dcache_locality_nfeatures_;
  uint64_t userpc_dcache_locality_wg_size_;

  std::vector<TraceArbiter::Ptr> commit_arbs_;

  uint32_t commit_exe_;
  std::vector<std::vector<uint64_t>> ibuffer_spawn_times_;
  std::vector<std::vector<uint64_t>> ibuffer_criticality_;
  // Per-slot per-warp-in-slot block id (= global_wid / WSPAWN_WARPS_PER_BLOCK).
  // Constant after construction; consumed by GCAWSArbiter to keep criticality
  // comparison within the same thread block (paper CAWA intent).
  std::vector<std::vector<uint64_t>> ibuffer_block_ids_;
  std::vector<Arbiter> ibuffer_arbs_;
  std::vector<uint64_t> cpl_inst_pending_;
  std::vector<uint64_t> cpl_stall_cycles_;
  std::vector<uint64_t> cpl_committed_instrs_;
  std::vector<uint64_t> cpl_last_issue_cycle_;
  // Per-warp arbitration-loss counter: per cycle, every warp that was in
  // ready_set but NOT picked by the arbiter gets +1.  This isolates the
  // *scheduler-actionable* portion of "stall between consecutive issues"
  // — i.e., the paper §2.2.4 "RR adds 52% additional wait" component —
  // separately from memory/fetch/HW stalls that the scheduler cannot fix.
  std::vector<uint64_t> cpl_arbitration_loss_;
  // Running max of cpl_committed_instrs_, updated in commit() and reset at
  // kernel-launch (wspawn).  Drives the nInst = max_committed - this warp's
  // committed signal used by cpl_update_score (gap-from-leader heuristic
  // replacing the paper-Algorithm-2 branch-delta accumulator).
  uint64_t cpl_max_committed_;
  // sched_criticality_ is declared earlier in this class (before emulator_)
  // so the schedule-stage policy can reference it during Emulator
  // construction; it's a per-wid mirror of ibuffer_criticality_ updated in
  // cpl_update_score().

  // Debug counters for diagnosing scheduler behavior.
  std::vector<uint64_t> dbg_grant_count_;        // per-wid: how often each warp was granted issue
  std::vector<uint32_t> dbg_last_grant_;         // per-slot: last grant index (for stick/swap)
  std::vector<uint64_t> dbg_stick_count_;        // per-slot: arbiter returned same warp as last cycle
  std::vector<uint64_t> dbg_swap_count_;         // per-slot: arbiter switched to a different warp
  std::vector<uint64_t> dbg_warp_ibuf_empty_;    // per-wid: count of issue() checks where this warp's ibuffer was empty
  std::vector<uint64_t> dbg_slot_all_empty_;     // per-slot: cycles where ALL warps in slot had empty ibuffer (no work)
  std::vector<uint64_t> dbg_slot_scrb_block_;    // per-slot: cycles where ibuffer had instrs but all scoreboard-blocked
  std::vector<uint64_t> dbg_slot_issued_;        // per-slot: cycles where actual issue happened
  std::vector<uint64_t> dbg_warp_scrb_block_;    // per-wid: cycles where this warp's head-of-ibuf was scoreboard-blocked
  // ready_set size histogram per slot: hist[0]=empty, [1]=size-1, [2]=size 2-3,
  // [3]=size 4-7, [4]=size 8+.  Size=1 means policy has no choice this cycle.
  std::vector<std::array<uint64_t, 5>> dbg_ready_size_hist_;

  // RR-vs-GCAWS divergence diagnostic: a shadow RR arbiter per slot is fed the
  // same ready_set every issue cycle; if its grant matches the real arbiter's
  // grant, the real policy made the same decision as RR would.  Cumulative
  // diff%=0 ⇒ the policy is behaviourally identical to RR for this workload.
  std::vector<Arbiter> dbg_shadow_rr_;
  std::vector<uint64_t> dbg_pick_same_as_rr_;    // per-slot
  std::vector<uint64_t> dbg_pick_diff_from_rr_;  // per-slot
  uint64_t dbg_crit_snap_last_cycle_;            // last CRIT_SNAP emit
  void dump_cpl_stats() const;
  void cpl_snap() const;

  PoolAllocator<instr_trace_t, 64> trace_pool_;

  friend class LsuUnit;
  friend class AluUnit;
  friend class FpuUnit;
  friend class SfuUnit;
};

} // namespace vortex
