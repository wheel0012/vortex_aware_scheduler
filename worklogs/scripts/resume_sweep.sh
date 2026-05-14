#!/usr/bin/env bash
# Resume a partially-completed sweep_4policy run.
# Scans <run_dir> for missing/incomplete *.perf2.log files and re-runs only those,
# then regenerates SUMMARY.md.
#
# Usage:
#   ./worklogs/scripts/resume_sweep.sh                 # auto-picks latest runN/
#   ./worklogs/scripts/resume_sweep.sh worklogs/runs/run3

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
  # Sweet-spot inputs (match sweep_4policy.sh).
  [bfs]="${ROOT_DIR}/tests/opencl/bfs/graph4k.txt"
  [kmeans]="-f100 -p1000"
  [hotspot]="128 1 2 temp_128 power_128 output.out"
  [sgemm3]="-n96"
  [blackscholes]=""
  [vecadd]="-n10000"
)
BENCHES=(bfs kmeans hotspot sgemm3 blackscholes vecadd)
PERF=2

RUNS_DIR="$ROOT_DIR/worklogs/runs"
if [ $# -ge 1 ]; then
  LOG_ROOT="$(cd "$1" && pwd)"
else
  LOG_ROOT=$(ls -dt "$RUNS_DIR"/run* 2>/dev/null | head -1)
fi
[ -z "${LOG_ROOT:-}" ] && { echo "no runN/ dir found"; exit 1; }
echo "Resuming: $LOG_ROOT"

is_complete() {
  local log="$1"
  [ -f "$log" ] && grep -q "^PERF: instrs=" "$log"
}

declare -A MISSING_FOR_POLICY
NEED_BUILD=()
TODO_LIST=()
for label in "${POLICIES[@]}"; do
  any_missing=0
  for bench in "${BENCHES[@]}"; do
    log="$LOG_ROOT/$label/${bench}.perf${PERF}.log"
    if ! is_complete "$log"; then
      TODO_LIST+=("$label/$bench")
      MISSING_FOR_POLICY[$label]+=" $bench"
      any_missing=1
    fi
  done
  [ $any_missing -eq 1 ] && NEED_BUILD+=("$label")
done

if [ ${#TODO_LIST[@]} -eq 0 ]; then
  echo "Nothing to resume — all logs already complete."
else
  echo "Missing logs: ${#TODO_LIST[@]}"
  for t in "${TODO_LIST[@]}"; do echo "  $t"; done
fi

for label in "${NEED_BUILD[@]}"; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  build_log="$cfg_dir/build.log"

  echo "===== [$label] rebuild (VORTEX_SCHED=$sched) =====" | tee -a "$build_log"
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$build_log" 2>&1; then
    echo "  sim build FAIL — skip $label"
    continue
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$build_log" 2>&1; then
    echo "  runtime build FAIL — skip $label"
    continue
  fi

  for bench in ${MISSING_FOR_POLICY[$label]}; do
    args="${BENCH_ARGS[$bench]}"
    blog="$cfg_dir/${bench}.perf${PERF}.log"
    : > "$blog"
    extra=()
    [ -n "$args" ] && extra=(--args="$args")
    echo ">>> [$label/$bench/perf$PERF] args=\"$args\"" | tee -a "$blog"
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

SUMMARY="$LOG_ROOT/SUMMARY.md"
{
  echo "# Sweep (4 policy) — GTO / RR / gCAWS / iPAWS x 6 workloads (sweet-spot)"
  echo
  echo "Timestamp: $(date) (resumed)"
  echo
  echo "## Sim configure"
  echo
  echo "- cores=$CORES, warps=$WARPS, threads=$THREADS, l2cache=on, perf=$PERF (CLASS_MEM)"
  echo "- base flags: \`$BASE_FLAGS\`"
  echo "- VORTEX_SCHED: 1=GTO, 2=RR, 3=gCAWS, 4=iPAWS"
  echo
  echo "## Workload inputs (sweet-spot: WS 3-25x L1, well within L2=1MB)"
  echo
  echo "| Workload | Args |"
  echo "|---|---|"
  for b in "${BENCHES[@]}"; do
    args="${BENCH_ARGS[$b]}"
    [ -z "$args" ] && args="(Makefile default OPTS)"
    echo "| $b | \`$args\` |"
  done
  echo
  echo "## Results (perf=$PERF, CLASS_MEM)"
  echo
  echo "Columns: IPC, Speedup_RR = IPC_policy/IPC_RR, dc_read_hit, MPKI = read_misses*1000/instrs, MPKI_RR = MPKI_policy/MPKI_RR."
  echo
  for bench in "${BENCHES[@]}"; do
    echo "### $bench"
    echo
    printf "| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |\n"
    printf "|---|---|---|---|---|---|---|---|---|\n"
    rr_log="$LOG_ROOT/RR/${bench}.perf${PERF}.log"
    rr_instrs=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]; exit}}' "$rr_log" 2>/dev/null)
    rr_ipc=$(awk -F'=' '/IPC=/ {print $NF; exit}' "$rr_log" 2>/dev/null)
    rr_rmiss=$(awk '/^PERF: core[0-9]+: dcache read misses/ {match($0, /misses=([0-9]+)/, m); print m[1]; exit}' "$rr_log" 2>/dev/null)
    rr_mpki=""
    [ -n "$rr_rmiss" ] && [ -n "$rr_instrs" ] && rr_mpki=$(awk -v m="$rr_rmiss" -v i="$rr_instrs" 'BEGIN{if (i+0>0) printf "%.2f", m*1000/i}')
    for label in "${POLICIES[@]}"; do
      log="$LOG_ROOT/$label/${bench}.perf${PERF}.log"
      if ! is_complete "$log"; then printf "| %s | (incomplete) |\n" "$label"; continue; fi
      instrs=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]; exit}}' "$log")
      cycles=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]; exit}}' "$log")
      ipc=$(awk -F'=' '/IPC=/ {print $NF; exit}' "$log")
      rhit=$(awk '/^PERF: core[0-9]+: dcache read misses/ {match($0, /hit ratio=(-?[0-9]+)/, m); print m[1]; exit}' "$log")
      rmiss=$(awk '/^PERF: core[0-9]+: dcache read misses/ {match($0, /misses=([0-9]+)/, m); print m[1]; exit}' "$log")
      speedup=""
      [ -n "$ipc" ] && [ -n "$rr_ipc" ] && speedup=$(awk -v a="$ipc" -v b="$rr_ipc" 'BEGIN{if (b+0>0) printf "%.3f", a/b}')
      mpki=""
      [ -n "$rmiss" ] && [ -n "$instrs" ] && mpki=$(awk -v m="$rmiss" -v i="$instrs" 'BEGIN{if (i+0>0) printf "%.2f", m*1000/i}')
      mpki_ratio=""
      [ -n "$mpki" ] && [ -n "$rr_mpki" ] && mpki_ratio=$(awk -v a="$mpki" -v b="$rr_mpki" 'BEGIN{if (b+0>0) printf "%.3f", a/b}')
      printf "| %s | %s | %s | %s | %s | %s%% | %s | %s | %s |\n" \
        "$label" "${instrs:-?}" "${cycles:-?}" "${ipc:-?}" "${speedup:-?}" "${rhit:-?}" "${rmiss:-?}" "${mpki:-?}" "${mpki_ratio:-?}"
    done
    echo
  done
} > "$SUMMARY"

echo "DONE: $LOG_ROOT"
echo "Summary: $SUMMARY"
