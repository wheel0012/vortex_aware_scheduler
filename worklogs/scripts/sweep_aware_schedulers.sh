#!/usr/bin/env bash
# Sweep simx issue arbiters over selected OpenCL workloads.
#
# This is adapted from ../vortex_gcaws_kbs/worklogs/scripts for the current
# vortex_aware_scheduler tree:
#   - build directory defaults to ./build, whose config.mk points VORTEX_HOME
#     at this source tree.
#   - scheduler selection uses VORTEX_ARBITER, not VORTEX_SCHED.
#   - available policies are Priority=0, GTO=1, RR=2, Matrix=3, gCAWS=4, GTOS=5.
#   - default sweep uses RR/GTO/gCAWS; override with:
#       POLICIES="RR GTO gCAWS Priority Matrix" ./worklogs/scripts/sweep_aware_schedulers.sh
#
# Output:
#   worklogs/runs/run<N>/<policy>/{build.log,<bench>.perf<M>.log}
#   worklogs/runs/run<N>/SUMMARY.md

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

CORES="${CORES:-1}"
WARPS="${WARPS:-32}"
THREADS="${THREADS:-32}"
TIMEOUT_SEC="${TIMEOUT_SEC:-3600}"

# PERF 2 gives memory/cache stats; add PERF=1 for pipeline stats if needed.
PERFS_STR="${PERFS:-2}"
POLICIES_STR="${POLICIES:-RR GTO gCAWS}"
BENCHES_STR="${BENCHES:-bfs kmeans hotspot sgemm3 blackscholes vecadd streamcluster}"

# Keep this separate so users can add hardware knobs without editing the script.
EXTRA_CONFIGS="${EXTRA_CONFIGS:-}"
BASE_FLAGS="-DPERF_ENABLE $EXTRA_CONFIGS"

declare -A ARBITER=(
  [Priority]=0
  [GTO]=1
  [RR]=2
  [Matrix]=3
  [gCAWS]=4
  [GTOS]=5
  [GTOStrict]=5
)

declare -A BENCH_ARGS=(
  [bfs]="${ROOT_DIR}/tests/opencl/bfs/graph128k.txt"
  [kmeans]="-f100 -p5000"
  [hotspot]="128 1 2 temp_128 power_128 output.out"
  [sgemm3]="-n128"
  [blackscholes]=""
  [vecadd]="-n10000"
  [streamcluster]="2 4 4 16 16 16 none output.txt 1 -t gpu -d 0"
)

read -r -a POLICIES_ARR <<< "$POLICIES_STR"
read -r -a BENCHES_ARR <<< "$BENCHES_STR"
read -r -a PERFS_ARR <<< "$PERFS_STR"

if [ ! -f "$BUILD_DIR/config.mk" ]; then
  echo "Missing $BUILD_DIR/config.mk. Run ./configure first, or set BUILD_DIR." >&2
  exit 1
fi

if [ ! -x "$BUILD_DIR/ci/blackbox.sh" ]; then
  echo "Missing $BUILD_DIR/ci/blackbox.sh. The configured build tree looks incomplete." >&2
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
done

RUNS_DIR="$ROOT_DIR/worklogs/runs"
mkdir -p "$RUNS_DIR"
NEXT_N=1
while [ -d "$RUNS_DIR/run$NEXT_N" ]; do
  NEXT_N=$((NEXT_N + 1))
done
LOG_ROOT="$RUNS_DIR/run$NEXT_N"
mkdir -p "$LOG_ROOT"

SUMMARY="$LOG_ROOT/SUMMARY.md"
{
  echo "# Vortex Aware Scheduler Sweep"
  echo
  echo "Timestamp: $(date)"
  echo
  echo "## Configuration"
  echo
  echo "- root: \`$ROOT_DIR\`"
  echo "- build: \`$BUILD_DIR\`"
  echo "- cores=$CORES, warps=$WARPS, threads=$THREADS, l2cache=on"
  echo "- perf classes: \`$PERFS_STR\`"
  echo "- base flags: \`$BASE_FLAGS\`"
  echo "- VORTEX_ARBITER: Priority=0, GTO=1, RR=2, Matrix=3, gCAWS=4, GTOS=5"
  echo
  echo "## Workloads"
  echo
  echo "| Workload | Args |"
  echo "|---|---|"
  for bench in "${BENCHES_ARR[@]}"; do
    args="${BENCH_ARGS[$bench]:-}"
    [ -z "$args" ] && args="(Makefile default OPTS)"
    echo "| $bench | \`$args\` |"
  done
  echo
} > "$SUMMARY"

echo "Sweep output dir: $LOG_ROOT"

for label in "${POLICIES_ARR[@]}"; do
  arb="${ARBITER[$label]}"
  conf="$BASE_FLAGS -DVORTEX_ARBITER=$arb"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  build_log="$cfg_dir/build.log"

  echo "===== [$label] building (VORTEX_ARBITER=$arb) =====" | tee "$build_log"
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$build_log" 2>&1; then
    echo "  sim build FAIL; see $build_log"
    continue
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$build_log" 2>&1; then
    echo "  runtime build FAIL; see $build_log"
    continue
  fi

  for perf in "${PERFS_ARR[@]}"; do
    for bench in "${BENCHES_ARR[@]}"; do
      args="${BENCH_ARGS[$bench]:-}"
      blog="$cfg_dir/${bench}.perf${perf}.log"
      extra=()
      [ -n "$args" ] && extra=(--args="$args")

      echo ">>> [$label/$bench/perf$perf] args=\"$args\"" | tee "$blog"
      (
        cd "$BUILD_DIR" && \
        CONFIGS="$conf" timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
          --driver=simx --app="$bench" \
          --cores="$CORES" --warps="$WARPS" --threads="$THREADS" \
          --l2cache --perf="$perf" \
          "${extra[@]}" >> "$blog" 2>&1
      )
      rc=$?
      ipc_line=$(grep "^PERF: instrs" "$blog" | tail -1 || true)
      echo "  [$label/$bench/perf$perf] rc=$rc  $ipc_line"
    done
  done
done

{
  echo
  echo "## Results"
  echo
  echo "IPC speedup is normalized to RR for each workload/perf class when RR exists."
  echo

  for perf in "${PERFS_ARR[@]}"; do
    echo "## perf=$perf"
    echo
    for bench in "${BENCHES_ARR[@]}"; do
      echo "### $bench"
      echo
      printf "| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |\n"
      printf "|---|---|---|---|---|---|---|---|\n"

      rr_log="$LOG_ROOT/RR/${bench}.perf${perf}.log"
      rr_ipc=$(awk -F'=' '/IPC=/ {print $NF}' "$rr_log" 2>/dev/null | tail -1)

      for label in "${POLICIES_ARR[@]}"; do
        log="$LOG_ROOT/$label/${bench}.perf${perf}.log"
        if [ ! -f "$log" ]; then
          printf "| %s | (no log) | | | | | | |\n" "$label"
          continue
        fi
        instrs=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^instrs=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
        cycles=$(awk -F'[ ,]' '/^PERF: instrs=/ {for (i=1;i<=NF;i++) if ($i ~ /^cycles=/) {split($i,a,"="); print a[2]}}' "$log" | tail -1)
        ipc=$(awk -F'=' '/IPC=/ {print $NF}' "$log" | tail -1)
        rhit=$(awk '/^PERF: core[0-9]+: dcache read misses/ {match($0, /hit ratio=(-?[0-9]+)/, m); print m[1]; exit}' "$log")
        rmiss=$(awk '/^PERF: core[0-9]+: dcache read misses/ {match($0, /misses=([0-9]+)/, m); print m[1]; exit}' "$log")

        speedup=""
        [ -n "$ipc" ] && [ -n "$rr_ipc" ] && speedup=$(awk -v a="$ipc" -v b="$rr_ipc" 'BEGIN{if (b+0>0) printf "%.3f", a/b}')

        mpki=""
        [ -n "$rmiss" ] && [ -n "$instrs" ] && mpki=$(awk -v m="$rmiss" -v i="$instrs" 'BEGIN{if (i+0>0) printf "%.2f", m*1000/i}')

        printf "| %s | %s | %s | %s | %s | %s%% | %s | %s |\n" \
          "$label" "${instrs:-?}" "${cycles:-?}" "${ipc:-?}" "${speedup:-?}" "${rhit:-?}" "${rmiss:-?}" "${mpki:-?}"
      done
      echo
    done
  done
} >> "$SUMMARY"

echo "DONE: $LOG_ROOT"
echo "Summary: $SUMMARY"
