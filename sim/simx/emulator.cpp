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
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <algorithm>
#include <limits>
#include <assert.h>
#include <util.h>

#include "emulator.h"
#include "arch.h"
#include "scheduler_ccws.h"
#include "instr_trace.h"
#include "instr.h"
#include "dcrs.h"
#include "core.h"
#include "socket.h"
#include "cluster.h"
#include "processor_impl.h"
#include "local_mem.h"

using namespace vortex;

namespace {

// Choose one schedule policy here for simx:
// WarpSchedulePolicy::Static
// WarpSchedulePolicy::RR
// WarpSchedulePolicy::GTO
// WarpSchedulePolicy::CCWS

#ifndef VX_SCHED_POLICY
#define VX_SCHED_POLICY WarpSchedulePolicy::Static
#endif 

constexpr WarpSchedulePolicy kDefaultSchedulePolicy = WarpSchedulePolicy::Static;

#ifndef VX_GTO_MSHR_AWARE
#define VX_GTO_MSHR_AWARE 0
#endif

#ifndef VX_GTO_MSHR_PRESSURE_NUM
#define VX_GTO_MSHR_PRESSURE_NUM 3
#endif

#ifndef VX_GTO_MSHR_PRESSURE_DEN
#define VX_GTO_MSHR_PRESSURE_DEN 4
#endif

#ifndef VX_GTO_MSHR_LOAD_COOLDOWN
#define VX_GTO_MSHR_LOAD_COOLDOWN 2
#endif


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

Emulator::Emulator(const vortex::Arch &arch, const DCRS &dcrs, Core* core)
    : arch_(arch)
    , dcrs_(dcrs)
    , core_(core)
    , warps_(arch.num_warps(), arch.num_threads())
    , schedule_policy_(VX_SCHED_POLICY)
    , schedule_cycle_(0)
    , greedy_warp_(-1)
    , rr_last_warp_(-1)
    , mshr_load_cooldown_ctr_(0)
    , ready_timestamps_(arch.num_warps(), 0)
    , barriers_(arch.num_barriers(), 0)
    , ipdom_size_(arch.num_threads()-1)
    , ccws_(new SchedulerCCWS(arch.num_warps()))
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

Emulator::~Emulator() {
  this->cout_flush();
  delete ccws_;
}

void Emulator::reset() {
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

  stalled_warps_.reset();
  active_warps_.reset();
  schedule_cycle_ = 0;
  greedy_warp_ = -1;
  rr_last_warp_ = -1;
  mshr_load_cooldown_ctr_ = 0;
  std::fill(ready_timestamps_.begin(), ready_timestamps_.end(), 0);
  ccws_->reset();

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
  bool mshr_pressured = false;
  if (VX_GTO_MSHR_AWARE) {
    uint32_t dcache_mshr_capacity = core_->socket()->dcache_mshr_capacity();
    uint32_t pressure_threshold = (dcache_mshr_capacity * VX_GTO_MSHR_PRESSURE_NUM) / VX_GTO_MSHR_PRESSURE_DEN;
    pressure_threshold = std::max<uint32_t>(pressure_threshold, 1);
    mshr_pressured = core_->socket()->is_dcache_mshr_pressured(pressure_threshold);
  }

  if (greedy_warp_ >= 0) {
    uint32_t wid = static_cast<uint32_t>(greedy_warp_);
    if (this->is_ready_warp(wid) && (!mshr_pressured || !this->warp_head_is_load(wid))) {
      return greedy_warp_;
    }
  }

  int selected_warp = -1;
  if (mshr_pressured) {
    // Under MSHR pressure, temporarily mask LOAD-headed warps.
    selected_warp = this->select_oldest_ready(-1, true);
    // Deadlock prevention: if all ready warps are LOAD-headed, allow the oldest.
    if (-1 == selected_warp) {
      if (mshr_load_cooldown_ctr_ < VX_GTO_MSHR_LOAD_COOLDOWN) {
        ++mshr_load_cooldown_ctr_;
        greedy_warp_ = -1;
        return -1;
      }
      mshr_load_cooldown_ctr_ = 0;
      selected_warp = this->select_oldest_ready(-1, false);
    }
  } else {
    mshr_load_cooldown_ctr_ = 0;
    selected_warp = this->select_oldest_ready(-1, false);
  }
  greedy_warp_ = selected_warp;
  return selected_warp;
}

bool Emulator::warp_head_is_load(uint32_t wid) {
  auto& warp = warps_.at(wid);
  if (!warp.ibuffer.empty()) {
    auto instr = warp.ibuffer.front();
    if (FUType::LSU != instr->getFUType())
      return false;

    auto op_type = instr->getOpType();
    if (auto lsu_type = std::get_if<LsuType>(&op_type)) {
      return (*lsu_type == LsuType::LOAD);
    }
#ifdef EXT_V_ENABLE
    if (auto vls_type = std::get_if<VlsType>(&op_type)) {
      return (*vls_type == VlsType::VL || *vls_type == VlsType::VLS || *vls_type == VlsType::VLX);
    }
#endif
    return false;
  }

  // If ibuffer is empty, predecode next opcode at PC without affecting core perf counters.
  uint32_t instr_code = 0;
#ifdef VM_ENABLE
  mmu_.read(&instr_code, warp.PC, sizeof(uint32_t), ACCESS_TYPE::FETCH);
#else
  mmu_.read(&instr_code, warp.PC, sizeof(uint32_t), 0);
#endif

  auto opcode = static_cast<Opcode>(instr_code & ((1u << width_opcode) - 1));
  return (opcode == Opcode::L || opcode == Opcode::FL);
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

int Emulator::select_ccws_warp() {
  uint64_t ready_count = this->count_ready_warps();

  if (ready_count > 0) {
    ccws_->record_issue_candidates(ready_count);
  }

  int selected_warp = this->select_oldest_ready(-1, false, false);

  if (selected_warp < 0) {
    // ready warp가 있을 때만 CCWS fallback으로 기록
    if (ready_count > 0) {
      ccws_->record_fallback_issue();
    }

    selected_warp = this->select_oldest_ready(-1, false, true);
  }

  return selected_warp;
}

bool Emulator::is_ready_warp(uint32_t wid) const {
  return active_warps_.test(wid) && !stalled_warps_.test(wid);
}

uint64_t Emulator::count_ready_warps() const {
  uint64_t count = 0;

  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (this->is_ready_warp(static_cast<uint32_t>(wid))) {
      ++count;
    }
  }

  return count;
}

int Emulator::select_oldest_ready(int excluded_warp,
                                  bool skip_load_warps,
                                  bool allow_blocked_loads) {
  int selected_warp = -1;
  uint64_t oldest_timestamp = std::numeric_limits<uint64_t>::max();
  bool use_ccws = (WarpSchedulePolicy::CCWS == schedule_policy_);

  for (size_t wid = 0, nw = arch_.num_warps(); wid < nw; ++wid) {
    if (static_cast<int>(wid) == excluded_warp)
      continue;

    if (!this->is_ready_warp(static_cast<uint32_t>(wid)))
      continue;

    bool head_is_load = this->warp_head_is_load(static_cast<uint32_t>(wid));

    if (skip_load_warps && head_is_load)
      continue;

    bool ccws_blocked = false;
    if (use_ccws && !allow_blocked_loads) {
#if VX_CCWS_GATE_WHOLE_WARP
      ccws_blocked = !ccws_->can_issue_warp(wid);
      if (ccws_blocked) {
        ccws_->record_throttled_warp();
      }
#else
      ccws_blocked = head_is_load && !ccws_->can_issue_load(wid);
#endif
    }

    if (ccws_blocked) {
      if (head_is_load)
        ccws_->record_throttled_load();
      continue;
    }

    auto ts = ready_timestamps_.at(wid);
    if (ts == 0)
      continue;

    if (ts < oldest_timestamp) {
      oldest_timestamp = ts;
      selected_warp = static_cast<int>(wid);
    }
  }

  return selected_warp;
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
  }

  if (WarpSchedulePolicy::GTO == schedule_policy_ 
    || WarpSchedulePolicy::CCWS == schedule_policy_) { // update ready timestamps for GTO and CCWS 
    ++schedule_cycle_;
    update_ready_timestamps();
    if (WarpSchedulePolicy::CCWS == schedule_policy_) {
      ccws_->set_active_warps(active_warps_.count());
      ccws_->tick();
    }
  }

  // find next ready warp according to policy
  switch (schedule_policy_) {
  case WarpSchedulePolicy::Static:
    scheduled_warp = select_static_warp();
    break;
  case WarpSchedulePolicy::GTO:
    scheduled_warp = select_gto_warp();
    break;
  case WarpSchedulePolicy::CCWS:
    scheduled_warp = select_ccws_warp();
    break;
  case WarpSchedulePolicy::RR:
    scheduled_warp = select_rr_warp();
    break;
  default:
    assert(false);
  }

  if (scheduled_warp == -1)
    return nullptr;

  if (WarpSchedulePolicy::CCWS == schedule_policy_) {
    ccws_->record_issue();
  }

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

SchedulerCCWS::PerfStats Emulator::ccws_perf_stats() const {
  return ccws_->perf_stats();
}

void Emulator::ccws_on_l1_miss(uint32_t wid, uint64_t line_addr) {
  if (WarpSchedulePolicy::CCWS == schedule_policy_) {
    ccws_->on_l1_miss(wid, line_addr);
  }
}

void Emulator::ccws_on_l1_eviction(uint32_t wid, uint64_t line_addr) {
  if (WarpSchedulePolicy::CCWS == schedule_policy_) {
    ccws_->on_l1_eviction(wid, line_addr);
  }
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
#ifdef PERF_ENABLE
        CSR_READ_64(VX_CSR_MPM_CCWS_VTA_INSERTS, core_perf.ccws_vta_inserts);
        CSR_READ_64(VX_CSR_MPM_CCWS_VTA_HITS, core_perf.ccws_vta_hits);
        CSR_READ_64(VX_CSR_MPM_CCWS_THROTTLED_LOADS, core_perf.ccws_throttled_loads);
        CSR_READ_64(VX_CSR_MPM_CCWS_THROTTLED_WARPS, core_perf.ccws_throttled_warps);
        CSR_READ_64(VX_CSR_MPM_CCWS_FALLBACK_ISSUES, core_perf.ccws_fallback_issues);
        CSR_READ_64(VX_CSR_MPM_CCWS_AVG_ACTIVE_ISSUE_CANDIDATES, core_perf.ccws_avg_active_issue_candidates);
        CSR_READ_64(VX_CSR_MPM_CCWS_AVG_LLS, core_perf.ccws_avg_lls);
        CSR_READ_64(VX_CSR_MPM_CCWS_MAX_LLS, core_perf.ccws_max_lls);
#endif
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
