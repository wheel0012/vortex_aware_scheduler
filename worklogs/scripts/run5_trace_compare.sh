#!/usr/bin/env bash
# run5: per-issue trace + cross-policy comparison PNG.
#
# Small inputs (kmeans -p2000, bfs graph128k) keep the CSV ≲ 50MB per policy
# while still reaching steady state. Re-builds each policy under
# DCACHE_NUM_WAYS=8 + ARBITER=X, runs with VX_TRACE_WARP_SCHED=1, then feeds
# all three CSVs into compare_policies.py.

set -u
set -o pipefail

ROOT_DIR="/data/URP_26_spring/bbosseong/vortex_aware_scheduler"
BUILD_DIR="$ROOT_DIR/build"
LOG_ROOT="$ROOT_DIR/worklogs/runs/run5"

CORES=1
WARPS=32
THREADS=32
TIMEOUT_SEC=3600
BASE_FLAGS="-DPERF_ENABLE -DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4 -DDCACHE_NUM_WAYS=8"

declare -A ARBITER=( [RR]=2 [GTO]=1 [gCAWS]=4 )
declare -A BENCH_ARGS=(
  [kmeans]="-f100 -p2000"
  [bfs]="$ROOT_DIR/tests/opencl/bfs/graph128k.txt"
)

POLICIES=(RR GTO gCAWS)
BENCHES=(kmeans bfs)
PERF=0   # disable perf dump; we only need the trace CSV here

mkdir -p "$LOG_ROOT"

for label in "${POLICIES[@]}"; do
  arb="${ARBITER[$label]}"
  conf="$BASE_FLAGS -DVORTEX_ARBITER=$arb"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"

  echo "===== [$label] building (VORTEX_ARBITER=$arb) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  sim build FAIL"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  rt build FAIL"; continue; }

  for bench in "${BENCHES[@]}"; do
    args="${BENCH_ARGS[$bench]:-}"
    bench_dir="$cfg_dir/$bench"
    mkdir -p "$bench_dir"
    trace_csv="$bench_dir/issue_trace.csv"
    run_log="$bench_dir/run.log"
    extra=()
    [ -n "$args" ] && extra=(--args="$args")

    echo ">>> [$label/$bench] args=\"$args\" trace=\"$trace_csv\""
    (
      cd "$BUILD_DIR" && \
      env -u DEBUG \
      CONFIGS="$conf" \
      VX_TRACE_WARP_SCHED=1 \
      VX_TRACE_WARP_SCHED_FILE="$trace_csv" \
      VX_WARP_SCHED_POLICY="$label" \
      timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
        --driver=simx --app="$bench" \
        --cores="$CORES" --warps="$WARPS" --threads="$THREADS" \
        --l2cache --perf="$PERF" \
        "${extra[@]}" >> "$run_log" 2>&1
    )
    rc=$?
    rows=$(wc -l < "$trace_csv" 2>/dev/null || echo 0)
    size=$(du -h "$trace_csv" 2>/dev/null | awk '{print $1}')
    echo "  [$label/$bench] rc=$rc rows=$rows size=${size:-?}"
  done
done

# Cross-policy comparison PNG, per bench.
echo
echo "===== building comparison plots ====="
for bench in "${BENCHES[@]}"; do
  csv_args=()
  for label in "${POLICIES[@]}"; do
    csv="$LOG_ROOT/$label/$bench/issue_trace.csv"
    [ -s "$csv" ] || { echo "  skip $bench: missing $csv"; csv_args=(); break; }
    csv_args+=(--csv "$label:$csv")
  done
  [ ${#csv_args[@]} -eq 0 ] && continue
  out="$LOG_ROOT/compare_${bench}.png"
  python3 "$ROOT_DIR/worklogs/scripts/compare_policies.py" \
    "${csv_args[@]}" \
    --out "$out" \
    --num-warps "$WARPS" \
    --window 5000 \
    --zoom-cycles 4000 \
    && echo "  wrote $out"
done

echo
echo "DONE: $LOG_ROOT"
