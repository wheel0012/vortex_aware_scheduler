# Vortex GPGPU

Vortex is a full-stack open-source RISC-V GPGPU. Vortex supports multiple **backend drivers**, including our C++ simulator (simx), an RTL simulator, and physical Xilinx and Altera FPGAs-- all controlled by a single driver script. The chosen driver determines the corresponding code invoked to run Vortex. Generally, developers will prototype their intended design in simx, before completing going forward with an RTL implementation. Alternatively, you can get up and running by selecting a driver of your choice and running a demo program.

## Website
Vortex news can be found on its [website](https://vortex.cc.gatech.edu/)

## Citation
```
@inproceedings{10.1145/3466752.3480128,
	author = {Tine, Blaise and Yalamarthy, Krishna Praveen and Elsabbagh, Fares and Hyesoon, Kim},
	title = {Vortex: Extending the RISC-V ISA for GPGPU and 3D-Graphics},
	year = {2021},
	isbn = {9781450385572},
	publisher = {Association for Computing Machinery},
	address = {New York, NY, USA},
	url = {https://doi.org/10.1145/3466752.3480128},
	doi = {10.1145/3466752.3480128},
	abstract = {The importance of open-source hardware and software has been increasing. However, despite GPUs being one of the more popular accelerators across various applications, there is very little open-source GPU infrastructure in the public domain. We argue that one of the reasons for the lack of open-source infrastructure for GPUs is rooted in the complexity of their ISA and software stacks. In this work, we first propose an ISA extension to RISC-V that supports GPGPUs and graphics. The main goal of the ISA extension proposal is to minimize the ISA changes so that the corresponding changes to the open-source ecosystem are also minimal, which makes for a sustainable development ecosystem. To demonstrate the feasibility of the minimally extended RISC-V ISA, we implemented the complete software and hardware stacks of Vortex on FPGA. Vortex is a PCIe-based soft GPU that supports OpenCL and OpenGL. Vortex can be used in a variety of applications, including machine learning, graph analytics, and graphics rendering. Vortex can scale up to 32 cores on an Altera Stratix 10 FPGA, delivering a peak performance of 25.6 GFlops at 200 Mhz.},
	booktitle = {MICRO-54: 54th Annual IEEE/ACM International Symposium on Microarchitecture},
	pages = {754–766},
	numpages = {13},
	keywords = {reconfigurable computing, memory systems., computer graphics},
	location = {Virtual Event, Greece},
	series = {MICRO '21}
}
```

## Specifications

- Support RISC-V RV32IMAF and RV64IMAFD

- Microarchitecture:
    - configurable number of cores, warps, and threads.
    - configurable number of ALU, FPU, LSU, and SFU units per core.
    - configurable pipeline issue width.
    - optional local memory, L1, L2, and L3 caches.
- Software:
    - OpenCL 1.2 Support.
- Supported FPGAs:
    - Altera Arria 10
    - Altera Stratix 10
    - Xilinx Alveo U50, U250, U280
    - Xilinx Versal VCK5000

## Directory structure

- `doc`: [Documentation](docs/index.md).
- `hw`: Hardware sources.
- `driver`: Host drivers repository.
- `runtime`: Kernel Runtime software.
- `sim`: Simulators repository.
- `tests`: Tests repository.
- `ci`: Continuous integration scripts.
- `miscs`: Miscellaneous resources.

## Quick Start
If you are interested in a stable release of Vortex, you can download the latest release [here](https://github.com/vortexgpgpu/vortex/releases/latest). Otherwise, you can pull the most recent, but (potentially) unstable version as shown below. The following steps demonstrate how to build and run Vortex with the default driver: SimX. If you are interested in a different backend, look [here](docs/simulation.md).

### Supported OS Platforms
- Ubuntu 18.04, 20.04, 22.04, 24.04
- Centos 7
### Toolchain Dependencies
The following dependencies will be fetched prebuilt by `toolchain_install.sh`.
- [POCL](http://portablecl.org/)
- [LLVM](https://llvm.org/)
- [RISCV-GNU-TOOLCHAIN](https://github.com/riscv-collab/riscv-gnu-toolchain)
- [Verilator](https://www.veripool.org/verilator)
- [cvfpu](https://github.com/openhwgroup/cvfpu.git)
- [SoftFloat](https://github.com/ucb-bar/berkeley-softfloat-3.git)
- [Ramulator](https://github.com/CMU-SAFARI/ramulator.git)
- [Yosys](https://github.com/YosysHQ/yosys)
- [Sv2v](https://github.com/zachjs/sv2v)
### Install Vortex codebase
```sh
	git clone --depth=1 --recursive https://github.com/vortexgpgpu/vortex.git
	cd vortex
```
### Install system dependencies
```sh
# ensure dependent libraries are present
sudo ./ci/install_dependencies.sh
```
### Configure your build folder
```sh
    mkdir build
    cd build
    # for 32bit
    ../configure --xlen=32 --tooldir=$HOME/tools
    # for 64bit
    ../configure --xlen=64 --tooldir=$HOME/tools
```
### Install prebuilt toolchain
```sh
   ./ci/toolchain_install.sh --all
```
### set environment variables
```sh
    # should always run before using the toolchain!
    source ./ci/toolchain_env.sh
```
### Building Vortex
```sh
make -s
```
### Quick demo running vecadd OpenCL kernel on 2 cores
```sh
./ci/blackbox.sh --cores=2 --app=vecadd
```

### Common Developer Tips
- Installing Vortex kernel and runtime libraries to use with external tools requires passing --prefix=<install-path> to the configure script.
```sh
../configure --xlen=32 --tooldir=$HOME/tools --prefix=<install-path>
make -s
make install
```
- Building Vortex 64-bit requires setting --xlen=64 configure option.
```sh
../configure --xlen=64 --tooldir=$HOME/tools
```
- Sourcing "./ci/toolchain_env.sh" is required everytime you start a new terminal. we recommend adding "source <build-path>/ci/toolchain_env.sh" to your ~/.bashrc file to automate the process at login.
```sh
echo "source <build-path>/ci/toolchain_env.sh" >> ~/.bashrc
```
- Making changes to Makefiles in your source tree or adding new folders will require executing the "configure" script again without any options to get changes propagated to your build folder.
```sh
../configure
```
- To debug the GPU, the simulation can generate a runtime trace for analysis. See /docs/debugging.md for more information.
```sh
./ci/blackbox.sh --app=demo --debug=3
```
- For additional information, check out the [documentation](docs/index.md)

## URP 실험 가이드 (simx)

### 정책 선택(코드에서 1줄 수정)
`sim/simx/emulator.cpp`의 `kDefaultSchedulePolicy` 값을 바꿔서 사용합니다.

- `WarpSchedulePolicy::Static` : 기존 priority 방식
- `WarpSchedulePolicy::RR` : round-robin
- `WarpSchedulePolicy::GTO` : greedy-then-oldest
- `WarpSchedulePolicy::gCAWS` : greedy criticality-aware (CAWA, Lee & Wu ISCA'15)

### 빌드
```sh
cd build
source ./ci/toolchain_env.sh
make -C sim/simx -j$(nproc)
```

### 실행 예시 (sgemm3)
```sh
./ci/blackbox.sh --driver=simx --app=sgemm3 --cores=32 --warps=32 --threads=32 --l2cache --perf=1
```

## URP 프로젝트 변경 사항 (CAWA 기반 iPAWS 구현)

본 프로젝트는 Vortex에 CAWA (Coordinated Criticality-Aware Warp Acceleration,
Lee & Wu, ISCA 2015) 와 iPAWS (Instruction-issue Pattern-based Adaptive Warp
Scheduling) 를 결합한 적응형 스케줄러를 구현하는 것을 목표로 합니다.

전체 로드맵
1. **Phase 1**: gCAWS warp 스케줄러
2. **Phase 2**: CACP 캐시 관리 (way reservation + SHiP-CB)
3. **Phase 3**: iPAWS 상태 기계 (gCAWS ↔ RR 적응)
4. **Phase 4**: RTL 구현
5. **Phase 5**: FPGA 평가

### Phase 1: gCAWS 스케줄러 (simx 에뮬레이터)

| 파일 | 변경 내용 |
|------|----------|
| `sim/simx/emulator.h` | `WarpSchedulePolicy::gCAWS` enum, `warp_cpl_t` 구조체(`instr_count`/`stall_cycles`/`criticality`), 멤버 `critical_warp_`, `warp_cpl_`, 메서드 `select_gcaws_warp()`, `update_cpl_counters()` 선언. |
| `sim/simx/emulator.cpp` | `update_cpl_counters()`로 매 사이클 stall/instr 카운터 갱신, 그리고 `nInst*CPI_avg + nStall` 공식으로 criticality 계산. `select_gcaws_warp()`는 greedy 단계(현 critical warp 유지) → highest-criticality + oldest-ready tie-break 순으로 선택. 기본 정책을 `gCAWS`로 설정. |

### Phase 2: CACP (CAWA의 캐시 관리 기법)

CACP는 두 메커니즘으로 구성됩니다.

1. **Way reservation (way partitioning)**: critical warp 전용으로 cache way의
   일부(`DCACHE_NUM_WAYS/2`)를 예약. non-critical warp은 reserved way에서 절대 evict 불가.
2. **SHiP-CB + RRIP 기반 교체 정책**: PC signature → SHCT(1024 × 3-bit 카운터) →
   삽입 시 RRPV(0=near, 2=long, 3=distant) 결정. 또 hit 시 SHCT++/reused=true,
   evict 시 reused=false이면 SHCT--.
3. **CACP 가중치**: critical warp의 insertion은 SHCT 예측을 덮어쓰고 `RRPV=0`(near)로
   강제하여 cache에 더 오래 머무르도록 함.

| 파일 | 변경 내용 |
|------|----------|
| `sim/simx/types.h` / `types.cpp` | `LsuReq` / `MemReq`에 `wid`, `pc` 필드 추가. `LsuMemAdapter::tick()`이 두 필드를 LsuReq → MemReq로 복사. |
| `sim/simx/func_unit.cpp` | LSU에서 `lsu_req.wid = trace->wid`, `lsu_req.pc = trace->PC` 설정. |
| `sim/simx/mem_coalescer.cpp` | coalesce 시 in_req의 `wid`/`pc`를 out_req로 복사. |
| `sim/simx/cache_sim.h` | `CacheSim::Config`에 `cacp_enable`, `cacp_reserved_ways` 필드. `set_critical_warp(int wid)` API 추가. |
| `sim/simx/cache_sim.cpp` | `line_t`를 LRU 카운터 대신 `signature`/`rrpv`/`reused`로 교체. SHCT 클래스 구현(1024 entry, 3-bit). `set_t::tag_lookup`를 RRIP 기반 victim selection + way-partition로 재작성. `CacheBank`에 `shct_`, `critical_warp_id_` 멤버. 핵심 후크: hit 시 SHCT++/RRPV=0, miss/evict 시 SHCT--, fill 시 critical/SHCT 예측에 따라 RRPV 결정. |
| `sim/simx/cache_cluster.h` | `set_critical_warp(int wid)`를 내부 모든 `CacheSim`으로 전파. |
| `sim/simx/socket.{h,cpp}` | dcache용 `set_critical_warp(int wid)`. dcache config에 `cacp_enable=(DCACHE_NUM_WAYS>=2)`, `cacp_reserved_ways=DCACHE_NUM_WAYS/2` 전달. |
| `sim/simx/core.{h,cpp}` | `Core::set_critical_warp(int wid)`가 socket으로 위임. `core.cpp`에 `socket.h` 포함. |
| `sim/simx/emulator.cpp` | `select_gcaws_warp()`에서 `critical_warp_`가 변경될 때마다 `core_->set_critical_warp(...)` 호출. |

### 기본 config (Vortex 논문 baseline)
- cores=1, warps=16, threads=16
- L1 D-cache: 16 KB, 4-way (기본) → CACP는 way=2를 critical에 예약
- 결과: BFS 4K 그래프 기준 `simx`가 정상 종료, 정합성 PASS.

### 빌드 & 실행 (CACP 켜진 상태)
```sh
cd build
make -C sim/simx -j$(nproc)
./ci/blackbox.sh --driver=simx --app=bfs --perf=2 --cores=1 --warps=16 --threads=16
```

