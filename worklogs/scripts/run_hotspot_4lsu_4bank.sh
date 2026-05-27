#!/usr/bin/env bash
# hotspot @ HW(LSU=2, banks=4) + WG=16x16 (rodinia orig).
# scheduler=RR, arbiter={RR,GTO,gCAWS} x perf={1,2}.
#
# WG size: tests/opencl/hotspot/hotspot.h #define BLOCK_SIZE 16 (this repo
# had been overridden to 4; restored to rodinia 16x16=256 threads/WG).
# Working set: 128x128 grid, 2 iterations (~64 KB).
#
# Output: worklogs/experiments/hotspot_wg256_4lsu_4bank/<policy>/{build.log,hotspot.perf{1,2}.log}
# Usage: ./worklogs/scripts/run_hotspot_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/hotspot_wg256_4lsu_4bank"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE $HW_TWEAK"
ARGS="128 1 2 temp_128 power_128 output.out"

SCHED_RR=2
declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
POLICIES=(RR GTO gCAWS)

mkdir -p "$EXP_DIR"

# Sanity: hotspot.h BLOCK_SIZE must be 16 (rodinia orig). 4 was repo override.
BLK=$(grep -E '^[[:space:]]*#define BLOCK_SIZE [0-9]+' "$ROOT_DIR/tests/opencl/hotspot/hotspot.h" | head -1 | awk '{print $3}')
echo "[CHECK] hotspot.h BLOCK_SIZE=$BLK (expect 16)"
if [ "$BLK" != "16" ]; then
  echo "  WARN: BLOCK_SIZE != 16. Patching ..."
  sed -i -E 's|^([[:space:]]*)#define BLOCK_SIZE [0-9]+.*$|\1#define BLOCK_SIZE 16|' "$ROOT_DIR/tests/opencl/hotspot/hotspot.h"
fi
# build dir keeps its own copy; refresh if stale
if [ -f "$BUILD_DIR/tests/opencl/hotspot/hotspot.h" ] && \
   ! cmp -s "$ROOT_DIR/tests/opencl/hotspot/hotspot.h" "$BUILD_DIR/tests/opencl/hotspot/hotspot.h"; then
  cp "$ROOT_DIR/tests/opencl/hotspot/hotspot.h" "$BUILD_DIR/tests/opencl/hotspot/hotspot.h"
  echo "[CHECK] refreshed build-dir hotspot.h"
fi

for label in "${POLICIES[@]}"; do
  arb=${ARBITER[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"
  echo "===== [$label] build (VORTEX_SCHED=$SCHED_RR, VORTEX_ARBITER=$arb, $HW_TWEAK) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  sim FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  rt FAIL"; continue; }
  make -C "$BUILD_DIR/tests/opencl/hotspot" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/hotspot.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app=hotspot --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$ARGS" >> "$blog" 2>&1 )
    rc=$?
    wg=$(grep -E "WG size of kernel" "$blog" | head -1)
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc $wg | $ipc"
  done
done
echo "DONE: $EXP_DIR"
