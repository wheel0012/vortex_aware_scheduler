#!/usr/bin/env bash
# Sweep: GTO / gCAWS / iPAWS  x  bfs / kmeans / hotspot / sgemm3 / blackscholes / vecadd
# perf=3, cores=1 warps=32 threads=32 + l2cache
#
# Usage: ./worklogs/scripts/sweep_3policy_6bench.sh

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
declare -A SCHED=( [GTO]=1 [gCAWS]=3 [iPAWS]=4 )

# Workload table: bench_label  app_name  args
# args="" means use Makefile default OPTS.
declare -A BENCH_ARGS=(
  [bfs]=""
  [kmeans]="-f100 -p5000"
  [hotspot]="512 1 2 temp_512 power_512 output.out"
  [sgemm3]="-n128"
  [blackscholes]=""
  [vecadd]="-n100000"
  [spmv]="-i ${ROOT_DIR}/tests/opencl/spmv/Dubcova3.mtx,${ROOT_DIR}/tests/opencl/spmv/Dubcova3.vec"
)
BENCHES=(bfs kmeans hotspot sgemm3 blackscholes vecadd spmv)

# Auto-increment runN naming (run1, run2, ... runN+1)
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
  echo "# Sweep — GTO / gCAWS / iPAWS x 7 workloads"
  echo
  echo "Timestamp: $(date)"
  echo
  echo "## Sim configure"
  echo
  echo "- cores=$CORES, warps=$WARPS, threads=$THREADS, l2cache=on, perf=3"
  echo "- base flags: \`$BASE_FLAGS\`"
  echo "- VORTEX_SCHED: 1=GTO, 3=gCAWS, 4=iPAWS"
  echo
  echo "## Workload inputs"
  echo
  echo "| Workload | Args |"
  echo "|---|---|"
  for b in "${BENCHES[@]}"; do
    args="${BENCH_ARGS[$b]}"
    [ -z "$args" ] && args="(Makefile default OPTS)"
    echo "| $b | \`$args\` |"
  done
  echo
  echo "Note: blackscholes \`optionCount\` is hardcoded at \`128*128\` in main.cc (~448KB working set; default was \`16*16\`)."
  echo
} > "$SUMMARY"

# Run sweep
for label in GTO gCAWS iPAWS; do
  sched=${SCHED[$label]}
  conf="$BASE_FLAGS -DVORTEX_SCHED=$sched"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  build_log="$cfg_dir/build.log"

  echo "===== [$label] building (VORTEX_SCHED=$sched) =====" | tee "$build_log"
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$build_log" 2>&1; then
    echo "  sim build FAIL"
    continue
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$build_log" 2>&1; then
    echo "  runtime build FAIL"
    continue
  fi

  for bench in "${BENCHES[@]}"; do
    args="${BENCH_ARGS[$bench]}"
    blog="$cfg_dir/${bench}.log"
    extra=()
    [ -n "$args" ] && extra=(--args="$args")
    echo ">>> [$label/$bench] args=\"$args\"" | tee -a "$blog"
    (
      cd "$BUILD_DIR" && \
      CONFIGS="$conf" timeout 3600 ./ci/blackbox.sh \
        --driver=simx --app="$bench" \
        --cores=$CORES --warps=$WARPS --threads=$THREADS \
        --l2cache --perf=3 \
        "${extra[@]}" >> "../${blog#$ROOT_DIR/}" 2>&1
    )
    rc=$?
    ipc_line=$(grep "^PERF: instrs" "$blog" | tail -1)
    echo "  rc=$rc  $ipc_line"
  done
done

# Build summary table per workload
{
  echo
  echo "## Results"
  echo
  for bench in "${BENCHES[@]}"; do
    echo "### $bench"
    echo
    printf "| Policy | instrs | cycles | IPC | dc_read_hit | dc_read_misses | l2_read_misses |\n"
    printf "|---|---|---|---|---|---|---|\n"
    for label in GTO gCAWS iPAWS; do
      log="$LOG_ROOT/$label/${bench}.log"
      [ -f "$log" ] || { printf "| %s | (no log) |\n" "$label"; continue; }
      instrs=$(awk -F'[ ,]' '/instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
      cycles=$(awk -F'[ ,]' '/cycles=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
      ipc=$(awk -F'=' '/IPC=/ {print $NF}' "$log" | tail -1)
      rhit=$(awk -F'[()=%]' '/^PERF: core[0-9]+: dcache read misses/ {for (i=1;i<=NF;i++) if ($i ~ /hit ratio/) {print $(i+1); exit}}' "$log")
      rmiss=$(awk '/^PERF: dcache read misses/ {match($0, /misses=([0-9]+)/, m); print m[1]; exit}' "$log")
      l2miss=$(awk '/^PERF: l2cache read misses/ {match($0, /misses=([0-9]+)/, m); print m[1]; exit}' "$log")
      printf "| %s | %s | %s | %s | %s%% | %s | %s |\n" \
        "$label" "${instrs:-?}" "${cycles:-?}" "${ipc:-?}" "${rhit:-?}" "${rmiss:-?}" "${l2miss:-?}"
    done
    echo
  done
} >> "$SUMMARY"

echo "DONE: $LOG_ROOT"
echo "Summary: $SUMMARY"
