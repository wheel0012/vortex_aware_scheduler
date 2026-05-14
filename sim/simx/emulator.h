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

#ifndef __WARP_H
#define __WARP_H

#include <vector>
#include <sstream>
#include <stack>
#include <mem.h>
#include "types.h"
#include "instr.h"
#ifdef EXT_TCU_ENABLE
#include "tensor_unit.h"
#endif
#ifdef EXT_V_ENABLE
#include "vec_unit.h"
#endif

namespace vortex {

class Arch;
class DCRS;
class Core;
class Instr;
class instr_trace_t;

struct ipdom_entry_t {
  ThreadMask  orig_tmask;
  ThreadMask  else_tmask;
  Word        PC;
  bool        fallthrough;

  ipdom_entry_t(const ThreadMask &orig_tmask, const ThreadMask &else_tmask, Word PC)
    : orig_tmask (orig_tmask)
    , else_tmask (else_tmask)
    , PC         (PC)
    , fallthrough(false)
  {}
};

///////////////////////////////////////////////////////////////////////////////

struct warp_t {
  std::vector<std::vector<Word>>    ireg_file;
  std::vector<std::vector<uint64_t>>freg_file;
  std::deque<Instr::Ptr>            ibuffer;
  std::stack<ipdom_entry_t>         ipdom_stack;
  ThreadMask                        tmask;
  Word                              PC;
  Byte                              fcsr;
  uint32_t                          uuid;

  warp_t(uint32_t num_threads);

  void reset(uint64_t startup_addr);
};

///////////////////////////////////////////////////////////////////////////////

struct wspawn_t {
  bool      valid;
  uint32_t  num_warps;
  Word      nextPC;
};

///////////////////////////////////////////////////////////////////////////////

enum class WarpSchedulePolicy {
  Static,
  GTO,
  RR,
  gCAWS, //gcaws 추가.
  iPAWS //ipaws 추가.
};

///////////////////////////////////////////////////////////////////////////////

// iPAWS는 원본 논문 Algorithm 1을 그대로 따라 gCAWS와 RR 사이를 전환함.
//   - Adapt phase: GTO로 probe한다. gcaws로 probe가 아님. 매 cycle GTO가 선택한 warp는 issue +1,
//                  그 외의 모든 active warp은 stall +1 (이유 무관 — ready였
//                  으나 패스됐든, scoreboard/barrier 때문에 대기 중이든).
//                  iscore[w] = adapt_issue[w] + adapt_stall[w]
//                  는 논문의 iscore = inst_i + btime_i 와 일치한다.
//   - Decide:      WOI(이 윈도우에 참여한 warp들) 만 본다. 만약
//                    iscore_sum < |WOI| × iscore_max / 2   (= mean/max < 0.5)
//                  이면 분포가 concave → gCAWS 선택. 아니면 convex → RR 선택.
//   - Execute phase: 선택된 정책을 IPAWS_EXECUTE_CYCLES 동안 실행한다.
enum class iPAWSPhase { //adapt → (concave) → execute / (convex) → recover → execute
  Adapt,
  Recover,  // 논문 §3.4: Convex(RR) 결정 후 GTO probe 의 instr_count skew 정리
  Execute
};

struct ipaws_state_t {
  iPAWSPhase           phase;  //현재 phase
  uint64_t             phase_start_cycle; //이 phase 시작 cycle
  WarpSchedulePolicy   chosen;       // execute에서 돌릴 policy

  // adapt window 내 카운터 ( window 시작마다 0으로 reset )
  std::vector<uint64_t> adapt_issue;
  std::vector<uint64_t> adapt_stall;

  // iPAWS paper §3.2, Figure 8: WOI(warps-of-interest) 필터용 영구 카운터.
  // ADAPT phase 매 cycle "가장 oldest active warp이 stalled"이면 그 warp에
  // +1. Decide 시점에 w* = argmax(issue_stall_count)을 잡고,
  // WOI = {w : w_id <= w*_id AND active} 로 metric 계산을 제한.
  // 영구 누적 (ADAPT window 마다 reset 안함) -> kernel 진행하며 WOI 수렴.
  std::vector<uint64_t> issue_stall_count;

  // 통계 / 진단 (diagnostic)
  uint64_t             decides_total;     // 전체 decide 횟수
  uint64_t             decides_valid;     // 유의미한 decide 횟수
  uint64_t             decides_skipped;   // decides where WOI < 2 (no meaningful test)
  uint64_t             decides_woi_fallback;  // issue_stall_count 전부 0이라 iscore>0 fallback한 횟수
  uint64_t             decides_concave;   // chose gCAWS branch 횟수
  uint64_t             decides_convex;    // chose RR branch 횟수
  double               decide_meanmax_accum;  // sum of (mean/max) over *valid* decides only
  double               decide_meanmax_min;
  double               decide_meanmax_max;
  uint64_t             woi_size_accum;    // valid decide의 |WOI| 합
  uint32_t             woi_size_min;
  uint32_t             woi_size_max;
  uint64_t             gcaws_exec_cycles; //gcaws로 실행한 cycle 누적
  uint64_t             rr_exec_cycles; //rr로 실행한 cycle 누적
  uint64_t             wspawn_events;     // kernel-boundary 마커: wspawn 디스패치 횟수
  uint64_t             recover_target;    // Recover 시작 시 max(instr_count) — newest 가 따라잡을 목표
  uint64_t             recover_cycles;    // Recover phase 누적 cycle (overhead 측정용)
  uint64_t             recover_entries;   // Recover phase 진입 횟수

  ipaws_state_t()
    : phase(iPAWSPhase::Adapt)
    , phase_start_cycle(0)
    , chosen(WarpSchedulePolicy::gCAWS)
    , decides_total(0)
    , decides_valid(0)
    , decides_skipped(0)
    , decides_woi_fallback(0)
    , decides_concave(0)
    , decides_convex(0)
    , decide_meanmax_accum(0.0)
    , decide_meanmax_min(1.0)
    , decide_meanmax_max(0.0)
    , woi_size_accum(0)
    , woi_size_min(UINT32_MAX)
    , woi_size_max(0)
    , gcaws_exec_cycles(0)
    , rr_exec_cycles(0)
    , wspawn_events(0)
    , recover_target(0)
    , recover_cycles(0)
    , recover_entries(0)
  {}
};

///////////////////////////////////////////////////////////////////////////////

// Per-warp criticality tracking for CAWA (Lee & Wu, ISCA 2015).
//   nCriticality = nInst * CPI_avg + nStall
//   nInst   : instruction-count disparity vs. fastest warp
//   CPI_avg : per-warp average CPI
//   nStall  : accumulated stall cycles (scoreboard + ibuffer)
struct warp_cpl_t {
  uint64_t instr_count; //이 warp가 누적 issue한 명령어 수
  uint64_t stall_cycles; //이 warp가 누적 stallgks cycle 수
  uint64_t criticality; //위 둘로 계산한 점수 (nInst * CPI + nStall)

  warp_cpl_t() : instr_count(0), stall_cycles(0), criticality(0) {} //kernel 전체 동안 누적-> 0 리셋안됨.
};

///////////////////////////////////////////////////////////////////////////////

class Emulator {
public:
  Emulator(const Arch &arch, const DCRS &dcrs, Core* core);

  ~Emulator();

  void reset();

  void attach_ram(RAM* ram);

#ifdef VM_ENABLE
  void set_satp(uint64_t satp) ;
#endif

  instr_trace_t* step();

  bool running() const;

  void suspend(uint32_t wid);

  void resume(uint32_t wid);

  bool barrier(uint32_t bar_id, uint32_t count, uint32_t wid);

  bool wspawn(uint32_t num_warps, Word nextPC);

  int get_exitcode() const;

  void dcache_read(void* data, uint64_t addr, uint32_t size);

  void dcache_write(const void* data, uint64_t addr, uint32_t size);

private: //메서드 선언만 -> 실제 로직은 emulator.cpp에 있음.

  uint32_t fetch(uint32_t wid, uint64_t uuid);

  int select_static_warp() const;

  int select_gto_warp();

  int select_rr_warp();

  int select_gcaws_warp();

  int select_ipaws_warp();

  int select_recover_warp();

  bool is_barrier_stalled(uint32_t wid) const;

  void ipaws_sample_and_step();

  void update_ready_timestamps();

  void update_cpl_counters();

  void decode(uint32_t code, uint32_t wid, uint64_t uuid);

  instr_trace_t* execute(const Instr &instr, uint32_t wid);

  void fetch_registers(std::vector<reg_data_t>& out, uint32_t wid, uint32_t src_index, const RegOpd& reg);

  void icache_read(void* data, uint64_t addr, uint32_t size);

  void dcache_amo_reserve(uint64_t addr);

  bool dcache_amo_check(uint64_t addr);

  void writeToStdOut(const void* data, uint64_t addr, uint32_t size);

  void cout_flush();

  Word get_csr(uint32_t addr, uint32_t wid, uint32_t tid);

  void set_csr(uint32_t addr, Word value, uint32_t wid, uint32_t tid);

  uint32_t get_fpu_rm(uint32_t funct3, uint32_t wid, uint32_t tid);

  void update_fcrs(uint32_t fflags, uint32_t wid, uint32_t tid);

  // temporarily added for riscv-vector tests
  // TODO: remove once ecall/ebreak are supported
  void trigger_ecall();
  void trigger_ebreak();

  const Arch& arch_;
  const DCRS& dcrs_;
  Core*       core_;

  std::vector<warp_t> warps_;
  WarpMask    active_warps_;
  WarpMask    stalled_warps_;
  WarpSchedulePolicy schedule_policy_;
  uint64_t    schedule_cycle_;
  uint32_t    kernel_id_;
  int         greedy_warp_;
  int         rr_last_warp_;
  int         critical_warp_; //gcaws가 직전에 고른 warp ( -1이면 없음 )
  int         last_scheduled_warp_; // 직전 cycle에 실제 실행된 warp (nStall scheduler delay 추적용)
  std::vector<uint64_t> ready_timestamps_;
  std::vector<warp_cpl_t> warp_cpl_; //warp별 criticality 카운터
  ipaws_state_t ipaws_state_; //ipaws 상태 통째로
  bool suppress_critical_push_;  // cacp 관련 : 지금은 flase로 고정
  std::vector<WarpMask> barriers_;
  std::unordered_map<int, std::stringstream> print_bufs_;
  MemoryUnit  mmu_;
  uint32_t    ipdom_size_;
  Word        csr_mscratch_;
  wspawn_t    wspawn_;

#ifdef EXT_TCU_ENABLE
  TensorUnit::Ptr tensor_unit_;
#endif

#ifdef EXT_V_ENABLE
  VecUnit::Ptr vec_unit_;
#endif

  PoolAllocator<Instr, 64> instr_pool_;
};

}

#endif
