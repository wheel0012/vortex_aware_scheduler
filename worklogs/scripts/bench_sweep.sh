#!/usr/bin/env bash
# Scheduler/CACP sweep across one or more benchmarks.
#
# Usage: worklogs/bench_sweep.sh [bench ...]
#   bench: tests/opencl name(s) (default: bfs)
#
# Rebuilds sim/simx + runtime/simx with -DVORTEX_SCHED / -DVORTEX_CACP_ENABLE
# once per policy combo and reuses that build across all benchmarks. Per-bench
# results are parsed from each run log and printed as a summary table.

set -u
set -o pipefail

# Accept "bench [args...]" entries; default is just bfs.
BENCH_ENTRIES=("$@")
if [ ${#BENCH_ENTRIES[@]} -eq 0 ]; then
  BENCH_ENTRIES=(bfs)
fi
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

# Configs to compare. Format: "label sched cacp ipaws_use_cacp"
# sched: 1=GTO 2=RR 3=gCAWS 4=iPAWS
# cacp:  global CACP enable for the dcache config (kept at 0; CACP is not
#        part of the RTL target — use scripts/exp_cacp_ablation.sh for the
#        CACP ablation study).
# ipaws_use_cacp: only relevant if cacp=1; left at 0 for consistency.
COMBOS=(
  "RR     2 0 0"
  "GTO    1 0 0"
  "gCAWS  3 0 0"
  "iPAWS  4 0 0"
)

# Base config: 8-way dcache, Vortex-paper warp/thread/core count.
BASE_FLAGS="-DDCACHE_NUM_WAYS=8 -DNUM_CORES=1 -DNUM_WARPS=32 -DNUM_THREADS=32 -DPERF_ENABLE"

extract() {
  # extract a numeric value following a label from a perf line.
  local file="$1" pat="$2"
  awk -v p="$pat" 'match($0, p"=([0-9.]+)", m) {print m[1]; exit}' "$file"
}

build_one() {
  local label="$1" conf="$2" log_dir="$3"
  local log="$log_dir/build.log"
  echo ">>> [$label] building with $conf" | tee "$log"
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$log" 2>&1; then
    echo "   build sim/simx FAILED (see $log)"; return 1
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$log" 2>&1; then
    echo "   build runtime/simx FAILED (see $log)"; return 1
  fi
}

run_bench() {
  local label="$1" conf="$2" bench="$3" args="$4" log="$5"
  echo ">>> [$label/$bench] args=\"$args\"" | tee -a "$log"
  local extra=()
  [ -n "$args" ] && extra=(--args="$args")
  (
    cd "$BUILD_DIR" && CONFIGS="$conf" \
      timeout 1800 ./ci/blackbox.sh --driver=simx --app="$bench" --perf=2 "${extra[@]}" \
      >> "$log" 2>&1
  )
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "   run FAILED rc=$rc (see $log)"; return 1
  fi
  echo "   done"
}

summarize_bench() {
  local bench="$1" log_root="$2"
  local out="$log_root/summary_${bench}.txt"
  {
    printf "\n=== %s ===\n" "$bench"
    printf "%-14s | %-9s | %-9s | %-12s | %-12s | %-8s\n" \
      "label" "instrs" "cycles" "IPC" "dc_read_hit" "dc_wr_hit"
    printf '%s\n' "$(printf '%.0s-' {1..80})"
    for combo in "${COMBOS[@]}"; do
      local label sched cacp ipaws_use_cacp
      read -r label sched cacp ipaws_use_cacp <<<"$combo"
      local log="$log_root/${label}/${bench}.log"
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
  } | tee "$out"
}

LOG_ROOT="$ROOT_DIR/worklogs/runs/sweep_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_ROOT"

echo "Sweep entries: ${BENCH_ENTRIES[*]}"
echo "Base flags  : $BASE_FLAGS"
echo "Log root    : $LOG_ROOT"

# Outer loop: build once per config, then run all benchmarks against it.
for combo in "${COMBOS[@]}"; do
  read -r label sched cacp ipaws_use_cacp <<<"$combo"
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched -DVORTEX_CACP_ENABLE=$cacp -DVORTEX_IPAWS_USE_CACP=$ipaws_use_cacp"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  if ! build_one "$label" "$conf" "$cfg_dir"; then
    continue
  fi
  for entry in "${BENCH_ENTRIES[@]}"; do
    bench=${entry%% *}
    args=""; [ "$entry" != "$bench" ] && args="${entry#* }"
    run_bench "$label" "$conf" "$bench" "$args" "$cfg_dir/${bench}.log" || true
  done
done

for entry in "${BENCH_ENTRIES[@]}"; do
  bench=${entry%% *}
  summarize_bench "$bench" "$LOG_ROOT"
done
echo "All summaries saved under $LOG_ROOT"
