#!/usr/bin/env bash
# sssp @ tiny.coo, HW(LSU=2, banks=4).
# scheduler=RR, arbiter=RR by default, perf=1.
# Override with POLICIES="RR GTO gCAWS" and/or PERFS="1 2".
#
# Output: worklogs/experiments/sssp_tiny_4lsu_4bank/<policy>/sssp.perf<N>.log
# Usage: ./worklogs/scripts/run_sssp_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
EXP_DIR="${EXP_DIR:-$ROOT_DIR/worklogs/experiments/sssp_tiny_4lsu_4bank}"

CORES="${CORES:-1}"
WARPS="${WARPS:-32}"
THREADS="${THREADS:-32}"
TIMEOUT_SEC="${TIMEOUT_SEC:-1800}"

HW_TWEAK="${HW_TWEAK:--DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4}"
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0 $HW_TWEAK"
ARGS="${ARGS:-$ROOT_DIR/tests/opencl/sssp/tiny.coo kernel.cl}"

SCHED_RR=2
POLICIES_STR="${POLICIES:-RR}"
PERFS_STR="${PERFS:-1}"

declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
read -r -a POLICIES_ARR <<< "$POLICIES_STR"
read -r -a PERFS_ARR <<< "$PERFS_STR"

if [ ! -f "$BUILD_DIR/config.mk" ]; then
  echo "Missing $BUILD_DIR/config.mk. Run ./configure first, or set BUILD_DIR." >&2
  exit 1
fi

if [ ! -x "$BUILD_DIR/ci/blackbox.sh" ]; then
  echo "Missing $BUILD_DIR/ci/blackbox.sh. The configured build tree looks incomplete." >&2
  exit 1
fi

mkdir -p "$BUILD_DIR/tests/opencl"
rm -rf "$BUILD_DIR/tests/opencl/sssp"
cp -a "$ROOT_DIR/tests/opencl/sssp" "$BUILD_DIR/tests/opencl/sssp"

mkdir -p "$EXP_DIR"

for label in "${POLICIES_ARR[@]}"; do
  if [ -z "${ARBITER[$label]+x}" ]; then
    echo "Unknown policy '$label'. Known: ${!ARBITER[*]}" >&2
    exit 1
  fi

  arb=${ARBITER[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"

  echo "===== [$label] build (VORTEX_SCHED=$SCHED_RR, VORTEX_ARBITER=$arb, $HW_TWEAK) ====="
  env -u DEBUG CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  [$label] sim build FAIL"; continue; }
  env -u DEBUG CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  [$label] runtime build FAIL"; continue; }
  make -C "$BUILD_DIR/tests/opencl/sssp" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in "${PERFS_ARR[@]}"; do
    blog="$cfg_dir/sssp.perf${perf}.log"
    : > "$blog"
    (
      cd "$BUILD_DIR" && \
      env -u DEBUG CONFIGS="$conf" timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
        --driver=simx --app=sssp --cores="$CORES" --warps="$WARPS" --threads="$THREADS" \
        --l2cache --perf="$perf" --args="$ARGS" >> "$blog" 2>&1
    )
    rc=$?
    pass=$(grep -E "^(PASSED|FAILED)$" "$blog" | tail -1 || true)
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1 || true)
    echo "  [$label/perf$perf] rc=$rc ${pass:-no-pass-line} $ipc"
  done
done

echo
echo "DONE: $EXP_DIR"
