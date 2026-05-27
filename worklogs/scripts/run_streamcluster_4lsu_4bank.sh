#!/usr/bin/env bash
# streamcluster @ HW(LSU=2, banks=4).
# scheduler=RR, arbiter={RR,GTO,gCAWS} x perf={1,2}.
#
# Input parameters:
#   kmin=2, kmax=4, dim=4, n=16, chunksize=16, clustersize=16
#
# Output: worklogs/experiments/streamcluster_large_4lsu_4bank/<policy>/{build.log,streamcluster.perf{1,2}.log}
# Usage: ./worklogs/scripts/run_streamcluster_4lsu_4bank.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/streamcluster_large_4lsu_4bank"

CORES=1
WARPS=32
THREADS=32
HW_TWEAK="-DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4"
BASE_FLAGS="-DPERF_ENABLE $HW_TWEAK"
ARCH_FLAGS="-DNUM_CORES=$CORES -DNUM_WARPS=$WARPS -DNUM_THREADS=$THREADS"

# Streamcluster parameters: kmin kmax dim n chunksize clustersize infile outfile nproc + extra args
STREAMCLUSTER_OPTS="${STREAMCLUSTER_OPTS:-2 4 4 16 16 16 none output.txt 1 -t gpu -d 0}"

SCHED_RR=2
declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
POLICIES=(RR GTO gCAWS)

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
  echo "Interrupted; stopping active streamcluster run..."
  cleanup_child
  exit 130
}

trap on_interrupt INT TERM
trap cleanup_child EXIT

existing_runs="$(pgrep -u "$(id -u)" -af '(^|/)streamcluster( |$)|blackbox.sh .*--app=streamcluster' || true)"
if [ -n "$existing_runs" ]; then
  echo "ERROR: another streamcluster/blackbox run is already active."
  echo "$existing_runs"
  echo "Stop those runs before starting this sweep."
  exit 1
fi

mkdir -p "$EXP_DIR"

STREAM_SRC="$ROOT_DIR/tests/opencl/streamcluster"
STREAM_BUILD="$BUILD_DIR/tests/opencl/streamcluster"

if [ ! -d "$STREAM_BUILD" ]; then
  mkdir -p "$(dirname "$STREAM_BUILD")"
fi

if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete \
    --exclude 'streamcluster' \
    --exclude 'streamcluster.host' \
    --exclude '*.o' \
    --exclude '*.log' \
    --exclude '.depend' \
    --exclude 'output.txt' \
    --exclude 'PD.txt' \
    --exclude 'result*' \
    "$STREAM_SRC/" "$STREAM_BUILD/"
else
  rm -rf "$STREAM_BUILD"
  cp -a "$STREAM_SRC" "$STREAM_BUILD"
fi
cp "$STREAM_SRC/streamcluster.cpp" "$STREAM_BUILD/streamcluster.cpp"
cp "$STREAM_SRC/streamcluster.h" "$STREAM_BUILD/streamcluster.h"
cp "$STREAM_SRC/streamcluster_cl.h" "$STREAM_BUILD/streamcluster_cl.h"
cp "$STREAM_SRC/Kernels.cl" "$STREAM_BUILD/Kernels.cl"
cp "$STREAM_SRC/Makefile" "$STREAM_BUILD/Makefile"
echo "[CHECK] refreshed build-dir streamcluster"

for label in "${POLICIES[@]}"; do
  arb=${ARBITER[$label]}
  conf="$BASE_FLAGS $ARCH_FLAGS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"
  echo "===== [$label] build (VORTEX_SCHED=$SCHED_RR, VORTEX_ARBITER=$arb, $HW_TWEAK) ====="
  env -u DEBUG CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  sim FAIL"; continue; }
  env -u DEBUG CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  rt FAIL"; continue; }
  make -C "$BUILD_DIR/tests/opencl/streamcluster" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/streamcluster.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && exec setsid env -u DEBUG CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app=streamcluster --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$STREAMCLUSTER_OPTS" >> "$blog" 2>&1 ) &
    active_child=$!
    wait "$active_child"
    rc=$?
    active_child=""
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc $ipc"
  done
done
echo "DONE: $EXP_DIR"
