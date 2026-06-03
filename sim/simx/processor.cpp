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

#include "processor.h"
#include "processor_impl.h"
#include <iomanip>
#include <iostream>

using namespace vortex;

ProcessorImpl::ProcessorImpl(const Arch& arch)
  : arch_(arch)
  , clusters_(arch.num_clusters())
{
  SimPlatform::instance().initialize();

	assert(PLATFORM_MEMORY_DATA_SIZE == MEM_BLOCK_SIZE);

  // create memory simulator
  memsim_ = MemSim::Create("dram", MemSim::Config{
    PLATFORM_MEMORY_NUM_BANKS,
    L3_MEM_PORTS,
    MEM_BLOCK_SIZE,
    MEM_CLOCK_RATIO
  });

  // create clusters
  for (uint32_t i = 0; i < arch.num_clusters(); ++i) {
    clusters_.at(i) = Cluster::Create(i, this, arch, dcrs_);
  }

  // create L3 cache
  l3cache_ = CacheSim::Create("l3cache", CacheSim::Config{
    !L3_ENABLED,
    log2ceil(L3_CACHE_SIZE),  // C
    log2ceil(MEM_BLOCK_SIZE), // L
    log2ceil(L2_LINE_SIZE),   // W
    log2ceil(L3_NUM_WAYS),    // A
    log2ceil(L3_NUM_BANKS),   // B
    XLEN,                     // address bits
    L3_NUM_REQS,              // request size
    L3_MEM_PORTS,             // memory ports
    L3_WRITEBACK,             // write-back
    false,                    // write response
    L3_MSHR_SIZE,             // mshr size
    SIMX_CACHE_LATENCY,       // pipeline latency
    MemCacheLevelL3,          // cache level
    }
  );

  // connect L3 core interfaces
  for (uint32_t i = 0; i < arch.num_clusters(); ++i) {
    for (uint32_t j = 0; j < L2_MEM_PORTS; ++j) {
      clusters_.at(i)->mem_req_ports.at(j).bind(&l3cache_->CoreReqPorts.at(i * L2_MEM_PORTS + j));
      l3cache_->CoreRspPorts.at(i * L2_MEM_PORTS + j).bind(&clusters_.at(i)->mem_rsp_ports.at(j));
    }
  }

  // connect L3 memory interfaces
  for (uint32_t i = 0; i < L3_MEM_PORTS; ++i) {
    l3cache_->MemReqPorts.at(i).bind(&memsim_->MemReqPorts.at(i));
    memsim_->MemRspPorts.at(i).bind(&l3cache_->MemRspPorts.at(i));
  }

  // set up memory profiling
  for (uint32_t i = 0; i < L3_MEM_PORTS; ++i) {
    memsim_->MemReqPorts.at(i).tx_callback([&](const MemReq& req, uint64_t cycle){
      __unused (cycle);
      perf_mem_reads_  += !req.write;
      perf_mem_writes_ += req.write;
      perf_mem_pending_reads_ += !req.write;
      if (req.userpc) {
        perf_userpc_mem_reads_ += !req.write;
        perf_userpc_mem_writes_ += req.write;
        perf_userpc_mem_pending_reads_ += !req.write;
      }
    });
    memsim_->MemRspPorts.at(i).tx_callback([&](const MemRsp& rsp, uint64_t cycle){
      __unused (cycle);
      --perf_mem_pending_reads_;
      if (rsp.userpc && perf_userpc_mem_pending_reads_ != 0)
        --perf_userpc_mem_pending_reads_;
    });
  }

#ifndef NDEBUG
  // dump device configuration
  std::cout << "CONFIGS:"
            << " num_threads=" << arch.num_threads()
            << ", num_warps=" << arch.num_warps()
            << ", num_cores=" << arch.num_cores()
            << ", num_clusters=" << arch.num_clusters()
            << ", socket_size=" << arch.socket_size()
            << ", local_mem_base=0x" << std::hex << arch.local_mem_base() << std::dec
            << ", num_barriers=" << arch.num_barriers()
            << std::endl;
#endif
  // reset the device
  this->reset();
}

ProcessorImpl::~ProcessorImpl() {
  if (dcrs_.base_dcrs.read(VX_DCR_BASE_MPM_CLASS) == VX_DCR_MPM_CLASS_MEM) {
    auto cerr_flags = std::cerr.flags();
    auto cerr_precision = std::cerr.precision();
    MemCoalescer::PerfStats coalescer_perf;
    for (auto& cluster : clusters_) {
      coalescer_perf += cluster->coalescer_perf_stats();
    }
    auto read_avg = coalescer_perf.read_outputs
                  ? double(coalescer_perf.read_inputs) / coalescer_perf.read_outputs
                  : 0.0;
    auto write_avg = coalescer_perf.write_outputs
                   ? double(coalescer_perf.write_inputs) / coalescer_perf.write_outputs
                   : 0.0;
    std::cerr << std::fixed << std::setprecision(2)
              << "PERF: coalescer average request read=" << read_avg
              << " write=" << write_avg << "\n";

    auto mem_perf = memsim_->perf_stats();
    if (mem_perf.cycles != 0 && !mem_perf.bank_requests.empty()) {
      std::cerr << "PERF: memory bank activity";
      for (uint32_t i = 0; i < mem_perf.bank_requests.size(); ++i) {
        auto activity = 100.0 * double(mem_perf.bank_requests.at(i)) / mem_perf.cycles;
        std::cerr << " bank" << i << "=" << activity << "%";
      }
      std::cerr << "\n";
      std::cerr << "PERF: memory bank conflicts";
      for (uint32_t i = 0; i < mem_perf.bank_conflicts.size(); ++i) {
        auto requests = i < mem_perf.bank_requests.size() ? mem_perf.bank_requests.at(i) : 0;
        auto conflicts = mem_perf.bank_conflicts.at(i);
        auto pressure = (requests + conflicts)
                      ? 100.0 * double(conflicts) / double(requests + conflicts)
                      : 0.0;
        std::cerr << " bank" << i << "=" << conflicts << "(" << pressure << "%)";
      }
      std::cerr << "\n";
    }
    for (auto& cluster : clusters_) {
      cluster->dump_cache_bank_activity(std::cerr);
    }
    std::cerr.flags(cerr_flags);
    std::cerr.precision(cerr_precision);
  }

  auto userpc_mem_requests = perf_userpc_mem_reads_ + perf_userpc_mem_writes_;
  if (userpc_mem_requests != 0) {
    auto avg_latency = perf_userpc_mem_reads_ ? double(perf_userpc_mem_latency_) / perf_userpc_mem_reads_ : 0.0;
    std::cerr << "PERF: userpc memory requests=" << userpc_mem_requests
              << " (reads=" << perf_userpc_mem_reads_
              << ", writes=" << perf_userpc_mem_writes_ << ")\n";
    std::cerr << "PERF: userpc memory latency=" << avg_latency << " cycles\n";
  }
  SimPlatform::instance().finalize();
}

void ProcessorImpl::attach_ram(RAM* ram) {
  for (auto cluster : clusters_) {
    cluster->attach_ram(ram);
  }
}
#ifdef VM_ENABLE
void ProcessorImpl::set_satp(uint64_t satp) {
  for (auto cluster : clusters_) {
    cluster->set_satp(satp);
  }
}
#endif

int ProcessorImpl::run() {
  SimPlatform::instance().reset();
  this->reset();

  bool done;
  int exitcode = 0;
  do {
    SimPlatform::instance().tick();
    done = true;
    for (auto cluster : clusters_) {
      if (cluster->running()) {
        done = false;
        continue;
      }
      exitcode |= cluster->get_exitcode();
    }
    perf_mem_latency_ += perf_mem_pending_reads_;
    perf_userpc_mem_latency_ += perf_userpc_mem_pending_reads_;
  } while (!done);

  return exitcode;
}

void ProcessorImpl::reset() {
  perf_mem_reads_ = 0;
  perf_mem_writes_ = 0;
  perf_mem_latency_ = 0;
  perf_mem_pending_reads_ = 0;
  perf_userpc_mem_reads_ = 0;
  perf_userpc_mem_writes_ = 0;
  perf_userpc_mem_latency_ = 0;
  perf_userpc_mem_pending_reads_ = 0;
}

void ProcessorImpl::dcr_write(uint32_t addr, uint32_t value) {
  dcrs_.write(addr, value);
}

int ProcessorImpl::mpm_query(uint32_t addr, uint32_t core_id, uint64_t* value) const {
  auto read64 = [&](uint32_t csr_addr, uint64_t csr_value) -> bool {
    if (addr == csr_addr) {
      *value = csr_value & 0xffffffff;
      return true;
    }
    if (addr == (csr_addr + (VX_CSR_MPM_BASE_H - VX_CSR_MPM_BASE))) {
      *value = (csr_value >> 32) & 0xffffffff;
      return true;
    }
    return false;
  };

  uint32_t cores_per_cluster = arch_.num_cores();
  uint32_t cluster_id = core_id / cores_per_cluster;
  uint32_t local_core_id = core_id % cores_per_cluster;
  if (cluster_id >= clusters_.size())
    return -1;

  auto cluster = clusters_.at(cluster_id);
  auto& core_perf = cluster->core_perf_stats(local_core_id);
  if (read64(VX_CSR_MCYCLE, core_perf.cycles))
    return 0;
  if (read64(VX_CSR_MINSTRET, core_perf.instrs))
    return 0;

  auto perf_class = dcrs_.base_dcrs.read(VX_DCR_BASE_MPM_CLASS);
  switch (perf_class) {
  case VX_DCR_MPM_CLASS_NONE:
    *value = 0;
    return 0;
  case VX_DCR_MPM_CLASS_CORE:
    if (read64(VX_CSR_MPM_SCHED_ID, core_perf.sched_idle)) return 0;
    if (read64(VX_CSR_MPM_SCHED_ST, core_perf.sched_stalls)) return 0;
    if (read64(VX_CSR_MPM_IBUF_ST, core_perf.ibuf_stalls)) return 0;
    if (read64(VX_CSR_MPM_SCRB_ST, core_perf.scrb_stalls)) return 0;
    if (read64(VX_CSR_MPM_OPDS_ST, core_perf.opds_stalls)) return 0;
    if (read64(VX_CSR_MPM_SCRB_ALU, core_perf.scrb_alu)) return 0;
    if (read64(VX_CSR_MPM_SCRB_FPU, core_perf.scrb_fpu)) return 0;
    if (read64(VX_CSR_MPM_SCRB_LSU, core_perf.scrb_lsu)) return 0;
    if (read64(VX_CSR_MPM_SCRB_SFU, core_perf.scrb_sfu)) return 0;
  #ifdef EXT_TCU_ENABLE
    if (read64(VX_CSR_MPM_SCRB_TCU, core_perf.scrb_tcu)) return 0;
  #endif
  #ifdef EXT_V_ENABLE
    if (read64(VX_CSR_MPM_SCRB_VPU, core_perf.scrb_vpu)) return 0;
  #endif
    if (read64(VX_CSR_MPM_SCRB_CSRS, core_perf.scrb_csrs)) return 0;
    if (read64(VX_CSR_MPM_SCRB_WCTL, core_perf.scrb_wctl)) return 0;
    if (read64(VX_CSR_MPM_IFETCHES, core_perf.ifetches)) return 0;
    if (read64(VX_CSR_MPM_LOADS, core_perf.loads)) return 0;
    if (read64(VX_CSR_MPM_STORES, core_perf.stores)) return 0;
    if (read64(VX_CSR_MPM_IFETCH_LT, core_perf.ifetch_latency)) return 0;
    if (read64(VX_CSR_MPM_LOAD_LT, core_perf.load_latency)) return 0;
    break;
  case VX_DCR_MPM_CLASS_MEM: {
    auto proc_perf = this->perf_stats();
    auto cluster_perf = cluster->perf_stats();
    auto socket_perf = cluster->socket_perf_stats(local_core_id);
    auto lmem_perf = cluster->local_mem_perf_stats(local_core_id);
    auto coalescer_misses = cluster->coalescer_misses(local_core_id);

    if (read64(VX_CSR_MPM_ICACHE_READS, socket_perf.icache.reads)) return 0;
    if (read64(VX_CSR_MPM_ICACHE_MISS_R, socket_perf.icache.read_misses)) return 0;
    if (read64(VX_CSR_MPM_ICACHE_MSHR_ST, socket_perf.icache.mshr_stalls)) return 0;
    if (read64(VX_CSR_MPM_DCACHE_READS, socket_perf.dcache.reads)) return 0;
    if (read64(VX_CSR_MPM_DCACHE_WRITES, socket_perf.dcache.writes)) return 0;
    if (read64(VX_CSR_MPM_DCACHE_MISS_R, socket_perf.dcache.read_misses)) return 0;
    if (read64(VX_CSR_MPM_DCACHE_MISS_W, socket_perf.dcache.write_misses)) return 0;
    if (read64(VX_CSR_MPM_DCACHE_BANK_ST, socket_perf.dcache.bank_stalls)) return 0;
    if (read64(VX_CSR_MPM_DCACHE_MSHR_ST, socket_perf.dcache.mshr_stalls)) return 0;
    if (read64(VX_CSR_MPM_L2CACHE_READS, cluster_perf.l2cache.reads)) return 0;
    if (read64(VX_CSR_MPM_L2CACHE_WRITES, cluster_perf.l2cache.writes)) return 0;
    if (read64(VX_CSR_MPM_L2CACHE_MISS_R, cluster_perf.l2cache.read_misses)) return 0;
    if (read64(VX_CSR_MPM_L2CACHE_MISS_W, cluster_perf.l2cache.write_misses)) return 0;
    if (read64(VX_CSR_MPM_L2CACHE_BANK_ST, cluster_perf.l2cache.bank_stalls)) return 0;
    if (read64(VX_CSR_MPM_L2CACHE_MSHR_ST, cluster_perf.l2cache.mshr_stalls)) return 0;
    if (read64(VX_CSR_MPM_L3CACHE_READS, proc_perf.l3cache.reads)) return 0;
    if (read64(VX_CSR_MPM_L3CACHE_WRITES, proc_perf.l3cache.writes)) return 0;
    if (read64(VX_CSR_MPM_L3CACHE_MISS_R, proc_perf.l3cache.read_misses)) return 0;
    if (read64(VX_CSR_MPM_L3CACHE_MISS_W, proc_perf.l3cache.write_misses)) return 0;
    if (read64(VX_CSR_MPM_L3CACHE_BANK_ST, proc_perf.l3cache.bank_stalls)) return 0;
    if (read64(VX_CSR_MPM_L3CACHE_MSHR_ST, proc_perf.l3cache.mshr_stalls)) return 0;
    if (read64(VX_CSR_MPM_MEM_READS, proc_perf.mem_reads)) return 0;
    if (read64(VX_CSR_MPM_MEM_WRITES, proc_perf.mem_writes)) return 0;
    if (read64(VX_CSR_MPM_MEM_LT, proc_perf.mem_latency)) return 0;
    if (read64(VX_CSR_MPM_MEM_BANK_ST, proc_perf.memsim.bank_stalls)) return 0;
    if (read64(VX_CSR_MPM_COALESCER_MISS, coalescer_misses)) return 0;
    if (read64(VX_CSR_MPM_LMEM_READS, lmem_perf.reads)) return 0;
    if (read64(VX_CSR_MPM_LMEM_WRITES, lmem_perf.writes)) return 0;
    if (read64(VX_CSR_MPM_LMEM_BANK_ST, lmem_perf.bank_stalls)) return 0;
  } break;
  default:
    return -1;
  }

  return -1;
}

ProcessorImpl::PerfStats ProcessorImpl::perf_stats() const {
  ProcessorImpl::PerfStats perf;
  perf.mem_reads   = perf_mem_reads_;
  perf.mem_writes  = perf_mem_writes_;
  perf.mem_latency = perf_mem_latency_;
  perf.l3cache     = l3cache_->perf_stats();
  perf.memsim      = memsim_->perf_stats();
  return perf;
}

///////////////////////////////////////////////////////////////////////////////

Processor::Processor(const Arch& arch)
  : impl_(new ProcessorImpl(arch))
{
#ifdef VM_ENABLE
  satp_ = NULL;
#endif
}

Processor::~Processor() {
  delete impl_;
#ifdef VM_ENABLE
  if (satp_ != NULL)
    delete satp_;
#endif
}

void Processor::attach_ram(RAM* mem) {
  impl_->attach_ram(mem);
}

int Processor::run() {
  try {
    return impl_->run();
  } catch (const std::exception& e) {
    std::cerr << "Error: exception: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "Error: unknown exception." << std::endl;
  }
  return -1;
}

void Processor::dcr_write(uint32_t addr, uint32_t value) {
  return impl_->dcr_write(addr, value);
}

int Processor::mpm_query(uint32_t addr, uint32_t core_id, uint64_t* value) const {
  return impl_->mpm_query(addr, core_id, value);
}

#ifdef VM_ENABLE
int16_t Processor::set_satp_by_addr(uint64_t base_addr) {
  uint16_t asid = 0;
  satp_ = new SATP_t (base_addr,asid);
  if (satp_ == NULL)
    return 1;
  uint64_t satp = satp_->get_satp();
  impl_->set_satp(satp);
  return 0;
}
bool Processor::is_satp_unset() {
  return (satp_== NULL);
}
uint8_t Processor::get_satp_mode() {
  assert (satp_!=NULL);
  return satp_->get_mode();
}
uint64_t Processor::get_base_ppn() {
  assert (satp_!=NULL);
  return satp_->get_base_ppn();
}
#endif
