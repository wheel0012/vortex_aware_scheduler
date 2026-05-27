#!/usr/bin/env bash
# bfs @ graph128k.txt, WG=256, HW(LSU=2, banks=4) — run4 baseline로 perf=1, perf=2 모두 측정.
# scheduler=RR, arbiter={RR,GTO,gCAWS}.
# Output: worklogs/experiments/bfs_wg256_4lsu_4bank/g128k/<policy>/bfs.perf{1,2}.log

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/bfs_wg256_4lsu_4bank/g128k"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0 $HW_TWEAK"
ARGS="$ROOT_DIR/tests/opencl/bfs/graph128k.txt"

SCHED_RR=2
declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
POLICIES=(RR GTO gCAWS)

mkdir -p "$EXP_DIR"

# Sanity: bfs main.cc MAX_THREADS_PER_BLOCK must be 256 (WG=256, 8 warps/WG).
MTB=$(grep -E '^#define MAX_THREADS_PER_BLOCK [0-9]+' "$ROOT_DIR/tests/opencl/bfs/main.cc" | head -1 | awk '{print $3}')
echo "[CHECK] bfs MAX_THREADS_PER_BLOCK=$MTB (expect 256)"

for label in "${POLICIES[@]}"; do
  arb=${ARBITER[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"

  echo "===== [$label] build (VORTEX_SCHED=$SCHED_RR, VORTEX_ARBITER=$arb, $HW_TWEAK) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  [$label] sim build FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  [$label] runtime build FAIL"; continue; }
  make -C "$BUILD_DIR/tests/opencl/bfs" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/bfs.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && CONFIGS="$conf" timeout 1800 ./ci/blackbox.sh \
        --driver=simx --app=bfs --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$ARGS" >> "$blog" 2>&1 )
    rc=$?
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc  $ipc"
  done
done

echo
echo "DONE: $EXP_DIR"
