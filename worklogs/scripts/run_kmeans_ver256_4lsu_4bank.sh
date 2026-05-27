#!/usr/bin/env bash
# Reproduces worklogs/experiments/kmeans_ver256_4lsu_4bank/.
#
# HW spec tweak via build flags (no source change):
#   -DNUM_LSU_BLOCKS=2   -> DCACHE_NUM_REQS auto becomes 4 (constants.h:45)
#   -DDCACHE_NUM_BANKS=4
#
# Workload:
#   kmeans -f100 -p1000 (WG=256, set in tests/opencl/kmeans/main.cc BLOCK_SIZE)
#   Working set ~400 KB (25x L1=16KB, 40% L2=1MB) — sweet-spot L1-thrash.
#
# Outputs per arbiter policy: build.log, kmeans.perf1.log (pipeline), kmeans.perf2.log (cache).
# Scheduler is fixed to RR; arbiter sweeps RR/GTO/gCAWS.
# Each ~30s; full run ~5 min.
#
# Usage: ./worklogs/scripts/run_kmeans_ver256_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/kmeans_ver256_4lsu_4bank"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0 $HW_TWEAK"
ARGS="-f100 -p50000"

SCHED_RR=2
declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
POLICIES=(RR GTO gCAWS)

mkdir -p "$EXP_DIR"

# Sanity: kmeans BLOCK_SIZE must be 256 (host-side WG size). 1 will collapse
# all 32-lane warps to 1-thread WGs (the original repo bug). 256 means 8
# warps per WG.
BLK=$(grep -E '^#define BLOCK_SIZE [0-9]+' "$ROOT_DIR/tests/opencl/kmeans/main.cc" | head -1 | awk '{print $3}')
BLK2=$(grep -E '^#define BLOCK_SIZE2 [0-9]+' "$ROOT_DIR/tests/opencl/kmeans/main.cc" | head -1 | awk '{print $3}')
echo "[CHECK] kmeans main.cc BLOCK_SIZE=$BLK BLOCK_SIZE2=$BLK2 (expect 256)"
if [ "$BLK" != "256" ] || [ "$BLK2" != "256" ]; then
  echo "  WARN: BLOCK_SIZE != 256. Patching in-place ..."
  sed -i -E "s|^#define BLOCK_SIZE [0-9]+(.*)$|#define BLOCK_SIZE 256\1|"  "$ROOT_DIR/tests/opencl/kmeans/main.cc"
  sed -i -E "s|^#define BLOCK_SIZE2 [0-9]+(.*)$|#define BLOCK_SIZE2 256\1|" "$ROOT_DIR/tests/opencl/kmeans/main.cc"
fi

for label in "${POLICIES[@]}"; do
  arb=${ARBITER[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"

  echo "===== [$label] build (VORTEX_SCHED=$SCHED_RR, VORTEX_ARBITER=$arb, HW=$HW_TWEAK) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  [$label] sim build FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  [$label] runtime build FAIL"; continue; }
  # Force kmeans test rebuild so BLOCK_SIZE / HW flag changes propagate.
  make -C "$BUILD_DIR/tests/opencl/kmeans" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/kmeans.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && CONFIGS="$conf" timeout 1800 ./ci/blackbox.sh \
        --driver=simx --app=kmeans --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$ARGS" >> "$blog" 2>&1 )
    rc=$?
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc  $ipc"
  done
done

echo
echo "DONE: $EXP_DIR"
echo "Inspect SUMMARY.md or run:"
echo "  python3 worklogs/scripts/plot_kmeans_4policy.py $EXP_DIR"
