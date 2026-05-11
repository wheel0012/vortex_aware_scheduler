#!/usr/bin/env bash
# Experiment E: sweep CACP way-reservation count on gCAWS.
# Holds scheduler=gCAWS and dcache associativity at 8-way, then varies
# the number of ways reserved for the critical warp from 0 (CACP off)
# through 4 (50%). Per-config builds are reused across benchmarks.

set -u
set -o pipefail

# Accept one or more "bench [args...]" entries (each entry is a single
# positional argument).  e.g. ./exp_E.sh bfs "sgemm3 -n128"
BENCH_ENTRIES=("$@")
if [ ${#BENCH_ENTRIES[@]} -eq 0 ]; then
  BENCH_ENTRIES=(bfs sgemm3 spmv)
fi
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

BASE_FLAGS="-DDCACHE_NUM_WAYS=8 -DNUM_CORES=1 -DNUM_WARPS=32 -DNUM_THREADS=32 -DPERF_ENABLE -DVORTEX_SCHED=3"

# label cacp_enable cacp_reserved
COMBOS=(
  "RESV0   0 0"
  "RESV1   1 1"
  "RESV2   1 2"
  "RESV3   1 3"
  "RESV4   1 4"
)

LOG_ROOT="$ROOT_DIR/worklogs/runs/exp_cacp_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_ROOT"

build_one() {
  local label="$1" conf="$2" log_dir="$3"
  local log="$log_dir/build.log"
  echo ">>> [$label] $conf" | tee "$log"
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$log" 2>&1 || return 1
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$log" 2>&1 || return 1
}

run_bench() {
  local label="$1" conf="$2" bench="$3" args="$4" log="$5"
  echo ">>> [$label/$bench] args=\"$args\"" | tee -a "$log"
  local extra=()
  [ -n "$args" ] && extra=(--args="$args")
  (cd "$BUILD_DIR" && CONFIGS="$conf" timeout 1800 \
    ./ci/blackbox.sh --driver=simx --app="$bench" --perf=2 "${extra[@]}" >> "$log" 2>&1)
}

for combo in "${COMBOS[@]}"; do
  read -r label en resv <<<"$combo"
  conf="$BASE_FLAGS -DVORTEX_CACP_ENABLE=$en -DVORTEX_CACP_RESERVED=$resv"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  build_one "$label" "$conf" "$cfg_dir" || continue
  for entry in "${BENCH_ENTRIES[@]}"; do
    bench=${entry%% *}
    args=""; [ "$entry" != "$bench" ] && args="${entry#* }"
    run_bench "$label" "$conf" "$bench" "$args" "$cfg_dir/${bench}.log" || true
  done
done

for entry in "${BENCH_ENTRIES[@]}"; do
  bench=${entry%% *}
  out="$LOG_ROOT/summary_${bench}.txt"
  {
    printf "\n=== %s (exp-E, gCAWS, varying CACP reserved ways) ===\n" "$entry"
    printf "%-8s | %-9s | %-12s | %-12s | %-8s\n" \
      "label" "cycles" "IPC" "dc_read_hit" "dc_wr_hit"
    printf '%s\n' "$(printf '%.0s-' {1..70})"
    for combo in "${COMBOS[@]}"; do
      read -r label en resv <<<"$combo"
      log="$LOG_ROOT/${label}/${bench}.log"
      [ -f "$log" ] || { printf "%-8s | (no log)\n" "$label"; continue; }
      cycles=$(awk -F'[ ,]' '/cycles=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
      ipc=$(awk -F'=' '/IPC=/ {print $NF}' "$log" | tail -1)
      rhit=$(awk -F'[()=%]' '/dcache read misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1)}}' "$log" | tail -1)
      whit=$(awk -F'[()=%]' '/dcache write misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1)}}' "$log" | tail -1)
      printf "%-8s | %-9s | %-12s | %-12s | %-8s\n" \
        "$label" "${cycles:-?}" "${ipc:-?}" "${rhit:-?}%" "${whit:-?}%"
    done
  } | tee "$out"
done
echo "Logs under $LOG_ROOT"
