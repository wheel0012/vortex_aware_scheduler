#!/usr/bin/env bash
# streamcluster @ HW(LSU=2, banks=4) + input size increased.
# 4 policies x perf={1,2}.
#
# Input parameters:
#   kmin=2, kmax=16, dim=8, n=4096, chunksize=256, clustersize=256
#   Working set: ~4096 * 8 * 4 = 128 KB
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

# Streamcluster parameters: kmin kmax dim n chunksize clustersize infile outfile nproc
STREAMCLUSTER_ARGS="2 16 8 4096 256 256 none output.txt 1"

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
  make -C "$BUILD_DIR/tests/opencl/streamcluster" clean >> "$cfg_dir/build.log" 2>&1 || true

  for perf in 2 1; do
    blog="$cfg_dir/streamcluster.perf${perf}.log"
    : > "$blog"
    ( cd "$BUILD_DIR" && CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app=streamcluster --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$perf --args="$STREAMCLUSTER_ARGS" >> "$blog" 2>&1 )
    rc=$?
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1)
    echo "  [$label/perf$perf] rc=$rc $ipc"
  done
done
echo "DONE: $EXP_DIR"
