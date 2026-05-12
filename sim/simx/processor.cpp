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
#include "socket.h"

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
    2,                        // pipeline latency
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
    });
    memsim_->MemRspPorts.at(i).tx_callback([&](const MemRsp&, uint64_t cycle){
      __unused (cycle);
      --perf_mem_pending_reads_;
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
  } while (!done);

  return exitcode;
}

void ProcessorImpl::reset() {
  perf_mem_reads_ = 0;
  perf_mem_writes_ = 0;
  perf_mem_latency_ = 0;
  perf_mem_pending_reads_ = 0;
}

void ProcessorImpl::dcr_write(uint32_t addr, uint32_t value) {
  dcrs_.write(addr, value);
}

int ProcessorImpl::mpm_query(uint32_t addr, uint32_t core_id, uint64_t* value) const {
  uint32_t offset = addr - VX_CSR_MPM_BASE;
  if (offset > 31 || core_id >= arch_.num_cores())
    return -1;

  uint32_t cores_per_cluster = NUM_SOCKETS * arch_.socket_size();
  uint32_t cluster_id = core_id / cores_per_cluster;
  uint32_t local_core_id = core_id % cores_per_cluster;
  uint32_t socket_id = local_core_id / arch_.socket_size();
  uint32_t socket_core_id = local_core_id % arch_.socket_size();

  auto cluster = clusters_.at(cluster_id).get();
  auto socket = cluster->socket(socket_id);
  auto core = socket->core(socket_core_id);

  auto core_perf = core->perf_stats();
  if (addr == VX_CSR_MCYCLE) {
    *value = core_perf.cycles;
    return 0;
  }
  if (addr == VX_CSR_MINSTRET) {
    *value = core_perf.instrs;
    return 0;
  }

  auto perf_class = dcrs_.base_dcrs.read(VX_DCR_BASE_MPM_CLASS);
  switch (perf_class) {
  case VX_DCR_MPM_CLASS_CORE:
    switch (addr) {
    case VX_CSR_MPM_SCHED_ID: *value = core_perf.sched_idle; return 0;
    case VX_CSR_MPM_SCHED_ST: *value = core_perf.sched_stalls; return 0;
    case VX_CSR_MPM_IBUF_ST: *value = core_perf.ibuf_stalls; return 0;
    case VX_CSR_MPM_SCRB_ST: *value = core_perf.scrb_stalls; return 0;
    case VX_CSR_MPM_OPDS_ST: *value = core_perf.opds_stalls; return 0;
    case VX_CSR_MPM_SCRB_ALU: *value = core_perf.scrb_alu; return 0;
    case VX_CSR_MPM_SCRB_FPU: *value = core_perf.scrb_fpu; return 0;
    case VX_CSR_MPM_SCRB_LSU: *value = core_perf.scrb_lsu; return 0;
    case VX_CSR_MPM_SCRB_SFU: *value = core_perf.scrb_sfu; return 0;
  #ifdef EXT_TCU_ENABLE
    case VX_CSR_MPM_SCRB_TCU: *value = core_perf.scrb_tcu; return 0;
  #endif
  #ifdef EXT_V_ENABLE
    case VX_CSR_MPM_SCRB_VPU: *value = core_perf.scrb_vpu; return 0;
  #endif
    case VX_CSR_MPM_SCRB_CSRS: *value = core_perf.scrb_csrs; return 0;
    case VX_CSR_MPM_SCRB_WCTL: *value = core_perf.scrb_wctl; return 0;
    case VX_CSR_MPM_IFETCHES: *value = core_perf.ifetches; return 0;
    case VX_CSR_MPM_LOADS: *value = core_perf.loads; return 0;
    case VX_CSR_MPM_STORES: *value = core_perf.stores; return 0;
    case VX_CSR_MPM_IFETCH_LT: *value = core_perf.ifetch_latency; return 0;
    case VX_CSR_MPM_LOAD_LT: *value = core_perf.load_latency; return 0;
    case VX_CSR_MPM_CCWS_VTA_INSERTS: *value = core_perf.ccws_vta_inserts; return 0;
    case VX_CSR_MPM_CCWS_VTA_HITS: *value = core_perf.ccws_vta_hits; return 0;
    case VX_CSR_MPM_CCWS_THROTTLED_LOADS: *value = core_perf.ccws_throttled_loads; return 0;
    case VX_CSR_MPM_CCWS_THROTTLED_WARPS: *value = core_perf.ccws_throttled_warps; return 0;
    case VX_CSR_MPM_CCWS_FALLBACK_ISSUES: *value = core_perf.ccws_fallback_issues; return 0;
    case VX_CSR_MPM_CCWS_AVG_ACTIVE_ISSUE_CANDIDATES: *value = core_perf.ccws_avg_active_issue_candidates; return 0;
    case VX_CSR_MPM_CCWS_AVG_LLS: *value = core_perf.ccws_avg_lls; return 0;
    case VX_CSR_MPM_CCWS_MAX_LLS: *value = core_perf.ccws_max_lls; return 0;
    default:
      break;
    }
    break;
  case VX_DCR_MPM_CLASS_MEM: {
    auto proc_perf = this->perf_stats();
    auto cluster_perf = cluster->perf_stats();
    auto socket_perf = socket->perf_stats();
    auto lmem_perf = core->local_mem()->perf_stats();
    uint64_t coalescer_misses = 0;
    for (uint32_t i = 0; i < NUM_LSU_BLOCKS; ++i) {
      coalescer_misses += core->mem_coalescer(i)->perf_stats().misses;
    }
    switch (addr) {
    case VX_CSR_MPM_ICACHE_READS: *value = socket_perf.icache.reads; return 0;
    case VX_CSR_MPM_ICACHE_MISS_R: *value = socket_perf.icache.read_misses; return 0;
    case VX_CSR_MPM_ICACHE_MSHR_ST: *value = socket_perf.icache.mshr_stalls; return 0;
    case VX_CSR_MPM_DCACHE_READS: *value = socket_perf.dcache.reads; return 0;
    case VX_CSR_MPM_DCACHE_WRITES: *value = socket_perf.dcache.writes; return 0;
    case VX_CSR_MPM_DCACHE_MISS_R: *value = socket_perf.dcache.read_misses; return 0;
    case VX_CSR_MPM_DCACHE_MISS_W: *value = socket_perf.dcache.write_misses; return 0;
    case VX_CSR_MPM_DCACHE_BANK_ST: *value = socket_perf.dcache.bank_stalls; return 0;
    case VX_CSR_MPM_DCACHE_MSHR_ST: *value = socket_perf.dcache.mshr_stalls; return 0;
    case VX_CSR_MPM_L2CACHE_READS: *value = cluster_perf.l2cache.reads; return 0;
    case VX_CSR_MPM_L2CACHE_WRITES: *value = cluster_perf.l2cache.writes; return 0;
    case VX_CSR_MPM_L2CACHE_MISS_R: *value = cluster_perf.l2cache.read_misses; return 0;
    case VX_CSR_MPM_L2CACHE_MISS_W: *value = cluster_perf.l2cache.write_misses; return 0;
    case VX_CSR_MPM_L2CACHE_BANK_ST: *value = cluster_perf.l2cache.bank_stalls; return 0;
    case VX_CSR_MPM_L2CACHE_MSHR_ST: *value = cluster_perf.l2cache.mshr_stalls; return 0;
    case VX_CSR_MPM_L3CACHE_READS: *value = proc_perf.l3cache.reads; return 0;
    case VX_CSR_MPM_L3CACHE_WRITES: *value = proc_perf.l3cache.writes; return 0;
    case VX_CSR_MPM_L3CACHE_MISS_R: *value = proc_perf.l3cache.read_misses; return 0;
    case VX_CSR_MPM_L3CACHE_MISS_W: *value = proc_perf.l3cache.write_misses; return 0;
    case VX_CSR_MPM_L3CACHE_BANK_ST: *value = proc_perf.l3cache.bank_stalls; return 0;
    case VX_CSR_MPM_L3CACHE_MSHR_ST: *value = proc_perf.l3cache.mshr_stalls; return 0;
    case VX_CSR_MPM_MEM_READS: *value = proc_perf.mem_reads; return 0;
    case VX_CSR_MPM_MEM_WRITES: *value = proc_perf.mem_writes; return 0;
    case VX_CSR_MPM_MEM_LT: *value = proc_perf.mem_latency; return 0;
    case VX_CSR_MPM_MEM_BANK_ST: *value = proc_perf.memsim.bank_stalls; return 0;
    case VX_CSR_MPM_LMEM_READS: *value = lmem_perf.reads; return 0;
    case VX_CSR_MPM_LMEM_WRITES: *value = lmem_perf.writes; return 0;
    case VX_CSR_MPM_LMEM_BANK_ST: *value = lmem_perf.bank_stalls; return 0;
    case VX_CSR_MPM_COALESCER_MISS: *value = coalescer_misses; return 0;
    default:
      break;
    }
  } break;
  default:
    break;
  }
  *value = 0;
  return 0;
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
