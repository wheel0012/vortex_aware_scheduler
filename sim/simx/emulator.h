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
  gCAWS,
  iPAWS
};

///////////////////////////////////////////////////////////////////////////////

// iPAWS adapts between criticality-aware (gCAWS+CACP) and uniform (RR)
// scheduling based on the runtime distribution of warp criticality.
//
//   - Adapt phase: sample the criticality distribution across active warps;
//                  count the fraction of warps that fall below
//                  (median_criticality * IPAWS_WOI_RATIO) — these are the
//                  Warps-of-Interest (WOI) for the criticality view.
//                  A high WOI fraction means a skewed (concave) pattern.
//   - Execute phase: run the chosen policy for IPAWS_EXECUTE_CYCLES.
enum class iPAWSPhase {
  Adapt,
  Execute
};

struct ipaws_state_t {
  iPAWSPhase           phase;
  uint64_t             phase_start_cycle;
  WarpSchedulePolicy   chosen;       // policy selected during Execute
  uint64_t             adapt_samples;
  double               adapt_woi_sum;  // sum of per-sample WOI ratios

  ipaws_state_t()
    : phase(iPAWSPhase::Adapt)
    , phase_start_cycle(0)
    , chosen(WarpSchedulePolicy::gCAWS)
    , adapt_samples(0)
    , adapt_woi_sum(0.0)
  {}
};

///////////////////////////////////////////////////////////////////////////////

// Per-warp criticality tracking for CAWA (Lee & Wu, ISCA 2015).
//   nCriticality = nInst * CPI_avg + nStall
//   nInst   : instruction-count disparity vs. fastest warp
//   CPI_avg : per-warp average CPI
//   nStall  : accumulated stall cycles (scoreboard + ibuffer)
struct warp_cpl_t {
  uint64_t instr_count;
  uint64_t stall_cycles;
  uint64_t criticality;

  warp_cpl_t() : instr_count(0), stall_cycles(0), criticality(0) {}
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

private:

  uint32_t fetch(uint32_t wid, uint64_t uuid);

  int select_static_warp() const;

  int select_gto_warp();

  int select_rr_warp();

  int select_gcaws_warp();

  int select_ipaws_warp();

  void ipaws_sample_and_step();

  double compute_woi_ratio() const;

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
  int         greedy_warp_;
  int         rr_last_warp_;
  int         critical_warp_;
  std::vector<uint64_t> ready_timestamps_;
  std::vector<warp_cpl_t> warp_cpl_;
  ipaws_state_t ipaws_state_;
  bool suppress_critical_push_;  // when true, gCAWS does not propagate critical_warp to caches
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
