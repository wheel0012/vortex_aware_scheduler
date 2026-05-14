#!/usr/bin/env bash
# Re-run the same 4-policy x 6-workload grid with --perf=1 (CLASS_CORE) to
# collect pipeline-side stats. Reuses binaries already built by sweep_4policy.sh
# (the policy define affects the build; perf class is a runtime DCR flag, so
# no rebuild needed between perf=1 and perf=2 runs).
#
# Usage: ./worklogs/scripts/sweep_perf1_followup.sh [<run_dir>]
# Default: latest runN/ under worklogs/runs/.

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

CORES=1
WARPS=32
THREADS=32
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0"

declare -A SCHED=( [GTO]=1 [RR]=2 [gCAWS]=3 [iPAWS]=4 )
POLICIES=(GTO RR gCAWS iPAWS)

declare -A BENCH_ARGS=(
  [bfs]="${ROOT_DIR}/tests/opencl/bfs/graph4k.txt"
  [kmeans]="-f100 -p1000"
  [hotspot]="128 1 2 temp_128 power_128 output.out"
  [sgemm3]="-n96"
  [blackscholes]=""
  [vecadd]="-n10000"
)
BENCHES=(bfs kmeans hotspot sgemm3 blackscholes vecadd)
PERF=1

# Pick run dir
RUNS_DIR="$ROOT_DIR/worklogs/runs"
if [ $# -ge 1 ]; then
  LOG_ROOT="$(cd "$1" && pwd)"
else
  LOG_ROOT=$(ls -dt "$RUNS_DIR"/run* 2>/dev/null | head -1)
fi
[ -z "${LOG_ROOT:-}" ] && { echo "no runN/ dir"; exit 1; }
echo "perf=1 follow-up on: $LOG_ROOT"

for label in "${POLICIES[@]}"; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$LOG_ROOT/$label"
  # ensure binaries match this policy (cheap — re-link only if VORTEX_SCHED differs)
  echo "===== [$label] ensure binaries (VORTEX_SCHED=$sched) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 || { echo "sim build FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 || { echo "runtime build FAIL"; continue; }

  for bench in "${BENCHES[@]}"; do
    args="${BENCH_ARGS[$bench]}"
    blog="$cfg_dir/${bench}.perf${PERF}.log"
    extra=()
    [ -n "$args" ] && extra=(--args="$args")
    echo ">>> [$label/$bench/perf$PERF] args=\"$args\"" | tee "$blog"
    (
      cd "$BUILD_DIR" && \
      CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app="$bench" \
        --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=$PERF \
        "${extra[@]}" >> "../${blog#$ROOT_DIR/}" 2>&1
    )
    rc=$?
    ipc_line=$(grep "^PERF: instrs" "$blog" | tail -1)
    echo "  rc=$rc  $ipc_line"
  done
done

echo "DONE: $LOG_ROOT (perf=1 logs added)"
