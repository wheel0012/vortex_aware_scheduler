#!/usr/bin/env bash
# blackscholes @ HW(LSU=2, banks=4) + WG=128.
# scheduler=RR, arbiter={RR,GTO,gCAWS} x perf={1,2}.
#
# WG size: tests/opencl/blackscholes/oclBlackScholes_launcher.cpp
#   size_t localWorkSize = 128; (was 1; orig comment says 128).
# Workload: optionCount = 64*64 = 4096 (main.cc), ~112 KB working set.
#
# Output: worklogs/experiments/blackscholes_wg128_4lsu_4bank/<policy>/{build.log,blackscholes.perf{1,2}.log}
# Usage: ./worklogs/scripts/run_blackscholes_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/blackscholes_wg128_4lsu_4bank"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE $HW_TWEAK"
ARGS=""

SCHED_RR=2
declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
POLICIES=(RR GTO gCAWS)

mkdir -p "$EXP_DIR"

LAUNCHER="$ROOT_DIR/tests/opencl/blackscholes/oclBlackScholes_launcher.cpp"
LWS=$(grep -E '^[[:space:]]*size_t[[:space:]]+localWorkSize' "$LAUNCHER" | head -1 | grep -oP "localWorkSize\s*=\s*\K[0-9]+")
echo "[CHECK] localWorkSize=$LWS (expect 128)"
if [ "$LWS" != "128" ]; then
  echo "  WARN: localWorkSize != 128. Patching ..."
  sed -i -E 's|(size_t[[:space:]]+localWorkSize[[:space:]]*=[[:space:]]*)[0-9]+|\1128|' "$LAUNCHER"
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
  make -C "$BUILD_DIR/tests/opencl/blackscholes" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/blackscholes.perf${perf}.log"
    : > "$blog"
    extra=()
    [ -n "$ARGS" ] && extra=(--args="$ARGS")
    ( cd "$BUILD_DIR" && CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app=blackscholes --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf "${extra[@]}" >> "$blog" 2>&1 )
    rc=$?
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc $ipc"
  done
done
echo "DONE: $EXP_DIR"
