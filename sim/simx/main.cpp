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
#include <string>
#include <sstream>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include "processor.h"
#include "mem.h"
#include "constants.h"
#include <util.h>
#include "core.h"
#include "warp_sched_trace.h"
#include "VX_types.h"

using namespace vortex;

static void show_usage() {
   std::cout << "Usage: [-c <cores>] [-w <warps>] [-t <threads>] [-v: vector-test] [-s: stats] [-h: help]\n"
                "       [--trace-warp-sched[=FILE]] [--trace-warp-sched-file FILE]\n"
                "       [--warp-sched-policy NAME] <program>" << std::endl;
}

uint32_t num_threads = NUM_THREADS;
uint32_t num_warps = NUM_WARPS;
uint32_t num_cores = NUM_CORES;
bool showStats = false;
bool vector_test = false;
bool trace_warp_sched = false;
std::string trace_warp_sched_file = "issue_trace.csv";
std::string warp_sched_policy;
const char* program = nullptr;

static std::string default_warp_sched_policy() {
  std::ostringstream os;
  os << configured_issue_arbiter();
  return os.str();
}

static void parse_args(int argc, char **argv) {
    enum {
      OPT_TRACE_WARP_SCHED = 1000,
      OPT_TRACE_WARP_SCHED_FILE,
      OPT_WARP_SCHED_POLICY
    };
    static struct option long_options[] = {
      {"trace-warp-sched", optional_argument, 0, OPT_TRACE_WARP_SCHED},
      {"trace-warp-sched-file", required_argument, 0, OPT_TRACE_WARP_SCHED_FILE},
      {"warp-sched-policy", required_argument, 0, OPT_WARP_SCHED_POLICY},
      {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:w:c:vsh", long_options, nullptr)) != -1) {
      switch (c) {
      case 't':
        num_threads = atoi(optarg);
        break;
      case 'w':
        num_warps = atoi(optarg);
        break;
      case 'c':
        num_cores = atoi(optarg);
        break;
      case 'v':
        vector_test = true;
        break;
      case 's':
        showStats = true;
        break;
      case OPT_TRACE_WARP_SCHED:
        trace_warp_sched = true;
        if (optarg) {
          trace_warp_sched_file = optarg;
        }
        break;
      case OPT_TRACE_WARP_SCHED_FILE:
        trace_warp_sched = true;
        trace_warp_sched_file = optarg;
        break;
      case OPT_WARP_SCHED_POLICY:
        warp_sched_policy = optarg;
        break;
      case 'h':
        show_usage();
        exit(0);
        break;
      default:
        show_usage();
        exit(-1);
      }
    }

    if (optind < argc) {
      program = argv[optind];
    std::cout << "Running " << program << "..." << std::endl;
    } else {
      show_usage();
    exit(-1);
    }
}

int main(int argc, char **argv) {
  int exitcode = 0;

  parse_args(argc, argv);
  if (trace_warp_sched) {
    WarpSchedTrace::configure(true,
                              trace_warp_sched_file,
                              warp_sched_policy.empty() ? default_warp_sched_policy() : warp_sched_policy);
  }

  {
    // create processor configuation
    Arch arch(num_threads, num_warps, num_cores);

    // create memory module
    RAM ram(0, MEM_PAGE_SIZE);

    // create processor
    Processor processor(arch);

    // attach memory module
    processor.attach_ram(&ram);

	  // setup base DCRs
    const uint64_t startup_addr(STARTUP_ADDR);
    processor.dcr_write(VX_DCR_BASE_STARTUP_ADDR0, startup_addr & 0xffffffff);
  #if (XLEN == 64)
    processor.dcr_write(VX_DCR_BASE_STARTUP_ADDR1, startup_addr >> 32);
  #endif
	  processor.dcr_write(VX_DCR_BASE_MPM_CLASS, 0);

    // load program
    {
      std::string program_ext(fileExtension(program));
      if (program_ext == "bin") {
        ram.loadBinImage(program, startup_addr);
      } else if (program_ext == "hex") {
        ram.loadHexImage(program);
      } else {
        std::cerr << "Error: only *.bin or *.hex images supported." << std::endl;
        return -1;
      }
    }
  #ifndef NDEBUG
    std::cout << "[VXDRV] START: program=" << program << std::endl;
  #endif
    // run simulation
  #ifdef EXT_V_ENABLE
    // vector test exitcode is a special case
    if (vector_test) return (processor.run() != 1);
  #endif
    // else continue as normal
    processor.run();

    // read exitcode from @MPM.1
    ram.read(&exitcode, (IO_MPM_ADDR + 8), 4);
  }

  return exitcode;
}
