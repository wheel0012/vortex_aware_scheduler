#!/usr/bin/env bash
# kmeans WG-size experiment (ver1): rebuild kmeans kernel with BLOCK_SIZE=64
# (was 1 in this repo, orig rodinia 256), fix scheduler=RR, and sweep
# arbiter={RR,GTO,gCAWS}.
# Output: worklogs/experiments/kmeans_ver1/{RR,GTO,gCAWS}/kmeans.perf2.log

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/kmeans_ver1"

CORES=1
WARPS=32
THREADS=32
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0"

SCHED_RR=2
declare -A ARBITER=( [GTO]=1 [RR]=2 [gCAWS]=4 )
POLICIES=(RR GTO gCAWS)

ARGS="-f100 -p1000"
PERF=2

mkdir -p "$EXP_DIR"

for label in "${POLICIES[@]}"; do
  arb=${ARBITER[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  build_log="$cfg_dir/build.log"

  echo "===== [$label] build (VORTEX_SCHED=$SCHED_RR, VORTEX_ARBITER=$arb) =====" | tee "$build_log"
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$build_log" 2>&1 \
    || { echo "  sim build FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$build_log" 2>&1 \
    || { echo "  runtime build FAIL"; continue; }
  # also force kmeans test rebuild (so the patched main.cc is recompiled)
  make -C "$BUILD_DIR/tests/opencl/kmeans" clean >> "$build_log" 2>&1 || true

  blog="$cfg_dir/kmeans.perf${PERF}.log"
  echo ">>> [$label/kmeans/perf$PERF] args=\"$ARGS\"" | tee "$blog"
  (
    cd "$BUILD_DIR" && \
    CONFIGS="$conf" timeout 1800 ./ci/blackbox.sh \
      --driver=simx --app=kmeans \
      --cores=$CORES --warps=$WARPS --threads=$THREADS \
      --l2cache --perf=$PERF \
      --args="$ARGS" >> "../${blog#$ROOT_DIR/}" 2>&1
  )
  rc=$?
  wg_line=$(grep -E "WG size of kernel" "$blog" | head -1)
  ipc_line=$(grep "^PERF: instrs" "$blog" | tail -1)
  echo "  rc=$rc  $wg_line  |  $ipc_line"
done

echo "DONE: $EXP_DIR"
