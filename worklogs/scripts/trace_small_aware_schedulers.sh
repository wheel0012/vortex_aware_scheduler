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
#   CORES=1 WARPS=64 THREADS=1
#   POLICIES="RR GTO gCAWS"
#   BENCHES="bfs sgemm3"
#
# Useful overrides:
#   BENCHES="sgemm3" POLICIES="gCAWS" ./worklogs/scripts/trace_small_aware_schedulers.sh
#   BENCHES=bfs BFS_GRAPH=tests/opencl/bfs/graph32.txt ./worklogs/scripts/trace_small_aware_schedulers.sh
#   EXTRA_CONFIGS="-D..." TIMEOUT_SEC=1200 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   MEM_LATENCY=4 CACHE_LATENCY=1 BENCHES=sgemm3 SGEMM_N=4 SGEMM_TILE=2 WARPS=4 THREADS=2 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   LSU_BLOCKS=2 DCACHE_BANKS=4 BENCHES=sgemm3 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   ANALYZE_ARGS="--split-by-wspawn" ./worklogs/scripts/trace_small_aware_schedulers.sh
#   ANALYZE_ARGS="--from-event WSPAWN:3 --to-event TMC:24 --split-by-wspawn" ./worklogs/scripts/trace_small_aware_schedulers.sh
#   USER_FROM_EVENT=WSPAWN:3 USER_TO_EVENT=WSPAWN:4 USER_PC_FROM=0x1c4 USER_PC_TO=0x248 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   PERF=1 ./worklogs/scripts/trace_small_aware_schedulers.sh
#   PERFS="1 2" ./worklogs/scripts/trace_small_aware_schedulers.sh
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
WARPS="${WARPS:-64}"
THREADS="${THREADS:-1}"
TIMEOUT_SEC="${TIMEOUT_SEC:-900}"

POLICIES_STR="${POLICIES:-RR GTO gCAWS}"
BENCHES_STR="${BENCHES:-bfs sgemm3}"

# Leave PERF/PERFS empty by default so this stays focused on behavior tracing.
# Set PERF=1 or PERF=2 for one perf class, or PERFS="1 2" for both.
PERF="${PERF:-}"
PERFS_STR="${PERFS:-}"

# Keep this separate so users can add hardware knobs without editing the script.
EXTRA_CONFIGS="${EXTRA_CONFIGS:-}"
MEM_LATENCY="${MEM_LATENCY:-}"
CACHE_LATENCY="${CACHE_LATENCY:-}"
LSU_BLOCKS="${LSU_BLOCKS:-}"
DCACHE_BANKS="${DCACHE_BANKS:-}"
ANALYZE_ARGS_STR="${ANALYZE_ARGS:-}"
USER_PC_BASE="${USER_PC_BASE:-0x80000000}"
USER_PC_FROM="${USER_PC_FROM:-}"
USER_PC_TO="${USER_PC_TO:-}"
USER_PC_AUTO="${USER_PC_AUTO:-}"
USER_PC_SYMBOLS="${USER_PC_SYMBOLS:-}"
USER_FROM_EVENT="${USER_FROM_EVENT:-}"
USER_TO_EVENT="${USER_TO_EVENT:-}"
USER_SPLIT_BY_WSPAWN="${USER_SPLIT_BY_WSPAWN:-}"

SGEMM_N="${SGEMM_N:-24}"
SGEMM_TILE="${SGEMM_TILE:-8}"
BFS_GRAPH="${BFS_GRAPH:-$ROOT_DIR/tests/opencl/bfs/graph4k.txt}"
if [ -n "$BFS_GRAPH" ] && [ "${BFS_GRAPH#/}" = "$BFS_GRAPH" ]; then
  BFS_GRAPH="$ROOT_DIR/$BFS_GRAPH"
fi
BFS_WORK_GROUP_SIZE="unknown"

declare -A ARBITER=(
  [Priority]=0
  [GTO]=1
  [RR]=2
  [Matrix]=3
  [gCAWS]=4
)

declare -A BENCH_ARGS=(
  [bfs]="$BFS_GRAPH"
  [sgemm3]="-n${SGEMM_N} -t${SGEMM_TILE}"
  [vecadd]="-n64"
)

read -r -a POLICIES_ARR <<< "$POLICIES_STR"
read -r -a BENCHES_ARR <<< "$BENCHES_STR"
read -r -a ANALYZE_ARGS_ARR <<< "$ANALYZE_ARGS_STR"
if [ -n "$PERFS_STR" ]; then
  read -r -a PERFS_ARR <<< "$PERFS_STR"
elif [ -n "$PERF" ]; then
  PERFS_ARR=("$PERF")
else
  PERFS_ARR=("")
fi

PERF_MULTI=0
[ "${#PERFS_ARR[@]}" -gt 1 ] && PERF_MULTI=1

if [ -n "$USER_FROM_EVENT" ]; then
  ANALYZE_ARGS_ARR+=(--from-event "$USER_FROM_EVENT")
fi
if [ -n "$USER_TO_EVENT" ]; then
  ANALYZE_ARGS_ARR+=(--to-event "$USER_TO_EVENT")
fi
if [ -n "$USER_PC_FROM" ] || [ -n "$USER_PC_TO" ]; then
  ANALYZE_ARGS_ARR+=(--pc-base "$USER_PC_BASE")
  [ -n "$USER_PC_FROM" ] && ANALYZE_ARGS_ARR+=(--pc-from "$USER_PC_FROM")
  [ -n "$USER_PC_TO" ] && ANALYZE_ARGS_ARR+=(--pc-to "$USER_PC_TO")
fi
if [ -n "$USER_SPLIT_BY_WSPAWN" ] && [ "$USER_SPLIT_BY_WSPAWN" != "0" ] && [ "$USER_SPLIT_BY_WSPAWN" != "false" ]; then
  ANALYZE_ARGS_ARR+=(--split-by-wspawn)
fi

USER_MONITOR_LABEL="disabled"
if [ -n "$USER_FROM_EVENT" ] || [ -n "$USER_TO_EVENT" ] || [ -n "$USER_PC_FROM" ] || [ -n "$USER_PC_TO" ]; then
  USER_MONITOR_LABEL="pc_base=${USER_PC_BASE}"
  [ -n "$USER_FROM_EVENT" ] && USER_MONITOR_LABEL="$USER_MONITOR_LABEL, from_event=${USER_FROM_EVENT}"
  [ -n "$USER_TO_EVENT" ] && USER_MONITOR_LABEL="$USER_MONITOR_LABEL, to_event=${USER_TO_EVENT}"
  [ -n "$USER_PC_FROM" ] && USER_MONITOR_LABEL="$USER_MONITOR_LABEL, pc_from=${USER_PC_FROM}"
  [ -n "$USER_PC_TO" ] && USER_MONITOR_LABEL="$USER_MONITOR_LABEL, pc_to=${USER_PC_TO}"
fi

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

config_value() {
  local key="$1"
  awk -F'[[:space:]]*[?]?=[[:space:]]*' -v key="$key" '$1 == key {print $2; exit}' "$BUILD_DIR/config.mk"
}

tool_path() {
  local value="$1"
  local tooldir="$2"
  value="${value//\$\(TOOLDIR\)/$tooldir}"
  echo "$value"
}

user_pc_auto_enabled() {
  [ -n "$USER_PC_AUTO" ] && [ "$USER_PC_AUTO" != "0" ] && [ "$USER_PC_AUTO" != "false" ]
}

default_user_pc_symbols() {
  local bench="$1"
  if [ -n "$USER_PC_SYMBOLS" ]; then
    echo "$USER_PC_SYMBOLS"
    return
  fi
  case "$bench" in
    bfs) echo "BFS_1 BFS_2" ;;
    sgemm3) echo "sgemm3" ;;
    *) echo "$bench" ;;
  esac
}

infer_user_pc_args() {
  local bench="$1"
  local elf_dir="$2"
  local analysis_dir="$3"
  local tooldir
  local llvm_vortex
  local llvm_nm
  local symbols

  tooldir="$(config_value TOOLDIR)"
  llvm_vortex="$(tool_path "$(config_value LLVM_VORTEX)" "$tooldir")"
  llvm_nm="${LLVM_NM:-$llvm_vortex/bin/llvm-nm}"
  symbols="$(default_user_pc_symbols "$bench")"

  if [ ! -x "$llvm_nm" ]; then
    echo "USER_PC_AUTO requested, but llvm-nm is missing: $llvm_nm" >&2
    return 1
  fi

  python3 "$ROOT_DIR/worklogs/scripts/infer_device_pc_window.py" \
    --elf-dir "$elf_dir" \
    --symbols "$symbols" \
    --nm "$llvm_nm" \
    --pc-base "$USER_PC_BASE" \
    --summary "$analysis_dir/userpc_auto_window.txt"
}

annotate_device_dumps() {
  local elf_dir="$1"
  local dump

  [ -d "$elf_dir" ] || return 0
  for dump in "$elf_dir"/*.dump; do
    [ -f "$dump" ] || continue
    python3 "$ROOT_DIR/worklogs/scripts/annotate_vortex_dump.py" "$dump"
  done
}

ISSUE_WIDTH=$(((WARPS + 15) / 16))
if [ -n "$LSU_BLOCKS" ] && [ "$LSU_BLOCKS" -gt "$ISSUE_WIDTH" ]; then
  echo "Invalid LSU_BLOCKS=$LSU_BLOCKS for WARPS=$WARPS." >&2
  echo "Default ISSUE_WIDTH is ceil(WARPS/16)=$ISSUE_WIDTH, and simx requires NUM_LSU_BLOCKS <= ISSUE_WIDTH." >&2
  echo "Use WARPS=16 for LSU_BLOCKS=1, WARPS=32 for LSU_BLOCKS=2, or pass ISSUE_WIDTH explicitly via EXTRA_CONFIGS." >&2
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
  if [ "$bench" = "bfs" ] && [ ! -f "$BFS_GRAPH" ]; then
    echo "Missing BFS graph file: $BFS_GRAPH" >&2
    exit 1
  fi
  if [ "$bench" = "bfs" ]; then
    read -r bfs_nodes < "$BFS_GRAPH"
    BFS_WORK_GROUP_SIZE="$bfs_nodes"
    [ "$BFS_WORK_GROUP_SIZE" -gt 256 ] && BFS_WORK_GROUP_SIZE=256
    if [ "$BFS_WORK_GROUP_SIZE" -gt $((WARPS * THREADS)) ]; then
      echo "bfs graph $(basename "$BFS_GRAPH") uses work_group_size=$BFS_WORK_GROUP_SIZE." >&2
      echo "Current WARPS*THREADS=$((WARPS * THREADS)) cannot host that local size." >&2
      echo "Use at least WARPS*THREADS=$BFS_WORK_GROUP_SIZE, e.g. WARPS=32 THREADS=1 for graph32." >&2
      exit 1
    fi
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

if [ "${#PERFS_ARR[@]}" -eq 1 ] && [ -z "${PERFS_ARR[0]}" ]; then
  PERF_LABEL="disabled"
else
  PERF_LABEL="${PERFS_ARR[*]}"
fi
SHAPE_FLAGS="-DNUM_CORES=$CORES -DNUM_WARPS=$WARPS -DNUM_THREADS=$THREADS -DL2_ENABLE"
PERF_FLAGS=""
[ "$PERF_LABEL" != "disabled" ] && PERF_FLAGS="-DPERF_ENABLE"
MEM_FLAGS=""
[ -n "$MEM_LATENCY" ] && MEM_FLAGS="-DSIMX_FIXED_MEM_LATENCY=$MEM_LATENCY"
CACHE_FLAGS=""
[ -n "$CACHE_LATENCY" ] && CACHE_FLAGS="-DSIMX_CACHE_LATENCY=$CACHE_LATENCY"
HW_FLAGS=""
[ -n "$LSU_BLOCKS" ] && HW_FLAGS="$HW_FLAGS -DNUM_LSU_BLOCKS=$LSU_BLOCKS"
[ -n "$DCACHE_BANKS" ] && HW_FLAGS="$HW_FLAGS -DDCACHE_NUM_BANKS=$DCACHE_BANKS"
BASE_FLAGS="$SHAPE_FLAGS $PERF_FLAGS $MEM_FLAGS $CACHE_FLAGS $HW_FLAGS $EXTRA_CONFIGS"

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
  echo "- perf classes: \`$PERF_LABEL\`"
  echo "- mem latency: \`${MEM_LATENCY:-ramulator}\`"
  echo "- cache latency: \`${CACHE_LATENCY:-2}\`"
  echo "- lsu blocks: \`${LSU_BLOCKS:-default}\`"
  echo "- dcache banks: \`${DCACHE_BANKS:-default}\`"
  echo "- hw tweak flags: \`${HW_FLAGS:-none}\`"
  echo "- base flags: \`$BASE_FLAGS\`"
  echo "- trace env: \`VX_TRACE_WARP_SCHED=1\`"
  echo "- analyzer: \`sim/simx/analyze_warp_sched_trace.py\`"
  echo "- analyzer args: \`${ANALYZE_ARGS_STR:-none}\`"
  echo "- effective analyzer args: \`${ANALYZE_ARGS_ARR[*]:-none}\`"
  echo "- user PC monitor: \`$USER_MONITOR_LABEL\`"
  echo "- user PC auto: \`${USER_PC_AUTO:-disabled}\`"
  echo "- user PC symbols: \`${USER_PC_SYMBOLS:-bench defaults}\`"
  echo "- bfs graph: \`$BFS_GRAPH\`"
  echo "- bfs work-group size: \`$BFS_WORK_GROUP_SIZE\`"
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
  echo "| Policy | Perf | Workload | Status | Trace rows | Issued | Mismatch count | Mismatch rate | Fallback count | Fallback rate | Artifacts |"
  echo "|---|---|---|---|---:|---:|---:|---:|---:|---:|---|"
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
      for perf in "${PERFS_ARR[@]}"; do
        perf_label="${perf:-disabled}"
        echo "| $label | $perf_label | $bench | build_fail | 0 | ? | ? | ? | ? | ? | \`$policy_dir\` |" >> "$SUMMARY"
      done
    done
    continue
  fi
  if ! CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" DEBUG= -j"$(nproc)" >> "$build_log" 2>&1; then
    echo "  runtime build FAIL; see $build_log"
    for bench in "${BENCHES_ARR[@]}"; do
      for perf in "${PERFS_ARR[@]}"; do
        perf_label="${perf:-disabled}"
        echo "| $label | $perf_label | $bench | build_fail | 0 | ? | ? | ? | ? | ? | \`$policy_dir\` |" >> "$SUMMARY"
      done
    done
    continue
  fi

  for bench in "${BENCHES_ARR[@]}"; do
    args="${BENCH_ARGS[$bench]-}"
    for perf in "${PERFS_ARR[@]}"; do
      perf_label="${perf:-disabled}"
      if [ "$PERF_MULTI" -eq 1 ]; then
        bench_dir="$policy_dir/perf${perf}/$bench"
      else
        bench_dir="$policy_dir/$bench"
      fi
      analysis_dir="$bench_dir/analysis"
      mkdir -p "$analysis_dir"
      device_elf_dir=""
      if user_pc_auto_enabled; then
        device_elf_dir="$bench_dir/device_elf"
        mkdir -p "$device_elf_dir"
      fi

      trace_csv="$bench_dir/issue_trace.csv"
      run_log="$bench_dir/run.log"
      analyze_log="$bench_dir/analyze.log"

      extra=()
      [ -n "$args" ] && extra=(--args="$args")

      perf_arg=()
      [ -n "$perf" ] && perf_arg=(--perf="$perf")

      echo ">>> [$label/perf=$perf_label/$bench] args=\"$args\" trace=\"$trace_csv\"" | tee "$run_log"
      (
        cd "$BUILD_DIR" || exit 1
        env -u DEBUG \
        CONFIGS="$conf" \
        VX_TRACE_WARP_SCHED=1 \
        VX_TRACE_WARP_SCHED_FILE="$trace_csv" \
        VXBIN_SAVE_ELF_DIR="$device_elf_dir" \
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

      annotate_device_dumps "$device_elf_dir"

      if [ ! -s "$trace_csv" ]; then
        echo "  [$label/perf=$perf_label/$bench] $status, but trace CSV is missing or empty"
        echo "| $label | $perf_label | $bench | missing_trace($status) | 0 | ? | ? | ? | ? | ? | \`$bench_dir\` |" >> "$SUMMARY"
        continue
      fi

      analyze_args=("${ANALYZE_ARGS_ARR[@]}")
      if user_pc_auto_enabled && [ -z "$USER_PC_FROM" ] && [ -z "$USER_PC_TO" ] && [[ "$ANALYZE_ARGS_STR" != *"--pc-from"* ]] && [[ "$ANALYZE_ARGS_STR" != *"--pc-to"* ]]; then
        auto_pc_log="$analysis_dir/userpc_auto_infer.log"
        mapfile -t auto_pc_args < <(infer_user_pc_args "$bench" "$device_elf_dir" "$analysis_dir" 2> "$auto_pc_log")
        infer_rc=$?
        if [ "$infer_rc" -eq 0 ]; then
          analyze_args+=("${auto_pc_args[@]}")
          echo "  [$label/perf=$perf_label/$bench] USER_PC_AUTO symbols=\"$(default_user_pc_symbols "$bench")\" args=\"${auto_pc_args[*]}\""
        else
          echo "  [$label/perf=$perf_label/$bench] USER_PC_AUTO failed; analyzing without auto PC filter (see $auto_pc_log)"
        fi
      fi

      if ! python3 "$ROOT_DIR/sim/simx/analyze_warp_sched_trace.py" "$trace_csv" -o "$analysis_dir" "${analyze_args[@]}" > "$analyze_log" 2>&1; then
        echo "  [$label/perf=$perf_label/$bench] analyzer FAIL; see $analyze_log"
        status="analyze_fail($status)"
      fi

      summary_txt="$analysis_dir/summary.txt"
      rows=$(trace_rows "$trace_csv")
      issued=$(summary_value "$summary_txt" "Total issued instructions")
      mismatch_count=$(summary_value "$summary_txt" "Mismatch count")
      mismatch_rate=$(summary_value "$summary_txt" "Mismatch rate")
      fallback_count=$(summary_value "$summary_txt" "Fallback count")
      fallback_rate=$(summary_value "$summary_txt" "Fallback rate")

      echo "  [$label/perf=$perf_label/$bench] $status rows=$rows issued=${issued:-?} mismatch=${mismatch_rate:-?} fallback=${fallback_rate:-?}"
      echo "| $label | $perf_label | $bench | $status | $rows | ${issued:-?} | ${mismatch_count:-?} | ${mismatch_rate:-?} | ${fallback_count:-?} | ${fallback_rate:-?} | \`$bench_dir\` |" >> "$SUMMARY"
    done
  done
done

echo
echo "DONE: $LOG_ROOT"
echo "Summary: $SUMMARY"
