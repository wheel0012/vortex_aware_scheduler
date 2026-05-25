#!/usr/bin/env bash
# kmeans WG-size sweep: parameterised by BLOCK_SIZE.
# Patches tests/opencl/kmeans/main.cc, force-rebuilds the test, runs 4 policies.
# Output: worklogs/experiments/kmeans_ver<WG>/<policy>/kmeans.perf2.log
#
# Usage: ./run_kmeans_wg.sh <BLOCK_SIZE>  (e.g. 64, 256, 512)

set -u
set -o pipefail

WG="${1:?usage: $0 <BLOCK_SIZE>}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
EXP_DIR="$ROOT_DIR/worklogs/experiments/kmeans_ver${WG}"
KMEANS_MAIN="$ROOT_DIR/tests/opencl/kmeans/main.cc"

CORES=1
WARPS=32
THREADS=32
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0"

declare -A SCHED=( [GTO]=1 [RR]=2 [gCAWS]=3 [iPAWS]=4 )
POLICIES=(GTO RR gCAWS iPAWS)
ARGS="-f100 -p1000"
PERF=2

mkdir -p "$EXP_DIR"

# Patch BLOCK_SIZE / BLOCK_SIZE2 in main.cc (in-place). Match either current
# value to make the script idempotent across runs.
sed -i -E "s|^#define BLOCK_SIZE [0-9]+(.*)$|#define BLOCK_SIZE ${WG}\1|" "$KMEANS_MAIN"
sed -i -E "s|^#define BLOCK_SIZE2 [0-9]+(.*)$|#define BLOCK_SIZE2 ${WG}\1|" "$KMEANS_MAIN"
echo "BLOCK_SIZE patched in $KMEANS_MAIN:"
grep -E "^#define BLOCK_SIZE2? " "$KMEANS_MAIN" | head -4

for label in "${POLICIES[@]}"; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$EXP_DIR/$label"
  mkdir -p "$cfg_dir"
  build_log="$cfg_dir/build.log"

  echo "===== [$label] build (VORTEX_SCHED=$sched, WG=$WG) =====" | tee "$build_log"
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$build_log" 2>&1 \
    || { echo "  sim build FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$build_log" 2>&1 \
    || { echo "  runtime build FAIL"; continue; }
  # Force kmeans test rebuild so the patched BLOCK_SIZE takes effect
  make -C "$BUILD_DIR/tests/opencl/kmeans" clean >> "$build_log" 2>&1 || true

  blog="$cfg_dir/kmeans.perf${PERF}.log"
  echo ">>> [$label/kmeans/perf$PERF/WG=$WG] args=\"$ARGS\"" | tee "$blog"
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
