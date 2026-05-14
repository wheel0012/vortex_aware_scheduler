#!/usr/bin/env bash
# Sweep: GTO / RR / gCAWS / iPAWS  x  7 workloads  x  perf {2}
# perf=2 (CLASS_MEM only — perf=3/kMPMClassAll has a Vortex MPM dump bug:
# kernel-side vx_perf_dump captures ONE class snapshot per kernel run, but
# vx_start hardcodes CLASS_CORE for kMPMClassAll, so MEM-class CSR queries
# return stale CORE values. We use perf=2 directly to get accurate cache stats.
# Pipeline stats (perf=1) can be re-run separately if needed.
#
# Usage: ./worklogs/scripts/sweep_4policy.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

# Fixed sim config
CORES=1
WARPS=32
THREADS=32

# Base build flags (CACP off — RTL target excludes it)
BASE_FLAGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0"

# Policy table: label  VORTEX_SCHED
# 1=GTO, 2=RR, 3=gCAWS, 4=iPAWS
declare -A SCHED=( [GTO]=1 [RR]=2 [gCAWS]=3 [iPAWS]=4 )
POLICIES=(GTO RR gCAWS iPAWS)

# Workload table: bench_label  args
# Scaled inputs to put working set well above 16KB L1 (~12x thrash).
declare -A BENCH_ARGS=(
  # Sweet-spot inputs: working set ~50-400KB (3-25x L1=16KB, well within L2=1MB).
  # Goal: L1 thrash regime without crossing into L2/DRAM saturation.
  # spmv dropped — Dubcova3.mtx is ~9MB which overshoots L2 deeply.
  [bfs]="${ROOT_DIR}/tests/opencl/bfs/graph4k.txt"
  [kmeans]="-f100 -p1000"
  [hotspot]="128 1 2 temp_128 power_128 output.out"
  [sgemm3]="-n96"
  [blackscholes]=""
  [vecadd]="-n10000"
)
BENCHES=(bfs kmeans hotspot sgemm3 blackscholes vecadd)

# perf classes to sweep. 2=MEM (cache stats), 1=CORE (pipeline stats).
PERFS=(2)

# Auto-increment runN naming
RUNS_DIR="$ROOT_DIR/worklogs/runs"
mkdir -p "$RUNS_DIR"
NEXT_N=1
while [ -d "$RUNS_DIR/run$NEXT_N" ]; do
  NEXT_N=$((NEXT_N + 1))
done
LOG_ROOT="$RUNS_DIR/run$NEXT_N"
mkdir -p "$LOG_ROOT"
echo "Sweep output dir: $LOG_ROOT"

# Header
SUMMARY="$LOG_ROOT/SUMMARY.md"
{
  echo "# Sweep (4 policy) — GTO / RR / gCAWS / iPAWS x 6 workloads"
  echo
  echo "Timestamp: $(date)"
  echo
  echo "## Sim configure"
  echo
  echo "- cores=$CORES, warps=$WARPS, threads=$THREADS, l2cache=on, perf=${PERFS[*]} (CLASS_MEM)"
  echo "- base flags: \`$BASE_FLAGS\`"
  echo "- VORTEX_SCHED: 1=GTO, 2=RR, 3=gCAWS, 4=iPAWS"
  echo
  echo "## Workload inputs (sweet-spot: working set 3-25x L1, within L2)"
  echo
  echo "| Workload | Args |"
  echo "|---|---|"
  for b in "${BENCHES[@]}"; do
    args="${BENCH_ARGS[$b]}"
    [ -z "$args" ] && args="(Makefile default OPTS)"
    echo "| $b | \`$args\` |"
  done
  echo
  echo "Note: bfs uses freshly-generated \`graph16k.txt\` (16384 nodes, avg 6 edges)."
  echo "kmeans scaled to 50K points (~200KB working set, 12x over L1)."
  echo "blackscholes \`optionCount\` hardcoded at 128*128 in main.cc."
  echo "Cache stats from perf=2 (CLASS_MEM). Pipeline stats omitted (would need separate perf=1 run)."
  echo
} > "$SUMMARY"

# Build + run loop
for label in "${POLICIES[@]}"; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  build_log="$cfg_dir/build.log"

  echo "===== [$label] building (VORTEX_SCHED=$sched) =====" | tee "$build_log"
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$build_log" 2>&1; then
    echo "  sim build FAIL — skip"
    continue
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$build_log" 2>&1; then
    echo "  runtime build FAIL — skip"
    continue
  fi

  for perf in "${PERFS[@]}"; do
    for bench in "${BENCHES[@]}"; do
      args="${BENCH_ARGS[$bench]}"
      blog="$cfg_dir/${bench}.perf${perf}.log"
      extra=()
      [ -n "$args" ] && extra=(--args="$args")
      echo ">>> [$label/$bench/perf$perf] args=\"$args\"" | tee -a "$blog"
      (
        cd "$BUILD_DIR" && \
        CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
          --driver=simx --app="$bench" \
          --cores=$CORES --warps=$WARPS --threads=$THREADS \
          --l2cache --perf=$perf \
          "${extra[@]}" >> "../${blog#$ROOT_DIR/}" 2>&1
      )
      rc=$?
      ipc_line=$(grep "^PERF: instrs" "$blog" | tail -1)
      echo "  rc=$rc  $ipc_line"
    done
  done
done

# Generate summary table (perf=2 stats — cache + IPC)
{
  echo
  echo "## Results (perf=2, CLASS_MEM — accurate cache stats)"
  echo
  echo "Columns:"
  echo "- **IPC** = instrs / cycles."
  echo "- **Speedup_RR** = IPC_policy / IPC_RR (RR baseline, CAWA-paper convention)."
  echo "- **dc_read_hit** = L1 dcache read hit ratio."
  echo "- **MPKI** = dcache read_misses × 1000 / instrs."
  echo "- **MPKI_RR** = MPKI_policy / MPKI_RR."
  echo
  for bench in "${BENCHES[@]}"; do
    echo "### $bench"
    echo
    printf "| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |\n"
    printf "|---|---|---|---|---|---|---|---|---|\n"
    # RR baseline
    rr_log="$LOG_ROOT/RR/${bench}.perf2.log"
    rr_instrs=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]; exit}}' "$rr_log" 2>/dev/null)
    rr_cycles=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]; exit}}' "$rr_log" 2>/dev/null)
    rr_ipc=$(awk -F'=' '/IPC=/ {print $NF; exit}' "$rr_log" 2>/dev/null)
    rr_rmiss=$(awk '/^PERF: core[0-9]+: dcache read misses/ {match($0, /misses=([0-9]+)/, m); print m[1]; exit}' "$rr_log" 2>/dev/null)
    rr_mpki=""
    if [ -n "$rr_rmiss" ] && [ -n "$rr_instrs" ]; then
      rr_mpki=$(awk -v m="$rr_rmiss" -v i="$rr_instrs" 'BEGIN{if (i+0>0) printf "%.2f", m*1000/i}')
    fi
    for label in "${POLICIES[@]}"; do
      log="$LOG_ROOT/$label/${bench}.perf2.log"
      [ -f "$log" ] || { printf "| %s | (no log) |\n" "$label"; continue; }
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
} >> "$SUMMARY"

echo "DONE: $LOG_ROOT"
echo "Summary: $SUMMARY"
