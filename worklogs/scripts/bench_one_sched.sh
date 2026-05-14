#!/usr/bin/env bash
# Run one scheduler policy against one or more OpenCL benchmarks.
#
# Usage: worklogs/scripts/bench_one_sched.sh <scheduler> [bench ...]
#   scheduler: RR | GTO | gCAWS | iPAWS, or 1 | 2 | 3 | 4
#              1=GTO 2=RR 3=gCAWS 4=iPAWS
#   bench:     tests/opencl name(s), optionally with app args in quotes
#              default: bfs
#
# Examples:
#   worklogs/scripts/bench_one_sched.sh gCAWS bfs
#   worklogs/scripts/bench_one_sched.sh RR "kmeans -f100 -p100"
#   worklogs/scripts/bench_one_sched.sh 4 "sgemm -n32"
#
# Results are saved under worklogs/runs/one_<scheduler>_<timestamp>/.

set -u
set -o pipefail

usage() {
  sed -n '2,16p' "$0" >&2
  exit 2
}

if [ $# -lt 1 ]; then
  usage
fi

SCHED_ARG="$1"
shift

case "$SCHED_ARG" in
  GTO|gto|1)
    LABEL="GTO"
    SCHED=1
    ;;
  RR|rr|2)
    LABEL="RR"
    SCHED=2
    ;;
  gCAWS|gcaws|GCAWS|3)
    LABEL="gCAWS"
    SCHED=3
    ;;
  iPAWS|ipaws|IPAWS|4)
    LABEL="iPAWS"
    SCHED=4
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    echo "Unknown scheduler: $SCHED_ARG" >&2
    usage
    ;;
esac

# Accept "bench [args...]" entries; default is just bfs.
BENCH_ENTRIES=("$@")
if [ ${#BENCH_ENTRIES[@]} -eq 0 ]; then
  BENCH_ENTRIES=(bfs)
fi

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

# Base config: 8-way dcache, Vortex-paper warp/thread/core count.
BASE_FLAGS="-DDCACHE_NUM_WAYS=8 -DNUM_CORES=1 -DNUM_WARPS=32 -DNUM_THREADS=32 -DPERF_ENABLE"
CACP=0
IPAWS_USE_CACP=0
CONF="$BASE_FLAGS -DVORTEX_SCHED=$SCHED -DVORTEX_CACP_ENABLE=$CACP -DVORTEX_IPAWS_USE_CACP=$IPAWS_USE_CACP"
if [ "$LABEL" = "gCAWS" ]; then
  CONF="$CONF -DVORTEX_CPL_LOG_INTERVAL=1000"
fi

build_one() {
  local log_dir="$1"
  local log="$log_dir/build.log"
  echo ">>> [$LABEL] building with $CONF" | tee "$log"
  if ! CONFIGS="$CONF" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$log" 2>&1; then
    echo "   build sim/simx FAILED (see $log)"
    return 1
  fi
  if ! CONFIGS="$CONF" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$log" 2>&1; then
    echo "   build runtime/simx FAILED (see $log)"
    return 1
  fi
}

run_bench() {
  local bench="$1" args="$2" log="$3"
  echo ">>> [$LABEL/$bench] args=\"$args\"" | tee -a "$log"
  local extra=()
  [ -n "$args" ] && extra=(--args="$args")
  (
    cd "$BUILD_DIR" && CONFIGS="$CONF" \
      timeout 1800 ./ci/blackbox.sh --driver=simx --app="$bench" --perf=2 "${extra[@]}" \
      >> "$log" 2>&1
  )
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "   run FAILED rc=$rc (see $log)"
    return 1
  fi
  echo "   done"
}

summarize_bench() {
  local bench="$1" log_root="$2"
  local log="$log_root/$LABEL/${bench}.log"
  local out="$log_root/summary_${bench}.txt"
  {
    printf "\n=== %s / %s ===\n" "$LABEL" "$bench"
    printf "%-14s | %-9s | %-9s | %-12s | %-12s | %-8s\n" \
      "label" "instrs" "cycles" "IPC" "dc_read_hit" "dc_wr_hit"
    printf '%s\n' "$(printf '%.0s-' {1..80})"
    if [ ! -f "$log" ]; then
      printf "%-14s | (no log)\n" "$LABEL"
      return
    fi
    local instrs cycles ipc rhit whit
    instrs=$(awk -F'[ ,]' '/instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
    cycles=$(awk -F'[ ,]' '/cycles=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
    ipc=$(awk -F'=' '/IPC=/ {print $NF}' "$log" | tail -1)
    rhit=$(awk -F'[()=%]' '/dcache read misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1)}}' "$log" | tail -1)
    whit=$(awk -F'[()=%]' '/dcache write misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1)}}' "$log" | tail -1)
    printf "%-14s | %-9s | %-9s | %-12s | %-12s | %-8s\n" \
      "$LABEL" "${instrs:-?}" "${cycles:-?}" "${ipc:-?}" "${rhit:-?}%" "${whit:-?}%"
  } | tee "$out"
}

LOG_ROOT="$ROOT_DIR/worklogs/runs/one_${LABEL}_$(date +%Y%m%d_%H%M%S)"
CFG_DIR="$LOG_ROOT/$LABEL"
mkdir -p "$CFG_DIR"

echo "Scheduler    : $LABEL ($SCHED)"
echo "Bench entries: ${BENCH_ENTRIES[*]}"
echo "Base flags   : $BASE_FLAGS"
echo "Log root     : $LOG_ROOT"

if build_one "$CFG_DIR"; then
  for entry in "${BENCH_ENTRIES[@]}"; do
    bench=${entry%% *}
    args=""
    [ "$entry" != "$bench" ] && args="${entry#* }"
    run_bench "$bench" "$args" "$CFG_DIR/${bench}.log" || true
  done
fi

for entry in "${BENCH_ENTRIES[@]}"; do
  bench=${entry%% *}
  summarize_bench "$bench" "$LOG_ROOT"
done

echo "All summaries saved under $LOG_ROOT"
