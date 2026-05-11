#!/usr/bin/env bash
# Scheduler/CACP sweep on a single benchmark.
#
# Usage: worklogs/bench_sweep.sh [bench]
#   bench: tests/opencl name (default: bfs)
#
# Rebuilds sim/simx + runtime/simx with -DVORTEX_SCHED / -DVORTEX_CACP_ENABLE
# for each policy and runs --driver=simx through ci/blackbox.sh. Results are
# parsed from each run log and printed as a table.

set -u
set -o pipefail

BENCH="${1:-bfs}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
LOG_DIR="$ROOT_DIR/worklogs/sweep_${BENCH}"
mkdir -p "$LOG_DIR"

# Configs to compare. Format: "label sched cacp"
# sched: 1=GTO 2=RR 3=gCAWS 4=iPAWS
# cacp:  0=off 1=on
COMBOS=(
  "RR             2 0"
  "GTO            1 0"
  "gCAWS_noCACP   3 0"
  "gCAWS_CACP     3 1"
  "iPAWS          4 1"
)

# Base config: 8-way dcache, Vortex-paper warp/thread/core count.
BASE_FLAGS="-DDCACHE_NUM_WAYS=8 -DNUM_CORES=1 -DNUM_WARPS=16 -DNUM_THREADS=16 -DPERF_ENABLE"

extract() {
  # extract a numeric value following a label from a perf line.
  local file="$1" pat="$2"
  awk -v p="$pat" 'match($0, p"=([0-9.]+)", m) {print m[1]; exit}' "$file"
}

run_one() {
  local label="$1" sched="$2" cacp="$3"
  local conf="$BASE_FLAGS -DVORTEX_SCHED=$sched -DVORTEX_CACP_ENABLE=$cacp"
  local log="$LOG_DIR/${label}.log"

  echo ">>> [$label] sched=$sched cacp=$cacp" | tee "$log"

  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$log" 2>&1; then
    echo "   build sim/simx FAILED (see $log)"; return 1
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$log" 2>&1; then
    echo "   build runtime/simx FAILED (see $log)"; return 1
  fi

  (
    cd "$BUILD_DIR" && CONFIGS="$conf" \
      timeout 600 ./ci/blackbox.sh --driver=simx --app="$BENCH" --perf=2 \
      >> "$log" 2>&1
  )
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "   run FAILED rc=$rc (see $log)"; return 1
  fi
  echo "   done"
}

summarize() {
  printf "\n%-14s | %-9s | %-9s | %-12s | %-12s | %-8s\n" \
    "label" "instrs" "cycles" "IPC" "dc_read_hit" "dc_wr_hit"
  printf '%s\n' "$(printf '%.0s-' {1..80})"
  for combo in "${COMBOS[@]}"; do
    local label sched cacp
    read -r label sched cacp <<<"$combo"
    local log="$LOG_DIR/${label}.log"
    [ -f "$log" ] || { printf "%-14s | (no log)\n" "$label"; continue; }
    local instrs cycles ipc rhit whit
    instrs=$(awk -F'[ ,]' '/instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
    cycles=$(awk -F'[ ,]' '/cycles=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
    ipc=$(awk -F'=' '/IPC=/ {print $NF}' "$log" | tail -1)
    rhit=$(awk -F'[()=%]' '/dcache read misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1)}}' "$log" | tail -1)
    whit=$(awk -F'[()=%]' '/dcache write misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1)}}' "$log" | tail -1)
    printf "%-14s | %-9s | %-9s | %-12s | %-12s | %-8s\n" \
      "$label" "${instrs:-?}" "${cycles:-?}" "${ipc:-?}" "${rhit:-?}%" "${whit:-?}%"
  done
}

echo "Sweep target: $BENCH"
echo "Base flags : $BASE_FLAGS"
echo "Log dir    : $LOG_DIR"

for combo in "${COMBOS[@]}"; do
  read -r label sched cacp <<<"$combo"
  run_one "$label" "$sched" "$cacp"
done

summarize | tee "$LOG_DIR/summary.txt"
echo "Summary saved to $LOG_DIR/summary.txt"
