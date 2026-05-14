#!/usr/bin/env bash
# sgemm3 @ HW(LSU=2, banks=4) + WG=32x32 (=1024 threads = full warp pool).
# 4 policies x perf={1,2}.
#
# WG size: tests/opencl/sgemm3/main.cc has hardcoded size=512, tile_size=32
# (line 173-174 overrides cmdline -n/-t). Local work group = 32*32 = 1024
# threads = 32 warps (NUM_WARPS=32 entire pool). Working set = 3 * 512^2 *
# 4 B = 3 MB (overflows L2=1MB; partially DRAM-bound).
#
# Output: worklogs/experiments/sgemm3_wg1024_4lsu_4bank/<policy>/{build.log,sgemm3.perf{1,2}.log}
# Usage: ./worklogs/scripts/run_sgemm3_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/sgemm3_wg1024_4lsu_4bank"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE $HW_TWEAK"
ARGS="-n96"  # ignored by sgemm3 main.cc (line 173 overrides size=512)

declare -A SCHED=( [GTO]=1 [RR]=2 [gCAWS]=3 [iPAWS]=4 )
POLICIES=(RR GTO gCAWS iPAWS)

mkdir -p "$EXP_DIR"

for label in "${POLICIES[@]}"; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"
  echo "===== [$label] build (VORTEX_SCHED=$sched, $HW_TWEAK) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  sim FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  rt FAIL"; continue; }
  make -C "$BUILD_DIR/tests/opencl/sgemm3" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/sgemm3.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app=sgemm3 --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$ARGS" >> "$blog" 2>&1 )
    rc=$?
    sz=$(grep "Matrix size=" "$blog" | head -1)
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc $sz | $ipc"
  done
done
echo "DONE: $EXP_DIR"
