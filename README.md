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
- `WarpSchedulePolicy::CCWS` : cache-conscious warp scheduling

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

### Rodinia benchmark 실행
Rodinia에서 가져온 OpenCL benchmark도 `tests/opencl/<app>` 아래에 있으면 `./ci/blackbox.sh`의 `--app`으로 실행할 수 있습니다. 각 benchmark의 기본 인자는 해당 디렉터리의 `Makefile` 또는 `makefile`에 있는 `OPTS ?= ...` 값을 사용합니다.

```sh
./ci/blackbox.sh --driver=simx --app=hotspot --cores=32 --warps=32 --threads=32 --l2cache --perf=1
./ci/blackbox.sh --driver=simx --app=bfs --cores=32 --warps=32 --threads=32 --l2cache --perf=1
./ci/blackbox.sh --driver=simx --app=kmeans --cores=32 --warps=32 --threads=32 --l2cache --perf=1
```

입력 크기나 입력 파일을 바꾸고 싶으면 `--args="..."`를 사용합니다. `--args`의 문자열은 그대로 benchmark 실행 인자로 전달됩니다. 상대경로는 `make -C tests/opencl/<app>`로 실행되므로 해당 benchmark 디렉터리 기준입니다.

```sh
# hotspot: <grid_size> <pyramid_height> <iterations> <temp_file> <power_file> <output_file>
./ci/blackbox.sh --driver=simx --app=hotspot --args="64 1 2 temp_64 power_64 output.out" --cores=32 --warps=32 --threads=32 --l2cache --perf=1

# bfs: <graph_file>
./ci/blackbox.sh --driver=simx --app=bfs --args="./graph4k.txt" --cores=32 --warps=32 --threads=32 --l2cache --perf=1

# spmv: -i <matrix_file>,<vector_file>
./ci/blackbox.sh --driver=simx --app=spmv --args="-i ./1138_bus.mtx,./1138_bus.vec" --cores=32 --warps=32 --threads=32 --l2cache --perf=1

# b+tree: file <input_file> command <command_file>
./ci/blackbox.sh --driver=simx --app=b+tree --args="file btree-smoke.input command btree-smoke.command" --cores=32 --warps=32 --threads=32 --l2cache --perf=1

# srad: <iterations> <lambda> <rows> <cols>
./ci/blackbox.sh --driver=simx --app=srad --args="1 0.5 16 16" --cores=32 --warps=32 --threads=32 --l2cache --perf=1
```

입력 파일이 benchmark 디렉터리 밖에 있으면 절대경로를 쓰거나, benchmark 디렉터리 기준 상대경로를 지정합니다.

```sh
./ci/blackbox.sh --driver=simx --app=bfs --args="/home/user/datasets/graph.txt" --cores=32 --warps=32 --threads=32 --l2cache
./ci/blackbox.sh --driver=simx --app=hotspot --args="1024 1 2 ../../datasets/temp_1024 ../../datasets/power_1024 output.out" --cores=32 --warps=32 --threads=32 --l2cache
```

### MSHR-aware(GTO)
`GTO` 스케줄러에 MSHR 압박 신호를 결합한 policy

- 동작 원리:
  - MSHR 압박 조건(`occupancy >= capacity * NUM / DEN`)이 참이면, head 명령이 `LOAD`인 ready warp를 임시 masking
  - 가능한 경우 `non-load` warp를 우선 스케줄링해 LSU 포화를 완화
  - 압박 상태에서 ready warp가 모두 `LOAD`이면, `LOAD_COOLDOWN` 주기마다 oldest load warp 1개를 허용해 진행 보장

- 기본값(현재 코드 기준):
  - `VX_GTO_MSHR_AWARE=1`
  - `VX_GTO_MSHR_PRESSURE_NUM=3`
  - `VX_GTO_MSHR_PRESSURE_DEN=4`
  - `VX_GTO_MSHR_LOAD_COOLDOWN=2`

- 실행 예시:
  - 기본 사용(옵션 없이, 코드 기본값 사용)
```sh
./ci/blackbox.sh --driver=simx --app=sgemm3 --cores=32 --warps=32 --threads=32 --l2cache --perf=1
```
  - OFF 비교(`MSHR-aware` 비활성화)
```sh
CONFIGS="-DVX_GTO_MSHR_AWARE=0" ./ci/blackbox.sh --driver=simx --app=sgemm3 --cores=32 --warps=32 --threads=32 --l2cache --perf=1
```
  - 튜닝 예시(`NUM/DEN/COOLDOWN` 지정)
```sh
CONFIGS="-DVX_GTO_MSHR_AWARE=1 -DVX_GTO_MSHR_PRESSURE_NUM=7 -DVX_GTO_MSHR_PRESSURE_DEN=8 -DVX_GTO_MSHR_LOAD_COOLDOWN=4" ./ci/blackbox.sh --driver=simx --app=sgemm3 --cores=32 --warps=32 --threads=32 --l2cache --perf=1
```

### CCWS scheduler 설정 안내
CONFIGS에 
```sh
-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS 추가
```

- 주요 튜닝 파라미터:
  - `VX_CCWS_VTA_SIZE=8`: warp별 VTA entry 수
  - `VX_CCWS_LLD_SCORE=10`: VTA hit 시 부여할 lost-locality score
  - `VX_CCWS_LLS_DECAY_PERIOD=1`: 몇 tick마다 LLS를 감소시킬지 설정, `0`이면 decay 비활성화
  - `VX_CCWS_LLS_DECAY_STEP=1`: decay 시 LLS 감소량
  - `VX_CCWS_LLS_CUTOFF=0`: cumulative LLS cutoff, `0`이면 고정 top-K 모드
  - `VX_CCWS_MAX_ACTIVE_LOAD_WARPS=4`: cumulative cutoff가 고른 schedulable warp 수의 상한
  - `VX_CCWS_GATE_WHOLE_WARP=0`: `0`이면 load-only gating, `1`이면 whole-warp gating

- 튜닝 예시:
```sh
CONFIGS="-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS -DVX_CCWS_VTA_SIZE=16 -DVX_CCWS_LLD_SCORE=20 -DVX_CCWS_LLS_DECAY_PERIOD=2 -DVX_CCWS_LLS_DECAY_STEP=1 -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=8" ./ci/blackbox.sh --driver=simx --app=sgemm3 --cores=32 --warps=32 --threads=32 --l2cache --perf=3
```
