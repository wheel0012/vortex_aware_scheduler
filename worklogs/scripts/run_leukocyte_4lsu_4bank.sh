#!/usr/bin/env bash
# leukocyte @ HW(LSU=2, banks=4).
# 4 policies x perf={1,2}.
#
# Output: worklogs/experiments/leukocyte_4lsu_4bank/<policy>/{build.log,leukocyte.perf{1,2}.log}
# Usage:
#   ./worklogs/scripts/run_leukocyte_4lsu_4bank.sh
#   LEUKOCYTE_FRAMES=10 ./worklogs/scripts/run_leukocyte_4lsu_4bank.sh
#   LEUKOCYTE_OPTS="/path/to/testfile.avi 10" ./worklogs/scripts/run_leukocyte_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/leukocyte_4lsu_4bank"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE $HW_TWEAK"
ARCH_FLAGS="-DNUM_CORES=$CORES -DNUM_WARPS=$WARPS -DNUM_THREADS=$THREADS"

LEUKOCYTE_FRAMES="${LEUKOCYTE_FRAMES:-3}"
LEUKOCYTE_OPTS="${LEUKOCYTE_OPTS:-$ROOT_DIR/tests/opencl/leukocyte/testfile.avi $LEUKOCYTE_FRAMES}"

declare -A SCHED=( [GTO]=1 [RR]=2 [gCAWS]=3 [iPAWS]=4 )
POLICIES=(RR GTO gCAWS iPAWS)

active_child=""

cleanup_child() {
  if [ -n "$active_child" ] && kill -0 "$active_child" 2>/dev/null; then
    kill -TERM "-$active_child" 2>/dev/null || kill -TERM "$active_child" 2>/dev/null || true
    sleep 1
    kill -KILL "-$active_child" 2>/dev/null || kill -KILL "$active_child" 2>/dev/null || true
  fi
}

on_interrupt() {
  echo
  echo "Interrupted; stopping active leukocyte run..."
  cleanup_child
  exit 130
}

trap on_interrupt INT TERM
trap cleanup_child EXIT

input_file="${LEUKOCYTE_OPTS%% *}"
if [ ! -f "$input_file" ]; then
  echo "ERROR: leukocyte input file not found: $input_file"
  echo "Set LEUKOCYTE_OPTS=\"/path/to/testfile.avi 1\" before running this script."
  exit 1
fi

existing_runs="$(pgrep -u "$(id -u)" -af '(^|/)leukocyte( |$)|blackbox.sh .*--app=leukocyte' || true)"
if [ -n "$existing_runs" ]; then
  echo "ERROR: another leukocyte/blackbox run is already active."
  echo "$existing_runs"
  echo "Stop those runs before starting this sweep."
  exit 1
fi

mkdir -p "$EXP_DIR"

LEUK_SRC="$ROOT_DIR/tests/opencl/leukocyte"
LEUK_BUILD="$BUILD_DIR/tests/opencl/leukocyte"
mkdir -p "$(dirname "$LEUK_BUILD")"
rm -rf "$LEUK_BUILD"
cp -a "$LEUK_SRC" "$LEUK_BUILD"
echo "[CHECK] refreshed build-dir leukocyte"

for label in "${POLICIES[@]}"; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS $ARCH_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"
  echo "===== [$label] build (VORTEX_SCHED=$sched, $HW_TWEAK) ====="
  env -u DEBUG CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  sim FAIL"; continue; }
  env -u DEBUG CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  rt FAIL"; continue; }
  make -C "$LEUK_BUILD" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/leukocyte.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && exec setsid env -u DEBUG CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app=leukocyte --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$LEUKOCYTE_OPTS" >> "$blog" 2>&1 ) &
    active_child=$!
    wait "$active_child"
    rc=$?
    active_child=""
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc $ipc"
  done
done
echo "DONE: $EXP_DIR"
