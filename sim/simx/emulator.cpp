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
#include <stdlib.h>
#include <unistd.h>
#include <cmath>
#include <algorithm>
#include <limits>
#include <assert.h>
#include <util.h>

#include "emulator.h"
#include "instr_trace.h"
#include "instr.h"
#include "dcrs.h"
#include "core.h"
#include "socket.h"
#include "cluster.h"
#include "processor_impl.h"
#include "local_mem.h"

using namespace vortex;

// iPAWS tuning macros hoisted above the Emulator destructor so its
// diagnostic printout can reference them.
// ADAPT phase 길이 (cycle). 너무 짧으면 분포 신호 약하고, 길면 overhead.
#ifndef VORTEX_IPAWS_ADAPT_CYCLES
#define VORTEX_IPAWS_ADAPT_CYCLES   4096
#endif
#ifndef VORTEX_IPAWS_WOI_RATIO
#define VORTEX_IPAWS_WOI_RATIO      0.5
#endif
#ifndef VORTEX_IPAWS_CONCAVE_TH
#define VORTEX_IPAWS_CONCAVE_TH     0.4
#endif
#ifndef VORTEX_IPAWS_USE_CACP
#define VORTEX_IPAWS_USE_CACP       1
#endif
// 1 이면 iPAWS Adapt 가 wspawn (= kernel launch) 시점에만 트리거되고
// Execute phase 는 다음 wspawn 까지 무한 지속. 0 이면 기존 periodic
// (ADAPT_CYCLES + EXECUTE_CYCLES) 동작.
// Change 3: btime = barrier-only. 1 이면 adapt_stall 이 barrier wait 만
// 카운트 (논문 §3.3 strict). 0 이면 active && !chosen 모두 카운트 (이전).
#ifndef VORTEX_IPAWS_BARRIER_ONLY_BTIME
#define VORTEX_IPAWS_BARRIER_ONLY_BTIME 1
#endif
// Change 4: Recover phase. 1 이면 Convex(RR) 결정 후 Recover phase 실행
// (least-instr-issued warp 우선). 0 이면 Adapt 직후 Execute(RR).
// 기본 0 — Vortex memory latency 특성상 strict 종료 조건 도달 불가능,
// cycle cap 으로만 동작하여 사실상 무용지물 (urp_analysis.md 참고).
#ifndef VORTEX_IPAWS_USE_RECOVER
#define VORTEX_IPAWS_USE_RECOVER 0
#endif
// Recover phase 종료 임계 (max-min instr_count <= 이면 종료). 너무 작으면
// 영원히 끝 안날 수 있으니 cycle cap 도 함께 사용.
#ifndef VORTEX_IPAWS_RECOVER_THRESHOLD
#define VORTEX_IPAWS_RECOVER_THRESHOLD 4
#endif
#ifndef VORTEX_IPAWS_RECOVER_MAX_CYCLES
#define VORTEX_IPAWS_RECOVER_MAX_CYCLES 8192
#endif
// CPL 스냅샷 로깅 주기 (cycle). 0이면 비활성화.
// 예: -DVORTEX_CPL_LOG_INTERVAL=4096
#ifndef VORTEX_CPL_LOG_INTERVAL
#define VORTEX_CPL_LOG_INTERVAL 0
#endif

namespace {

// Choose one schedule policy here for simx:
// VORTEX_SCHED build define selects the default scheduler:
//   0 = Static, 1 = GTO, 2 = RR, 3 = gCAWS, 4 = iPAWS  (default = iPAWS)
#ifndef VORTEX_SCHED
#define VORTEX_SCHED 4
#endif

constexpr WarpSchedulePolicy kSchedTable[] = {
  WarpSchedulePolicy::Static,
  WarpSchedulePolicy::GTO,
  WarpSchedulePolicy::RR,
  WarpSchedulePolicy::gCAWS,
  WarpSchedulePolicy::iPAWS
};
constexpr WarpSchedulePolicy kDefaultSchedulePolicy = kSchedTable[VORTEX_SCHED];

} // namespace

warp_t::warp_t(uint32_t num_threads)
  : ireg_file(MAX_NUM_REGS, std::vector<Word>(num_threads))
  , freg_file(MAX_NUM_REGS, std::vector<uint64_t>(num_threads))
  , tmask(num_threads)
  , PC(0)
  , uuid(0)
{}

void warp_t::reset(uint64_t startup_addr) {
  this->tmask.reset();
  this->PC = startup_addr;
  this->uuid = 0;
  this->fcsr = 0;

  for (auto& reg_file : this->ireg_file) {
    for (auto& reg : reg_file) {
    #ifndef NDEBUG
      reg = 0;
    #else
      reg = std::rand();
    #endif
    }
  }

  // set x0 to zero
  for (auto& reg : this->ireg_file.at(0)) {
    reg = 0;
  }

  for (auto& reg_file : this->freg_file) {
    for (auto& reg : reg_file) {
    #ifndef NDEBUG
      reg = 0;
    #else
      reg = std::rand();
    #endif
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

Emulator::Emulator(const Arch &arch, const DCRS &dcrs, Core* core) //생성자
    : arch_(arch)
    , dcrs_(dcrs)
    , core_(core)
    , warps_(arch.num_warps(), arch.num_threads())
    , schedule_policy_(kDefaultSchedulePolicy)
    , schedule_cycle_(0)
    , kernel_id_(0)
    , greedy_warp_(-1) //gto 의 직전 warp 없음( 처음 reset 이라서)
    , rr_last_warp_(-1) //마찬가지
    , critical_warp_(-1) //마찬가지
    , last_scheduled_warp_(-1)
    , ready_timestamps_(arch.num_warps(), 0)
    , warp_cpl_(arch.num_warps())
    , suppress_critical_push_(false)
    , barriers_(arch.num_barriers(), 0)
    , ipdom_size_(arch.num_threads()-1)
  #ifdef EXT_TCU_ENABLE
    , tensor_unit_(core->tensor_unit())
  #endif
  #ifdef EXT_V_ENABLE
    , vec_unit_(core->vec_unit())
  #endif
{
  std::srand(50);
  this->reset(); 
}

Emulator::~Emulator() { //소멸자
  this->cout_flush(); //프로그램이 출력하던 stdout을 마저 비운다. 
  if (schedule_policy_ == WarpSchedulePolicy::iPAWS) { //ipaws 정책일때만 통계 출력
    auto& s = ipaws_state_;
    double avg_meanmax = (s.decides_valid > 0)
                         ? (s.decide_meanmax_accum / s.decides_valid)
                         : 0.0;
    double min_meanmax = (s.decides_valid > 0) ? s.decide_meanmax_min : 0.0;
    double max_meanmax = (s.decides_valid > 0) ? s.decide_meanmax_max : 0.0;
    double avg_woi = (s.decides_valid > 0)
                     ? (static_cast<double>(s.woi_size_accum) / s.decides_valid)
                     : 0.0;
    uint32_t woi_min = (s.decides_valid > 0) ? s.woi_size_min : 0;
    uint32_t woi_max = (s.decides_valid > 0) ? s.woi_size_max : 0;
    // DEBUG: per-warp permanent stall counter dump.
    std::cerr << "IPAWS_DBG_STALL:";
    for (size_t i = 0; i < s.issue_stall_count.size(); ++i) {
      std::cerr << " " << i << "=" << s.issue_stall_count[i];
    }
    std::cerr << std::endl;
    std::cerr << "IPAWS_STATS: decides=" << s.decides_total
              << " valid=" << s.decides_valid
              << " skipped=" << s.decides_skipped
              << " woi_fallback=" << s.decides_woi_fallback
              << " concave=" << s.decides_concave
              << " convex=" << s.decides_convex
              << " mm_avg=" << std::fixed << std::setprecision(3) << avg_meanmax
              << " mm_min=" << min_meanmax
              << " mm_max=" << max_meanmax
              << " woi_avg=" << avg_woi
              << " woi_min=" << woi_min
              << " woi_max=" << woi_max
              << " gcaws_exec_cycles=" << s.gcaws_exec_cycles
              << " rr_exec_cycles=" << s.rr_exec_cycles
              << " wspawn_events=" << s.wspawn_events
              << " recover_entries=" << s.recover_entries
              << " recover_cycles=" << s.recover_cycles
              << " test=mean/max<0.5"
              << " use_cacp=" << (VORTEX_IPAWS_USE_CACP ? 1 : 0)
              << " barrier_only_btime=" << (VORTEX_IPAWS_BARRIER_ONLY_BTIME ? 1 : 0)
              << " use_recover=" << (VORTEX_IPAWS_USE_RECOVER ? 1 : 0)
              << std::endl;
  }
}

void Emulator::reset() { // 시뮬 재시작시 호출 + 객체 만들때 호출 
  uint64_t startup_addr = dcrs_.base_dcrs.read(VX_DCR_BASE_STARTUP_ADDR0);
#if (XLEN == 64)
  startup_addr |= (uint64_t(dcrs_.base_dcrs.read(VX_DCR_BASE_STARTUP_ADDR1)) << 32);
#endif

  uint64_t startup_arg = dcrs_.base_dcrs.read(VX_DCR_BASE_STARTUP_ARG0);
#if (XLEN == 64)
  startup_arg |= (uint64_t(dcrs_.base_dcrs.read(VX_DCR_BASE_STARTUP_ARG1)) << 32);
#endif

  for (auto& warp : warps_) {
    warp.reset(startup_addr);
  }

  for (auto& barrier : barriers_) {
    barrier.reset();
  }

#ifdef EXT_V_ENABLE
  vec_unit_->reset();
#endif

  csr_mscratch_ = startup_arg;

  stalled_warps_.reset(); // _ : emulator class안의 멤버변수를 표현하는 것임.
  active_warps_.reset();
  schedule_cycle_ = 0;
  ++kernel_id_;
  greedy_warp_ = -1;
  rr_last_warp_ = -1;
  critical_warp_ = -1; //gcaws 직전 선택 초기화
  last_scheduled_warp_ = -1;
  std::fill(ready_timestamps_.begin(), ready_timestamps_.end(), 0);

  //gcaws 카운터 리셋
  for (auto& cpl : warp_cpl_) {
    cpl.instr_count = 0;
    cpl.stall_cycles = 0;
    cpl.criticality = 0;
  }
  //ipaws 상태 초기화.
  ipaws_state_ = ipaws_state_t();
  ipaws_state_.adapt_issue.assign(arch_.num_warps(), 0);
  ipaws_state_.adapt_stall.assign(arch_.num_warps(), 0);
  ipaws_state_.issue_stall_count.assign(arch_.num_warps(), 0);
  suppress_critical_push_ = false;

  // activate first warp and thread
  active_warps_.set(0);
  warps_[0].tmask.set(0);
  wspawn_.valid = false;
}

void Emulator::attach_ram(RAM* ram) {
  // bind RAM to memory unit
#if (XLEN == 64)
  mmu_.attach(*ram, 0, 0x7FFFFFFFFF); //39bit SV39
#else
  mmu_.attach(*ram, 0, 0xFFFFFFFF);
#endif
}

uint32_t Emulator::fetch(uint32_t wid, uint64_t uuid) {
  auto& warp = warps_.at(wid);
  __unused(uuid);

  uint32_t instr_code = 0;
  this->icache_read(&instr_code, warp.PC, sizeof(uint32_t));

  DP(1, "Fetch: code=0x" << std::hex << instr_code << std::dec << ", cid=" << core_->id() << ", wid=" << wid << ", tmask=" << warp.tmask
         << ", PC=0x" << std::hex << warp.PC << " (#" << std::dec << uuid << ")");
  return instr_code;
}

int Emulator::select_static_warp() const {
  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    bool warp_active = active_warps_.test(wid);
    bool warp_stalled = stalled_warps_.test(wid);
    if (warp_active && !warp_stalled) {
      return wid;
    }
  }
  return -1;
}

void Emulator::update_ready_timestamps() {
  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    bool warp_ready = active_warps_.test(wid) && !stalled_warps_.test(wid);
    if (warp_ready) {
      if (ready_timestamps_.at(wid) == 0) {
        ready_timestamps_.at(wid) = schedule_cycle_;
      }
    } else {
      ready_timestamps_.at(wid) = 0;
    }
  }
}

int Emulator::select_gto_warp() {
  auto select_oldest_ready = [&](int excluded_warp) -> int {
    int selected_warp = -1;
    uint64_t oldest_timestamp = std::numeric_limits<uint64_t>::max();
    for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
      if (static_cast<int>(wid) == excluded_warp)
        continue;
      auto ts = ready_timestamps_.at(wid);
      if (ts == 0)
        continue;
      if (ts < oldest_timestamp) {
        oldest_timestamp = ts;
        selected_warp = wid;
      }
    }
    return selected_warp;
  };

  if (greedy_warp_ >= 0) {
    uint32_t wid = static_cast<uint32_t>(greedy_warp_);
    if (active_warps_.test(wid) && !stalled_warps_.test(wid)) {
      return greedy_warp_;
    }
  }

  int selected_warp = select_oldest_ready(-1);
  greedy_warp_ = selected_warp;
  return selected_warp;
}

int Emulator::select_rr_warp() {
  auto nw = arch_.num_warps();
  if (0 == nw)
    return -1;
  for (size_t step = 1; step <= nw; ++step) {
    uint32_t wid = (uint32_t(rr_last_warp_ + step) % nw);
    if (active_warps_.test(wid) && !stalled_warps_.test(wid)) {
      rr_last_warp_ = static_cast<int>(wid);
      return static_cast<int>(wid);
    }
  }
  return -1;
}

void Emulator::update_cpl_counters() { //매 cycle 호출되어서 모든 active warp의 criticality 점수를 갱신
  // 1. Accumulate stall cycles for all active warps that did NOT execute last cycle.
  // Paper (CAWA Algorithm 3): nStall includes both hardware stalls (scoreboard/barrier)
  // AND scheduler delay (ready but not selected). Any active warp that was not the
  // last scheduled warp counts as stalled this cycle.
  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (active_warps_.test(wid) && static_cast<int>(wid) != last_scheduled_warp_) {
      warp_cpl_.at(wid).stall_cycles++;
    }
  }

  // 2. Find the leading warp (highest instr_count) among active warps.
  uint64_t max_inst = 0;
  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (!active_warps_.test(wid)) continue;
    max_inst = std::max(max_inst, warp_cpl_.at(wid).instr_count);
  } //max inst = 가장 많은 명령어 issue한 warp의 카운트

  // 3. Recompute nCriticality = nInst * CPI_avg + nStall.
  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (!active_warps_.test(wid)) continue;
    auto& cpl = warp_cpl_.at(wid); //alias
    uint64_t nInst = max_inst - cpl.instr_count; //내가 선두보다 몇 개 명령어 뒤쳐졌나?
    // CPI_avg approximation: elapsed cycles / committed instructions.
    // Floor at 1 to avoid zeroing out the instruction-disparity term for
    // a warp that just started.
    uint64_t cpi_avg = (cpl.instr_count > 0)
        ? std::max<uint64_t>(1, schedule_cycle_ / cpl.instr_count)
        : 1;
    cpl.criticality = nInst * cpi_avg + cpl.stall_cycles;
  }

#if VORTEX_CPL_LOG_INTERVAL > 0
  if (schedule_cycle_ > 0 && (schedule_cycle_ % VORTEX_CPL_LOG_INTERVAL) == 0) {
    std::cerr << "CPL_LOG: kernel=" << kernel_id_
              << " cycle=" << schedule_cycle_
              << " core=" << (core_ ? core_->id() : 0);
    for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
      const auto& cpl = warp_cpl_.at(wid);
      int active = active_warps_.test(wid) ? 1 : 0;
      std::cerr << " w" << wid << "=" << cpl.instr_count
                << "," << cpl.stall_cycles
                << "," << cpl.criticality
                << "," << active;
    }
    std::cerr << "\n";
  }
#endif
} //결과적으로 이번 cycle에 새로운 criticality 값 update됨.

int Emulator::select_gcaws_warp() {
 
  int prev_critical = critical_warp_;
  if (critical_warp_ >= 0) {
    uint32_t wid = static_cast<uint32_t>(critical_warp_);
    if (active_warps_.test(wid) && !stalled_warps_.test(wid)) {
      return critical_warp_;
    }
  }

  int selected_warp = -1;
  uint64_t best_crit = 0;
  uint64_t best_ts = std::numeric_limits<uint64_t>::max();

  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (!active_warps_.test(wid)) continue;
    if (stalled_warps_.test(wid)) continue;

    uint64_t crit = warp_cpl_.at(wid).criticality;
    uint64_t ts = ready_timestamps_.at(wid);
    if (ts == 0) ts = std::numeric_limits<uint64_t>::max();

    bool take = false;
    if (selected_warp < 0) {
      take = true;
    } else if (crit > best_crit) {
      take = true;
    } else if (crit == best_crit && ts < best_ts) {
      take = true;
    }
    if (take) {
      selected_warp = static_cast<int>(wid);
      best_crit = crit;
      best_ts = ts;
    }
  }

  critical_warp_ = selected_warp;
  if (selected_warp != prev_critical && core_ && !suppress_critical_push_) {
    core_->set_critical_warp(selected_warp);
  }
  return selected_warp;
}

// iPAWS tuning constants.
// VORTEX_IPAWS_* macros are defined at the top of the file (see header
// comment near `using namespace vortex;`).
namespace {
constexpr uint64_t IPAWS_ADAPT_CYCLES        = VORTEX_IPAWS_ADAPT_CYCLES;
constexpr bool     IPAWS_USE_CACP            = (VORTEX_IPAWS_USE_CACP != 0);
constexpr bool     IPAWS_BARRIER_ONLY_BTIME  = (VORTEX_IPAWS_BARRIER_ONLY_BTIME != 0);
constexpr bool     IPAWS_USE_RECOVER         = (VORTEX_IPAWS_USE_RECOVER != 0);
constexpr uint64_t IPAWS_RECOVER_THRESHOLD   = VORTEX_IPAWS_RECOVER_THRESHOLD;
constexpr uint64_t IPAWS_RECOVER_MAX_CYCLES  = VORTEX_IPAWS_RECOVER_MAX_CYCLES;
}

// 논문 §3.3: btime = barrier-wait time. warp w 가 barrier 에서 대기 중인지
// 판정 (scoreboard stall 과 구분). barriers_[i] 는 barrier i 에서 대기 중인
// warp bitmask.
bool Emulator::is_barrier_stalled(uint32_t wid) const {
  for (const auto& b : barriers_) {
    if (b.test(wid)) return true;
  }
  return false;
}

// Change 4: Recover phase scheduler — 논문 §3.4 의 "least instruction-issued
// warp" 우선. argmin(instr_count) + oldest-ready tie-break. newer (Adapt 동안
// 적게 issue 한) warp 가 빨리 따라잡도록 함.
int Emulator::select_recover_warp() {
  int selected = -1;
  uint64_t min_inst = std::numeric_limits<uint64_t>::max();
  uint64_t min_ts = std::numeric_limits<uint64_t>::max();
  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (!active_warps_.test(wid)) continue;
    if (stalled_warps_.test(wid)) continue;
    uint64_t cnt = warp_cpl_.at(wid).instr_count;
    uint64_t ts = ready_timestamps_.at(wid);
    if (ts == 0) ts = std::numeric_limits<uint64_t>::max();
    bool take = false;
    if (selected < 0) take = true;
    else if (cnt < min_inst) take = true;
    else if (cnt == min_inst && ts < min_ts) take = true;
    if (take) {
      selected = static_cast<int>(wid);
      min_inst = cnt;
      min_ts = ts;
    }
  }
  return selected;
}

int Emulator::select_ipaws_warp() {
  auto& s = ipaws_state_;
  if (s.phase == iPAWSPhase::Adapt) {
    // ADAPT phase: GTO 로 probe.
    // - 선택된 warp: adapt_issue +1 (= inst term).
    // - 그 외 active warp 의 adapt_stall (= btime term):
    //   * BARRIER_ONLY_BTIME=1 (Change 3 strict): barrier-stalled warp 만 +1.
    //     논문 §3.3 의 btime = barrier wait time 정의에 정확히 일치.
    //   * BARRIER_ONLY_BTIME=0 (이전 동작): active && !chosen 모두 +1.
    int chosen = select_gto_warp();
    if (chosen >= 0) {
      ++s.adapt_issue.at(chosen);
    }
    for (uint32_t w = 0, nw = arch_.num_warps(); w < nw; ++w) {
      if (static_cast<int>(w) == chosen) continue;
      if (!active_warps_.test(w)) continue;
      if (IPAWS_BARRIER_ONLY_BTIME) {
        if (is_barrier_stalled(w)) {
          ++s.adapt_stall.at(w);
        }
      } else {
        ++s.adapt_stall.at(w);
      }
    }
    // iPAWS 논문 §3.2 WOI 필터용 per-warp issue-stall counter (영구 누적).
    // Paper strict 해석: "warp w 가 stalled 인데 oldest 일 때" 만 +1.
    // NOTE: 본 Vortex 환경에서 이 필터는 architectural mismatch 로 적용 불가
    //   (urp_analysis.md 종합 진단 참고). 코드는 paper-faithful 유지.
    for (uint32_t w = 0, nw = arch_.num_warps(); w < nw; ++w) {
      if (!active_warps_.test(w)) continue;
      if (stalled_warps_.test(w)) {
        ++s.issue_stall_count.at(w);
      }
      break;  // lowest-id active warp 만 "the oldest" 자격.
    }
    return chosen;
  }
  if (s.phase == iPAWSPhase::Recover) {
    return select_recover_warp();
  }
  // Execute phase: dispatch the policy chosen at last Decide.
  if (s.chosen == WarpSchedulePolicy::RR) {
    return select_rr_warp();
  }
  return select_gcaws_warp();
}

void Emulator::ipaws_sample_and_step() {
  auto& s = ipaws_state_;
  uint64_t now = schedule_cycle_;
  uint64_t elapsed = now - s.phase_start_cycle;

  switch (s.phase) {
  case iPAWSPhase::Adapt: {
    if (elapsed < IPAWS_ADAPT_CYCLES) break;

    // iPAWS Algorithm 1 평가.
    //   iscore[w] = adapt_issue[w] + adapt_stall[w]
    //   WOI       = stall-rank 기반 필터 (논문 §3.2):
    //               w* = argmax(issue_stall_count)  (active warps 중)
    //               WOI = {w : w_id <= w*_id AND active}
    //               -> 한번도 head-of-line 못 된 newer warp 들 제외.
    //   concave   = iscore_sum < |WOI| * iscore_max / 2  (mean/max < 0.5). -> gcaws
    //
    // issue_stall_count 가 전부 0 (= ADAPT 안에서 oldest active warp 이
    // 한번도 stall 안한 경우) 면 stall-rank로 WOI 못 정함 -> 기존
    // "iscore>0" 필터로 fallback.
    int w_star = -1;
    uint64_t best_stall = 0;
    for (uint32_t w = 0, nw = arch_.num_warps(); w < nw; ++w) {
      if (!active_warps_.test(w)) continue;
      uint64_t sc = s.issue_stall_count.at(w);
      if (sc > best_stall) {
        best_stall = sc;
        w_star = static_cast<int>(w);
      }
      // tie 는 더 oldest (lower wid) 우선 ->  조건만 사용
    }

    bool used_woi_filter = (w_star >= 0);
    if (!used_woi_filter) {
      ++s.decides_woi_fallback;
    }

    uint64_t iscore_sum = 0;
    uint64_t iscore_max = 0;
    size_t   woi_size   = 0;
    for (uint32_t w = 0, nw = arch_.num_warps(); w < nw; ++w) {
      if (!active_warps_.test(w)) continue;
      // stall-rank WOI 필터: w_id <= w*_id 만 포함.
      if (used_woi_filter && static_cast<int>(w) > w_star) continue;
      uint64_t iscore = s.adapt_issue.at(w) + s.adapt_stall.at(w);
      if (iscore == 0) continue;  // 참여 안한 warp 제거 (fallback path 에서 의미 있음).
      iscore_sum += iscore;
      if (iscore > iscore_max) iscore_max = iscore;
      ++woi_size;
    }

    bool concave = false;
    bool valid = (woi_size >= 2 && iscore_max > 0);
    double meanmax = 0.0;
    if (valid) {
      uint64_t threshold = static_cast<uint64_t>(woi_size) * iscore_max / 2;
      concave = (iscore_sum < threshold);
      meanmax = static_cast<double>(iscore_sum)
              / (static_cast<double>(woi_size) * static_cast<double>(iscore_max));
    }

    ++s.decides_total;
    if (valid) {
      ++s.decides_valid;
      s.decide_meanmax_accum += meanmax;
      if (meanmax < s.decide_meanmax_min) s.decide_meanmax_min = meanmax;
      if (meanmax > s.decide_meanmax_max) s.decide_meanmax_max = meanmax;
      uint32_t wsize = static_cast<uint32_t>(woi_size);
      s.woi_size_accum += wsize;
      if (wsize < s.woi_size_min) s.woi_size_min = wsize;
      if (wsize > s.woi_size_max) s.woi_size_max = wsize;
    } else {
      ++s.decides_skipped;
    }

    if (concave) {
      // Concave -> gCAWS. Recover 불필요 (gCAWS 가 skew 자체를 활용).
      s.chosen = WarpSchedulePolicy::gCAWS;
      ++s.decides_concave;
      suppress_critical_push_ = !IPAWS_USE_CACP;
      if (suppress_critical_push_ && core_) {
        core_->set_critical_warp(-1);
      }
      s.phase = iPAWSPhase::Execute;
    } else {
      // Convex -> RR. Change 4 (논문 §3.4): Recover phase 로 instr_count
      // skew 정리 후 RR 진입. USE_RECOVER=0 이면 바로 Execute.
      s.chosen = WarpSchedulePolicy::RR;
      ++s.decides_convex;
      suppress_critical_push_ = true;
      if (core_) {
        critical_warp_ = -1;
        core_->set_critical_warp(-1);
      }
      if (IPAWS_USE_RECOVER) {
        // Recover 목표 = 현재 active warp 들의 max(instr_count).
        // newest warp 들이 이 값에 도달할 때까지 Recover.
        uint64_t mx = 0, mn = std::numeric_limits<uint64_t>::max();
        uint64_t sum = 0, sumsq = 0;
        size_t n_act = 0;
        int wid_max = -1, wid_min = -1;
        std::cerr << "IPAWS_DBG_RECOVER_ENTRY: cycle=" << schedule_cycle_
                  << " per_warp_instr_count=[";
        for (uint32_t w = 0, nw = arch_.num_warps(); w < nw; ++w) {
          if (!active_warps_.test(w)) continue;
          uint64_t c = warp_cpl_.at(w).instr_count;
          if (n_act > 0) std::cerr << ",";
          std::cerr << w << ":" << c;
          if (c > mx) { mx = c; wid_max = static_cast<int>(w); }
          if (c < mn) { mn = c; wid_min = static_cast<int>(w); }
          sum += c;
          sumsq += c * c;
          ++n_act;
        }
        double mean = (n_act > 0) ? (static_cast<double>(sum) / n_act) : 0.0;
        double var = (n_act > 0) ? (static_cast<double>(sumsq) / n_act - mean * mean) : 0.0;
        double stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
        double range_over_mean = (mean > 0.0) ? (static_cast<double>(mx - mn) / mean) : 0.0;
        double range_over_max = (mx > 0) ? (static_cast<double>(mx - mn) / mx) : 0.0;
        std::cerr << "] n=" << n_act
                  << " max=" << mx << "(w" << wid_max << ")"
                  << " min=" << mn << "(w" << wid_min << ")"
                  << " mean=" << std::fixed << std::setprecision(1) << mean
                  << " stddev=" << stddev
                  << " range=" << (mx - mn)
                  << " range/mean=" << std::setprecision(3) << range_over_mean
                  << " range/max=" << range_over_max
                  << std::endl;
        s.recover_target = mx;
        ++s.recover_entries;
        s.phase = iPAWSPhase::Recover;
      } else {
        s.phase = iPAWSPhase::Execute;
      }
    }
    s.phase_start_cycle = now;
    std::fill(s.adapt_issue.begin(), s.adapt_issue.end(), 0);
    std::fill(s.adapt_stall.begin(), s.adapt_stall.end(), 0);
    break;
  }
  case iPAWSPhase::Recover: {
    ++s.recover_cycles;
    // 종료 조건: (a) active warp 들의 min(instr_count) 가 recover_target 도달,
    //          또는 (b) max-min <= threshold,
    //          또는 (c) cycle cap (RECOVER_MAX_CYCLES) 도달 — safety.
    uint64_t mn = std::numeric_limits<uint64_t>::max();
    uint64_t mx = 0;
    size_t   n_active = 0;
    for (uint32_t w = 0, nw = arch_.num_warps(); w < nw; ++w) {
      if (!active_warps_.test(w)) continue;
      uint64_t c = warp_cpl_.at(w).instr_count;
      mn = std::min(mn, c);
      mx = std::max(mx, c);
      ++n_active;
    }
    bool done = false;
    if (n_active == 0) done = true;
    else if (mn >= s.recover_target) done = true;
    else if ((mx - mn) <= IPAWS_RECOVER_THRESHOLD) done = true;
    else if (elapsed >= IPAWS_RECOVER_MAX_CYCLES) done = true;
    if (done) {
      s.phase = iPAWSPhase::Execute;
      s.phase_start_cycle = now;
    }
    break;
  }
  case iPAWSPhase::Execute: {
    // 다음 wspawn (kernel launch) 까지 무한 지속. periodic 재트리거 없음.
    if (s.chosen == WarpSchedulePolicy::gCAWS)
      ++s.gcaws_exec_cycles;
    else
      ++s.rr_exec_cycles;
    break;
  }
  }
}

instr_trace_t* Emulator::step() {
  int scheduled_warp = -1;

  // process pending wspawn
  if (wspawn_.valid && active_warps_.count() == 1) {
    DP(3, "*** Activate " << (wspawn_.num_warps-1) << " warps at PC: " << std::hex << wspawn_.nextPC << std::dec);
    for (uint32_t i = 1; i < wspawn_.num_warps; ++i) {
      auto& warp = warps_.at(i);
      warp.PC = wspawn_.nextPC;
      warp.tmask.set(0);
      active_warps_.set(i);
    }
    wspawn_.valid = false;
    stalled_warps_.reset(0);

    // wspawn = kernel-launch 마커. iPAWS 의 Adapt phase 를 여기서 강제로
    // 시작 (이전 Execute 결과 무시). 매 kernel 마다 policy 결정 fresh.
    if (schedule_policy_ == WarpSchedulePolicy::iPAWS) {
      auto& s = ipaws_state_;
      ++s.wspawn_events;
      s.phase = iPAWSPhase::Adapt;
      s.phase_start_cycle = schedule_cycle_;
      std::fill(s.adapt_issue.begin(), s.adapt_issue.end(), 0);
      std::fill(s.adapt_stall.begin(), s.adapt_stall.end(), 0);
      // critical_warp / CACP 신호 quiesce.
      suppress_critical_push_ = true;
      critical_warp_ = -1;
      if (core_) core_->set_critical_warp(-1);
    }
  }

  if (WarpSchedulePolicy::GTO == schedule_policy_
      || WarpSchedulePolicy::gCAWS == schedule_policy_
      || WarpSchedulePolicy::iPAWS == schedule_policy_) {
    ++schedule_cycle_;
    update_ready_timestamps();
  }
  if (WarpSchedulePolicy::gCAWS == schedule_policy_
      || WarpSchedulePolicy::iPAWS == schedule_policy_) {
    update_cpl_counters();
  }
  if (WarpSchedulePolicy::iPAWS == schedule_policy_) {
    ipaws_sample_and_step();
  }

  // find next ready warp according to policy
  switch (schedule_policy_) {
  case WarpSchedulePolicy::Static:
    scheduled_warp = select_static_warp();
    break;
  case WarpSchedulePolicy::GTO:
    scheduled_warp = select_gto_warp();
    break;
  case WarpSchedulePolicy::RR:
    scheduled_warp = select_rr_warp();
    break;
  case WarpSchedulePolicy::gCAWS:
    scheduled_warp = select_gcaws_warp();
    break;
  case WarpSchedulePolicy::iPAWS:
    scheduled_warp = select_ipaws_warp();
    break;
  default:
    assert(false);
  }

  // Record the selected warp so update_cpl_counters() can charge scheduler delay
  // to all other active warps next cycle (paper CAWA Algorithm 3).
  if (schedule_policy_ == WarpSchedulePolicy::gCAWS
      || schedule_policy_ == WarpSchedulePolicy::iPAWS) {
    last_scheduled_warp_ = scheduled_warp;
  }

  if (scheduled_warp == -1)
    return nullptr;

  // get scheduled warp
  auto& warp = warps_.at(scheduled_warp);
  assert(warp.tmask.any());

  // fetch next instruction if ibuffer is empty
  if (warp.ibuffer.empty()) {
    uint64_t uuid = 0;
  #ifndef NDEBUG
    {
      // generate unique universal instruction ID
      uint32_t instr_uuid = warp.uuid++;
      uint32_t g_wid = core_->id() * arch_.num_warps() + scheduled_warp;
      uuid = (uint64_t(g_wid) << 32) | instr_uuid;
    }
  #endif

    // Fetch
    auto instr_code = this->fetch(scheduled_warp, uuid);

    // decode
    this->decode(instr_code, scheduled_warp, uuid);
  } else {
    // we have a micro-instruction in the ibuffer
    // adjust PC back to original (incremented in execute())
    warp.PC -= 4;
  }

  // pop the instruction from the ibuffer
  auto instr = warp.ibuffer.front();
  warp.ibuffer.pop_front();

  // Execute
  auto trace = this->execute(*instr, scheduled_warp);

  // Track committed instruction count for CPL.
  warp_cpl_.at(scheduled_warp).instr_count++;

  return trace;
}

bool Emulator::running() const {
  return active_warps_.any();
}

int Emulator::get_exitcode() const {
  return warps_.at(0).ireg_file.at(3).at(0);
}

void Emulator::suspend(uint32_t wid) {
  assert(!stalled_warps_.test(wid));
  stalled_warps_.set(wid);
}

void Emulator::resume(uint32_t wid) {
  if (wid != 0xffffffff) {
    assert(stalled_warps_.test(wid));
    stalled_warps_.reset(wid);
  } else {
    stalled_warps_.reset();
  }
}

bool Emulator::wspawn(uint32_t num_warps, Word nextPC) {
  num_warps = std::min<uint32_t>(num_warps, arch_.num_warps());
  if (num_warps < 2 && active_warps_.count() == 1)
    return true;
  wspawn_.valid = true;
  wspawn_.num_warps = num_warps;
  wspawn_.nextPC = nextPC;
  return false;
}

bool Emulator::barrier(uint32_t bar_id, uint32_t count, uint32_t wid) {
  if (count < 2)
    return true;

  uint32_t bar_idx = bar_id & 0x7fffffff;
  bool is_global = (bar_id >> 31);

  auto& barrier = barriers_.at(bar_idx);
  barrier.set(wid);
  DP(3, "*** Suspend core #" << core_->id() << ", warp #" << wid << " at barrier #" << bar_idx);

  if (is_global) {
    // global barrier handling
    if (barrier.count() == active_warps_.count()) {
      core_->socket()->barrier(bar_idx, count, core_->id());
      barrier.reset();
    }
  } else {
    // local barrier handling
    if (barrier.count() == (size_t)count) {
      // resume suspended warps
      for (uint32_t i = 0; i < arch_.num_warps(); ++i) {
        if (barrier.test(i)) {
          DP(3, "*** Resume core #" << core_->id() << ", warp #" << i << " at barrier #" << bar_idx);
          stalled_warps_.reset(i);
        }
      }
      barrier.reset();
    }
  }
  return false;
}

#ifdef VM_ENABLE
void Emulator::icache_read(void *data, uint64_t addr, uint32_t size) {
  DP(3, "*** icache_read 0x" << std::hex << addr << ", size = 0x "  << size);
  try
  {
    mmu_.read(data, addr, size, ACCESS_TYPE::FETCH);
  }
  catch (Page_Fault_Exception& page_fault)
  {
    std::cout<<page_fault.what()<<std::endl;
    throw;
  }
}
#else
void Emulator::icache_read(void *data, uint64_t addr, uint32_t size) {
  mmu_.read(data, addr, size, 0);
}
#endif

#ifdef VM_ENABLE
void Emulator::set_satp(uint64_t satp) {
  DPH(3, "set satp 0x" << std::hex << satp << " in emulator module\n");
  set_csr(VX_CSR_SATP,satp,0,0);
}
#endif


#ifdef VM_ENABLE
void Emulator::dcache_read(void *data, uint64_t addr, uint32_t size) {
  DP(1, "*** dcache_read 0x" << std::hex << addr << ", size = 0x "  << size);
  auto type = get_addr_type(addr);
  if (type == AddrType::Shared) {
    core_->local_mem()->read(data, addr, size);
  } else {
    try
    {
      mmu_.read(data, addr, size, ACCESS_TYPE::LOAD);
    }
    catch (Page_Fault_Exception& page_fault)
    {
      std::cout<<page_fault.what()<<std::endl;
      throw;
    }
  }
  DPH(2, "Mem Read: addr=0x" << std::hex << addr << ", data=0x" << ByteStream(data, size) << " (size=" << size << ", type=" << type << ")" << std::endl);
}
#else
void Emulator::dcache_read(void *data, uint64_t addr, uint32_t size) {
  auto type = get_addr_type(addr);
  if (type == AddrType::Shared) {
    core_->local_mem()->read(data, addr, size);
  } else {
    mmu_.read(data, addr, size, 0);
  }
  DPH(2, "Mem Read: addr=0x" << std::hex << addr << ", data=0x" << ByteStream(data, size) << std::dec << " (size=" << size << ", type=" << type << ")" << std::endl);
}
#endif

#ifdef VM_ENABLE
void Emulator::dcache_write(const void* data, uint64_t addr, uint32_t size) {
  DP(1, "*** dcache_write 0x" << std::hex << addr << ", size = 0x "  << size);
  auto type = get_addr_type(addr);
  if (addr >= uint64_t(IO_COUT_ADDR)
   && addr < (uint64_t(IO_COUT_ADDR) + IO_COUT_SIZE)) {
     this->writeToStdOut(data, addr, size);
  } else {
    if (type == AddrType::Shared) {
      core_->local_mem()->write(data, addr, size);
    } else {
      try
      {
        // mmu_.write(data, addr, size, 0);
        mmu_.write(data, addr, size, ACCESS_TYPE::STORE);
      }
      catch (Page_Fault_Exception& page_fault)
      {
        std::cout<<page_fault.what()<<std::endl;
        throw;
      }
    }
  }
  DPH(2, "Mem Write: addr=0x" << std::hex << addr << ", data=0x" << ByteStream(data, size) << " (size=" << size << ", type=" << type << ")" << std::endl);
}
#else
void Emulator::dcache_write(const void* data, uint64_t addr, uint32_t size) {
  auto type = get_addr_type(addr);
  if (addr >= uint64_t(IO_COUT_ADDR)
   && addr < (uint64_t(IO_COUT_ADDR) + IO_COUT_SIZE)) {
    this->writeToStdOut(data, addr, size);
  } else {
    if (type == AddrType::Shared) {
      core_->local_mem()->write(data, addr, size);
    } else {
      mmu_.write(data, addr, size, 0);
    }
  }
  DPH(2, "Mem Write: addr=0x" << std::hex << addr << ", data=0x" << ByteStream(data, size) << std::dec << " (size=" << size << ", type=" << type << ")" << std::endl);
}
#endif

void Emulator::dcache_amo_reserve(uint64_t addr) {
  auto type = get_addr_type(addr);
  if (type == AddrType::Global) {
    mmu_.amo_reserve(addr);
  }
}

bool Emulator::dcache_amo_check(uint64_t addr) {
  auto type = get_addr_type(addr);
  if (type == AddrType::Global) {
    return mmu_.amo_check(addr);
  }
  return false;
}

void Emulator::writeToStdOut(const void* data, uint64_t addr, uint32_t size) {
  if (size != 1)
    std::abort();
  uint32_t tid = (addr - IO_COUT_ADDR) & (IO_COUT_SIZE-1);
  auto& ss_buf = print_bufs_[tid];
  char c = *(char*)data;
  ss_buf << c;
  if (c == '\n') {
    std::cout << "#" << tid << ": " << ss_buf.str() << std::flush;
    ss_buf.str("");
  }
}

void Emulator::cout_flush() {
  for (auto& buf : print_bufs_) {
    auto str = buf.second.str();
    if (!str.empty()) {
      std::cout << "#" << buf.first << ": " << str << std::endl;
    }
  }
}

#ifdef XLEN_64
  #define CSR_READ_64(addr, value) \
    case addr: return value
#else
  #define CSR_READ_64(addr, value) \
    case addr : return (uint32_t)value; \
    case (addr + (VX_CSR_MPM_BASE_H-VX_CSR_MPM_BASE)) : return ((value >> 32) & 0xFFFFFFFF)
#endif

Word Emulator::get_csr(uint32_t addr, uint32_t wid, uint32_t tid) {
  auto core_perf = core_->perf_stats();
  switch (addr) {
  case VX_CSR_SATP:
#ifdef VM_ENABLE
    // return csrs_.at(wid).at(tid)[addr];
    return mmu_.get_satp();
#endif
  case VX_CSR_PMPCFG0:
  case VX_CSR_PMPADDR0:
  case VX_CSR_MSTATUS:
  case VX_CSR_MISA:
  case VX_CSR_MEDELEG:
  case VX_CSR_MIDELEG:
  case VX_CSR_MIE:
  case VX_CSR_MTVEC:
  case VX_CSR_MEPC:
  case VX_CSR_MNSTATUS:
  case VX_CSR_MCAUSE:
    return 0;

  case VX_CSR_FFLAGS: return warps_.at(wid).fcsr & 0x1F;
  case VX_CSR_FRM:    return (warps_.at(wid).fcsr >> 5);
  case VX_CSR_FCSR:   return warps_.at(wid).fcsr;

  case VX_CSR_MHARTID:    return (core_->id() * arch_.num_warps() + wid) * arch_.num_threads() + tid;
  case VX_CSR_THREAD_ID:  return tid;
  case VX_CSR_WARP_ID:    return wid;
  case VX_CSR_CORE_ID:    return core_->id();
  case VX_CSR_ACTIVE_THREADS:return warps_.at(wid).tmask.to_ulong();
  case VX_CSR_ACTIVE_WARPS:return active_warps_.to_ulong();
  case VX_CSR_NUM_THREADS:return arch_.num_threads();
  case VX_CSR_NUM_WARPS:  return arch_.num_warps();
  case VX_CSR_NUM_CORES:  return uint32_t(arch_.num_cores()) * arch_.num_clusters();
  case VX_CSR_LOCAL_MEM_BASE: return arch_.local_mem_base();
  case VX_CSR_MSCRATCH:   return csr_mscratch_;

  CSR_READ_64(VX_CSR_MCYCLE, core_perf.cycles);
  CSR_READ_64(VX_CSR_MINSTRET, core_perf.instrs);
  default:
  #ifdef EXT_V_ENABLE
    Word value = 0;
    if (vec_unit_->get_csr(addr, wid, tid, &value))
      return value;
  #endif
    if ((addr >= VX_CSR_MPM_BASE && addr < (VX_CSR_MPM_BASE + 32))
     || (addr >= VX_CSR_MPM_BASE_H && addr < (VX_CSR_MPM_BASE_H + 32))) {
      // user-defined MPM CSRs
      auto perf_class = dcrs_.base_dcrs.read(VX_DCR_BASE_MPM_CLASS);
      switch (perf_class) {
      case VX_DCR_MPM_CLASS_NONE:
        break;
      case VX_DCR_MPM_CLASS_CORE: {
        switch (addr) {
        CSR_READ_64(VX_CSR_MPM_SCHED_ID, core_perf.sched_idle);
        CSR_READ_64(VX_CSR_MPM_SCHED_ST, core_perf.sched_stalls);
        CSR_READ_64(VX_CSR_MPM_IBUF_ST, core_perf.ibuf_stalls);
        CSR_READ_64(VX_CSR_MPM_SCRB_ST, core_perf.scrb_stalls);
        CSR_READ_64(VX_CSR_MPM_OPDS_ST, core_perf.opds_stalls);
        CSR_READ_64(VX_CSR_MPM_SCRB_ALU, core_perf.scrb_alu);
        CSR_READ_64(VX_CSR_MPM_SCRB_FPU, core_perf.scrb_fpu);
        CSR_READ_64(VX_CSR_MPM_SCRB_LSU, core_perf.scrb_lsu);
        CSR_READ_64(VX_CSR_MPM_SCRB_SFU, core_perf.scrb_sfu);
      #ifdef EXT_TCU_ENABLE
        CSR_READ_64(VX_CSR_MPM_SCRB_TCU, core_perf.scrb_tcu);
      #endif
      #ifdef EXT_VPU_ENABLE
        CSR_READ_64(VX_CSR_MPM_SCRB_TCU, core_perf.scrb_vpu);
      #endif
        CSR_READ_64(VX_CSR_MPM_SCRB_CSRS, core_perf.scrb_csrs);
        CSR_READ_64(VX_CSR_MPM_SCRB_WCTL, core_perf.scrb_wctl);
        CSR_READ_64(VX_CSR_MPM_IFETCHES, core_perf.ifetches);
        CSR_READ_64(VX_CSR_MPM_LOADS, core_perf.loads);
        CSR_READ_64(VX_CSR_MPM_STORES, core_perf.stores);
        CSR_READ_64(VX_CSR_MPM_IFETCH_LT, core_perf.ifetch_latency);
        CSR_READ_64(VX_CSR_MPM_LOAD_LT, core_perf.load_latency);
        }
      } break;
      case VX_DCR_MPM_CLASS_MEM: {
        auto proc_perf = core_->socket()->cluster()->processor()->perf_stats();
        auto cluster_perf = core_->socket()->cluster()->perf_stats();
        auto socket_perf = core_->socket()->perf_stats();
        auto lmem_perf = core_->local_mem()->perf_stats();

        uint64_t coalescer_misses = 0;
        for (uint i = 0; i < NUM_LSU_BLOCKS; ++i) {
          coalescer_misses += core_->mem_coalescer(i)->perf_stats().misses;
        }

        switch (addr) {
        CSR_READ_64(VX_CSR_MPM_ICACHE_READS, socket_perf.icache.reads);
        CSR_READ_64(VX_CSR_MPM_ICACHE_MISS_R, socket_perf.icache.read_misses);
        CSR_READ_64(VX_CSR_MPM_ICACHE_MSHR_ST, socket_perf.icache.mshr_stalls);

        CSR_READ_64(VX_CSR_MPM_DCACHE_READS, socket_perf.dcache.reads);
        CSR_READ_64(VX_CSR_MPM_DCACHE_WRITES, socket_perf.dcache.writes);
        CSR_READ_64(VX_CSR_MPM_DCACHE_MISS_R, socket_perf.dcache.read_misses);
        CSR_READ_64(VX_CSR_MPM_DCACHE_MISS_W, socket_perf.dcache.write_misses);
        CSR_READ_64(VX_CSR_MPM_DCACHE_BANK_ST, socket_perf.dcache.bank_stalls);
        CSR_READ_64(VX_CSR_MPM_DCACHE_MSHR_ST, socket_perf.dcache.mshr_stalls);

        CSR_READ_64(VX_CSR_MPM_L2CACHE_READS, cluster_perf.l2cache.reads);
        CSR_READ_64(VX_CSR_MPM_L2CACHE_WRITES, cluster_perf.l2cache.writes);
        CSR_READ_64(VX_CSR_MPM_L2CACHE_MISS_R, cluster_perf.l2cache.read_misses);
        CSR_READ_64(VX_CSR_MPM_L2CACHE_MISS_W, cluster_perf.l2cache.write_misses);
        CSR_READ_64(VX_CSR_MPM_L2CACHE_BANK_ST, cluster_perf.l2cache.bank_stalls);
        CSR_READ_64(VX_CSR_MPM_L2CACHE_MSHR_ST, cluster_perf.l2cache.mshr_stalls);

        CSR_READ_64(VX_CSR_MPM_L3CACHE_READS, proc_perf.l3cache.reads);
        CSR_READ_64(VX_CSR_MPM_L3CACHE_WRITES, proc_perf.l3cache.writes);
        CSR_READ_64(VX_CSR_MPM_L3CACHE_MISS_R, proc_perf.l3cache.read_misses);
        CSR_READ_64(VX_CSR_MPM_L3CACHE_MISS_W, proc_perf.l3cache.write_misses);
        CSR_READ_64(VX_CSR_MPM_L3CACHE_BANK_ST, proc_perf.l3cache.bank_stalls);
        CSR_READ_64(VX_CSR_MPM_L3CACHE_MSHR_ST, proc_perf.l3cache.mshr_stalls);

        CSR_READ_64(VX_CSR_MPM_MEM_READS, proc_perf.mem_reads);
        CSR_READ_64(VX_CSR_MPM_MEM_WRITES, proc_perf.mem_writes);
        CSR_READ_64(VX_CSR_MPM_MEM_LT, proc_perf.mem_latency);
        CSR_READ_64(VX_CSR_MPM_MEM_BANK_ST, proc_perf.memsim.bank_stalls);

        CSR_READ_64(VX_CSR_MPM_COALESCER_MISS, coalescer_misses);

        CSR_READ_64(VX_CSR_MPM_LMEM_READS, lmem_perf.reads);
        CSR_READ_64(VX_CSR_MPM_LMEM_WRITES, lmem_perf.writes);
        CSR_READ_64(VX_CSR_MPM_LMEM_BANK_ST, lmem_perf.bank_stalls);
        }
      } break;
      default:
        std::cerr << "Error: invalid MPM CLASS: value=" << perf_class << std::endl;
        std::abort();
        break;
      }
    } else {
      std::cerr << "Error: invalid CSR read addr=0x"<< std::hex << addr << std::dec << std::endl;
      std::abort();
    }
  }
  return 0;
}

void Emulator::set_csr(uint32_t addr, Word value, uint32_t wid, uint32_t tid) {
  __unused (tid);
  switch (addr) {
  case VX_CSR_FFLAGS:
    warps_.at(wid).fcsr = (warps_.at(wid).fcsr & ~0x1F) | (value & 0x1F);
    break;
  case VX_CSR_FRM:
    warps_.at(wid).fcsr = (warps_.at(wid).fcsr & ~0xE0) | (value << 5);
    break;
  case VX_CSR_FCSR:
    warps_.at(wid).fcsr = value & 0xff;
    break;
  case VX_CSR_MSCRATCH:
    csr_mscratch_ = value;
    break;
  case VX_CSR_SATP:
  #ifdef VM_ENABLE
    mmu_.set_satp(value);
  #endif
    break;
  case VX_CSR_MSTATUS:
  case VX_CSR_MEDELEG:
  case VX_CSR_MIDELEG:
  case VX_CSR_MIE:
  case VX_CSR_MTVEC:
  case VX_CSR_MEPC:
  case VX_CSR_PMPCFG0:
  case VX_CSR_PMPADDR0:
  case VX_CSR_MNSTATUS:
  case VX_CSR_MCAUSE:
    break;
  default: {
    #ifdef EXT_V_ENABLE
      if (vec_unit_->set_csr(addr, wid, tid, value))
        return;
    #endif
      std::cerr << "Error: invalid CSR write addr=0x" << std::hex << addr << ", value=0x" << value << std::dec << std::endl;
      std::flush(std::cout);
      std::abort();
    }
  }
}

uint32_t Emulator::get_fpu_rm(uint32_t funct3, uint32_t wid, uint32_t tid) {
  return (funct3 == 0x7) ? this->get_csr(VX_CSR_FRM, wid, tid) : funct3;
}

void Emulator::update_fcrs(uint32_t fflags, uint32_t wid, uint32_t tid) {
  if (fflags) {
    this->set_csr(VX_CSR_FCSR, this->get_csr(VX_CSR_FCSR, wid, tid) | fflags, wid, tid);
    this->set_csr(VX_CSR_FFLAGS, this->get_csr(VX_CSR_FFLAGS, wid, tid) | fflags, wid, tid);
  }
}

// For riscv-vector test functionality, ecall and ebreak must trap
// These instructions are used in the vector tests to stop execution of the test
// Therefore, without these instructions, undefined and incorrect behavior happens
//
// For now, we need these instructions to trap for testing the riscv-vector isa
void Emulator::trigger_ecall() {
  active_warps_.reset();
}
void Emulator::trigger_ebreak() {
  active_warps_.reset();
}
