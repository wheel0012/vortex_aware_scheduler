#!/usr/bin/env bash
# Run small simx workloads with warp-scheduler issue tracing enabled.
#
# This is meant for scheduler behavior validation, not IPC sweeps. It builds a
# small configuration, runs each policy/workload, saves issue_trace.csv, and
# invokes sim/simx/analyze_warp_sched_trace.py to create:
#   summary.txt, mismatch_report.csv, wid_timeline.png, pc_timeline.png,
#   and score_timeline.png when score data exists.
#
# Defaults:
#   CORES=1 WARPS=8 THREADS=8
#   POLICIES="RR GTO gCAWS"
#   BENCHES="bfs sgemm3"
#
# Useful overrides:
#   BENCHES="sgemm3" POLICIES="gCAWS" ./worklogs/scripts/trace_small_aware_schedulers.sh
#   EXTRA_CONFIGS="-D..." TIMEOUT_SEC=1200 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   MEM_LATENCY=4 CACHE_LATENCY=1 BENCHES=sgemm3 SGEMM_N=4 SGEMM_TILE=2 WARPS=4 THREADS=2 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   ANALYZE_ARGS="--split-by-wspawn" ./worklogs/scripts/trace_small_aware_schedulers.sh
#   ANALYZE_ARGS="--from-event WSPAWN:3 --to-event TMC:24 --split-by-wspawn" ./worklogs/scripts/trace_small_aware_schedulers.sh
#   PERF=1 ./worklogs/scripts/trace_small_aware_schedulers.sh
#
# Output:
#   worklogs/trace_runs/run<N>/<policy>/<bench>/issue_trace.csv
#   worklogs/trace_runs/run<N>/<policy>/<bench>/analysis/{summary.txt,...}
#   worklogs/trace_runs/run<N>/SUMMARY.md

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

CORES="${CORES:-1}"
WARPS="${WARPS:-8}"
THREADS="${THREADS:-8}"
TIMEOUT_SEC="${TIMEOUT_SEC:-900}"

POLICIES_STR="${POLICIES:-RR GTO gCAWS}"
BENCHES_STR="${BENCHES:-bfs sgemm3}"

# Leave PERF empty by default so this stays focused on behavior tracing.
# Set PERF=1 or PERF=2 if perf counters should be enabled too.
PERF="${PERF:-}"

# Keep this separate so users can add hardware knobs without editing the script.
EXTRA_CONFIGS="${EXTRA_CONFIGS:-}"
MEM_LATENCY="${MEM_LATENCY:-}"
CACHE_LATENCY="${CACHE_LATENCY:-}"
ANALYZE_ARGS_STR="${ANALYZE_ARGS:-}"

SGEMM_N="${SGEMM_N:-24}"
SGEMM_TILE="${SGEMM_TILE:-8}"

declare -A ARBITER=(
  [Priority]=0
  [GTO]=1
  [RR]=2
  [Matrix]=3
  [gCAWS]=4
)

declare -A BENCH_ARGS=(
  [bfs]="${ROOT_DIR}/tests/opencl/bfs/graph4k.txt"
  [sgemm3]="-n${SGEMM_N} -t${SGEMM_TILE}"
  [vecadd]="-n64"
)

read -r -a POLICIES_ARR <<< "$POLICIES_STR"
read -r -a BENCHES_ARR <<< "$BENCHES_STR"
read -r -a ANALYZE_ARGS_ARR <<< "$ANALYZE_ARGS_STR"

if [ ! -f "$BUILD_DIR/config.mk" ]; then
  echo "Missing $BUILD_DIR/config.mk. Run ./configure first, or set BUILD_DIR." >&2
  exit 1
fi

if [ ! -x "$BUILD_DIR/ci/blackbox.sh" ]; then
  echo "Missing $BUILD_DIR/ci/blackbox.sh. The configured build tree looks incomplete." >&2
  exit 1
fi

if [ ! -x "$ROOT_DIR/sim/simx/analyze_warp_sched_trace.py" ]; then
  echo "Missing executable analyzer: $ROOT_DIR/sim/simx/analyze_warp_sched_trace.py" >&2
  exit 1
fi

for label in "${POLICIES_ARR[@]}"; do
  if [ -z "${ARBITER[$label]+x}" ]; then
    echo "Unknown policy '$label'. Known: ${!ARBITER[*]}" >&2
    exit 1
  fi
done

for bench in "${BENCHES_ARR[@]}"; do
  if [ ! -d "$ROOT_DIR/tests/opencl/$bench" ] && [ ! -d "$ROOT_DIR/tests/regression/$bench" ]; then
    echo "Unknown workload '$bench' under tests/opencl or tests/regression." >&2
    exit 1
  fi
  if [ "$bench" = "sgemm3" ]; then
    if [ $((SGEMM_TILE * SGEMM_TILE)) -gt $((WARPS * THREADS)) ]; then
      echo "sgemm3 tile ${SGEMM_TILE}x${SGEMM_TILE} needs at least $((SGEMM_TILE * SGEMM_TILE)) hardware threads." >&2
      echo "Current WARPS*THREADS=$((WARPS * THREADS)). Use WARPS=8 THREADS=8 for -t8, or set SGEMM_TILE=4." >&2
      exit 1
    fi
  fi
done

summary_value() {
  local file="$1"
  local key="$2"
  awk -F': ' -v key="$key" '$1 == key {print $2; exit}' "$file" 2>/dev/null
}

trace_rows() {
  local file="$1"
  awk 'END {if (NR > 0) print NR - 1; else print 0}' "$file" 2>/dev/null
}

RUNS_DIR="$ROOT_DIR/worklogs/trace_runs"
mkdir -p "$RUNS_DIR"
NEXT_N=1
while [ -d "$RUNS_DIR/run$NEXT_N" ]; do
  NEXT_N=$((NEXT_N + 1))
done
LOG_ROOT="$RUNS_DIR/run$NEXT_N"
mkdir -p "$LOG_ROOT"

PERF_LABEL="${PERF:-disabled}"
SHAPE_FLAGS="-DNUM_CORES=$CORES -DNUM_WARPS=$WARPS -DNUM_THREADS=$THREADS -DL2_ENABLE"
PERF_FLAGS=""
[ -n "$PERF" ] && PERF_FLAGS="-DPERF_ENABLE"
MEM_FLAGS=""
[ -n "$MEM_LATENCY" ] && MEM_FLAGS="-DSIMX_FIXED_MEM_LATENCY=$MEM_LATENCY"
CACHE_FLAGS=""
[ -n "$CACHE_LATENCY" ] && CACHE_FLAGS="-DSIMX_CACHE_LATENCY=$CACHE_LATENCY"
BASE_FLAGS="$SHAPE_FLAGS $PERF_FLAGS $MEM_FLAGS $CACHE_FLAGS $EXTRA_CONFIGS"

SUMMARY="$LOG_ROOT/SUMMARY.md"
{
  echo "# Small Warp Scheduler Trace Run"
  echo
  echo "Timestamp: $(date)"
  echo
  echo "## Configuration"
  echo
  echo "- root: \`$ROOT_DIR\`"
  echo "- build: \`$BUILD_DIR\`"
  echo "- cores=$CORES, warps=$WARPS, threads=$THREADS, l2cache=on"
  echo "- perf: \`$PERF_LABEL\`"
  echo "- mem latency: \`${MEM_LATENCY:-ramulator}\`"
  echo "- cache latency: \`${CACHE_LATENCY:-2}\`"
  echo "- base flags: \`$BASE_FLAGS\`"
  echo "- trace env: \`VX_TRACE_WARP_SCHED=1\`"
  echo "- analyzer: \`sim/simx/analyze_warp_sched_trace.py\`"
  echo "- analyzer args: \`${ANALYZE_ARGS_STR:-none}\`"
  echo "- VORTEX_ARBITER: Priority=0, GTO=1, RR=2, Matrix=3, gCAWS=4"
  echo
  echo "## Workloads"
  echo
  echo "| Workload | Args |"
  echo "|---|---|"
  for bench in "${BENCHES_ARR[@]}"; do
    args="${BENCH_ARGS[$bench]-}"
    [ -z "$args" ] && args="(Makefile default OPTS)"
    echo "| $bench | \`$args\` |"
  done
  echo
  echo "## Results"
  echo
  echo "| Policy | Workload | Status | Trace rows | Issued | Mismatch count | Mismatch rate | Fallback count | Fallback rate | Artifacts |"
  echo "|---|---|---|---:|---:|---:|---:|---:|---:|---|"
} > "$SUMMARY"

echo "Trace output dir: $LOG_ROOT"

for label in "${POLICIES_ARR[@]}"; do
  arb="${ARBITER[$label]}"
  conf="$BASE_FLAGS -DVORTEX_ARBITER=$arb"
  policy_dir="$LOG_ROOT/$label"
  mkdir -p "$policy_dir"
  build_log="$policy_dir/build.log"

  echo "===== [$label] building small simx config (VORTEX_ARBITER=$arb) =====" | tee "$build_log"
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" DEBUG= -j"$(nproc)" >> "$build_log" 2>&1; then
    echo "  sim build FAIL; see $build_log"
    for bench in "${BENCHES_ARR[@]}"; do
      echo "| $label | $bench | build_fail | 0 | ? | ? | ? | ? | ? | \`$policy_dir\` |" >> "$SUMMARY"
    done
    continue
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" DEBUG= -j"$(nproc)" >> "$build_log" 2>&1; then
    echo "  runtime build FAIL; see $build_log"
    for bench in "${BENCHES_ARR[@]}"; do
      echo "| $label | $bench | build_fail | 0 | ? | ? | ? | ? | ? | \`$policy_dir\` |" >> "$SUMMARY"
    done
    continue
  fi

  for bench in "${BENCHES_ARR[@]}"; do
    args="${BENCH_ARGS[$bench]-}"
    bench_dir="$policy_dir/$bench"
    analysis_dir="$bench_dir/analysis"
    mkdir -p "$analysis_dir"

    trace_csv="$bench_dir/issue_trace.csv"
    run_log="$bench_dir/run.log"
    analyze_log="$bench_dir/analyze.log"

    extra=()
    [ -n "$args" ] && extra=(--args="$args")

    perf_arg=()
    [ -n "$PERF" ] && perf_arg=(--perf="$PERF")

    echo ">>> [$label/$bench] args=\"$args\" trace=\"$trace_csv\"" | tee "$run_log"
    (
      cd "$BUILD_DIR" || exit 1
      env -u DEBUG \
      CONFIGS="$conf" \
      VX_TRACE_WARP_SCHED=1 \
      VX_TRACE_WARP_SCHED_FILE="$trace_csv" \
      VX_WARP_SCHED_POLICY="$label" \
      timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
        --driver=simx --app="$bench" \
        "${perf_arg[@]}" \
        "${extra[@]}" >> "$run_log" 2>&1
    )
    rc=$?

    status="ok"
    if [ "$rc" -ne 0 ]; then
      status="run_rc_$rc"
    fi

    if [ ! -s "$trace_csv" ]; then
      echo "  [$label/$bench] $status, but trace CSV is missing or empty"
      echo "| $label | $bench | missing_trace($status) | 0 | ? | ? | ? | ? | ? | \`$bench_dir\` |" >> "$SUMMARY"
      continue
    fi

    if ! python3 "$ROOT_DIR/sim/simx/analyze_warp_sched_trace.py" "$trace_csv" -o "$analysis_dir" "${ANALYZE_ARGS_ARR[@]}" > "$analyze_log" 2>&1; then
      echo "  [$label/$bench] analyzer FAIL; see $analyze_log"
      status="analyze_fail($status)"
    fi

    summary_txt="$analysis_dir/summary.txt"
    rows=$(trace_rows "$trace_csv")
    issued=$(summary_value "$summary_txt" "Total issued instructions")
    mismatch_count=$(summary_value "$summary_txt" "Mismatch count")
    mismatch_rate=$(summary_value "$summary_txt" "Mismatch rate")
    fallback_count=$(summary_value "$summary_txt" "Fallback count")
    fallback_rate=$(summary_value "$summary_txt" "Fallback rate")

    echo "  [$label/$bench] $status rows=$rows issued=${issued:-?} mismatch=${mismatch_rate:-?} fallback=${fallback_rate:-?}"
    echo "| $label | $bench | $status | $rows | ${issued:-?} | ${mismatch_count:-?} | ${mismatch_rate:-?} | ${fallback_count:-?} | ${fallback_rate:-?} | \`$bench_dir\` |" >> "$SUMMARY"
  done
done

echo
echo "DONE: $LOG_ROOT"
echo "Summary: $SUMMARY"
