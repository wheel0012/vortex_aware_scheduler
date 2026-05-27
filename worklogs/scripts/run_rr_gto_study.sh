#!/usr/bin/env bash
# RR/GTO study driver for the three-part experiment:
#   1) micro: controlled arithmetic-chain sanity check
#   2) trace: small-input scheduler trace/diagnosis
#   3) macro: large-input IPC/MPKI cache-pressure sweep
#
# Outputs are written only under git-ignored directories:
#   worklogs/experiments/rr_gto_study/run<N>/
#   worklogs/trace_runs/run<N>/  (created by the trace phase)
#
# Useful overrides:
#   PHASES="micro trace macro" POLICIES="RR GTO" ./worklogs/scripts/run_rr_gto_study.sh
#   PHASES=macro CACHE_SIZES="4096 8192 16384" BFS_GRAPHS="graph128k.txt graph512k.txt" ./worklogs/scripts/run_rr_gto_study.sh
#   PHASES=macro MACRO_BENCHES="hotspot bfs" HOTSPOT_SIZES="128 512" ./worklogs/scripts/run_rr_gto_study.sh
#   PHASES=trace TRACE_BENCHES="gto_arith_chain bfs sgemm3" TRACE_SGEMM_N=24 TRACE_SGEMM_TILE=8 ./worklogs/scripts/run_rr_gto_study.sh
#   POLICIES="RR GTO gCAWS" PHASES=macro ./worklogs/scripts/run_rr_gto_study.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

SCHED_RR=2
POLICIES_STR="${POLICIES:-RR GTO}"
PHASES_STR="${PHASES:-micro trace macro}"
TIMEOUT_SEC="${TIMEOUT_SEC:-3600}"

declare -A ARBITER=(
  [Priority]=0
  [GTO]=1
  [RR]=2
  [Matrix]=3
  [gCAWS]=4
)

read -r -a POLICIES_ARR <<< "$POLICIES_STR"
read -r -a PHASES_ARR <<< "$PHASES_STR"

MICRO_WARPS="${MICRO_WARPS:-32}"
MICRO_THREADS="${MICRO_THREADS:-32}"
MICRO_CORES="${MICRO_CORES:-1}"
MICRO_GLOBAL_SIZE="${MICRO_GLOBAL_SIZE:-1024}"
MICRO_LOCAL_SIZE="${MICRO_LOCAL_SIZE:-32}"
MICRO_ITERS_LIST="${MICRO_ITERS_LIST:-16 64 256}"
MICRO_PERFS="${MICRO_PERFS:-1}"
MICRO_CPL_DUMP="${MICRO_CPL_DUMP:-1}"

TRACE_WARPS="${TRACE_WARPS:-32}"
TRACE_THREADS="${TRACE_THREADS:-2}"
TRACE_CORES="${TRACE_CORES:-1}"
TRACE_BENCHES="${TRACE_BENCHES:-gto_arith_chain bfs sgemm3}"
TRACE_BFS_GRAPH="${TRACE_BFS_GRAPH:-tests/opencl/bfs/graph32.txt}"
TRACE_SGEMM_N="${TRACE_SGEMM_N:-24}"
TRACE_SGEMM_TILE="${TRACE_SGEMM_TILE:-8}"
TRACE_PERFS="${TRACE_PERFS:-1}"
TRACE_TIMEOUT_SEC="${TRACE_TIMEOUT_SEC:-900}"

MACRO_CORES="${MACRO_CORES:-1}"
MACRO_WARPS="${MACRO_WARPS:-32}"
MACRO_THREADS="${MACRO_THREADS:-32}"
MACRO_PERFS="${MACRO_PERFS:-1 2}"
MACRO_BENCHES="${MACRO_BENCHES:-sgemm3 bfs}"
CACHE_SIZES="${CACHE_SIZES:-4096 8192 16384}"
LSU_BLOCKS_LIST="${LSU_BLOCKS_LIST:-1 2}"
DCACHE_BANKS="${DCACHE_BANKS:-4}"
SGEMM_N_LIST="${SGEMM_N_LIST:-128}"
SGEMM_TILE="${SGEMM_TILE:-16}"
BFS_GRAPHS="${BFS_GRAPHS:-graph128k.txt graph512k.txt}"
HOTSPOT_SIZES="${HOTSPOT_SIZES:-128}"
HOTSPOT_PYRAMID="${HOTSPOT_PYRAMID:-1}"
HOTSPOT_ITERS="${HOTSPOT_ITERS:-2}"
HOTSPOT_OUTPUT="${HOTSPOT_OUTPUT:-output.out}"

EXTRA_CONFIGS="${EXTRA_CONFIGS:-}"

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

has_phase() {
  local want="$1"
  local phase
  for phase in "${PHASES_ARR[@]}"; do
    [ "$phase" = "$want" ] && return 0
  done
  return 1
}

csv_field() {
  local value="${1:-}"
  value="${value//\"/\"\"}"
  printf '"%s"' "$value"
}

last_match() {
  local pattern="$1"
  local file="$2"
  sed -nE "$pattern" "$file" 2>/dev/null | tail -1
}

mpki() {
  local misses="$1"
  local instrs="$2"
  if [ -z "$misses" ] || [ -z "$instrs" ]; then
    return
  fi
  awk -v m="$misses" -v i="$instrs" 'BEGIN { if (i + 0 > 0) printf "%.3f", 1000.0 * m / i }'
}

next_run_dir() {
  local base="$1"
  local n=1
  mkdir -p "$base"
  while [ -d "$base/run$n" ]; do
    n=$((n + 1))
  done
  echo "$base/run$n"
}

build_config() {
  local conf="$1"
  local build_log="$2"

  : > "$build_log"
  echo "CONFIGS=$conf" >> "$build_log"
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j"$(nproc)" >> "$build_log" 2>&1 &&
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j"$(nproc)" >> "$build_log" 2>&1
}

clean_bench() {
  local bench="$1"
  local build_log="$2"
  if [ -d "$BUILD_DIR/tests/opencl/$bench" ]; then
    make -C "$BUILD_DIR/tests/opencl/$bench" clean >> "$build_log" 2>&1 || true
  fi
}

run_blackbox() {
  local conf="$1"
  local bench="$2"
  local args="$3"
  local perf="$4"
  local cores="$5"
  local warps="$6"
  local threads="$7"
  local log="$8"
  local cpl_dump="${9:-0}"

  local extra=()
  local perf_arg=()
  local env_extra=()

  [ -n "$args" ] && extra=(--args="$args")
  [ -n "$perf" ] && perf_arg=(--perf="$perf")
  if [ "$cpl_dump" != "0" ] && [ "$cpl_dump" != "false" ]; then
    env_extra=(VX_CPL_DUMP=1)
  fi

  : > "$log"
  (
    cd "$BUILD_DIR" || exit 1
    env -u DEBUG "${env_extra[@]}" CONFIGS="$conf" timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
      --driver=simx --app="$bench" \
      --cores="$cores" --warps="$warps" --threads="$threads" \
      --l2cache "${perf_arg[@]}" "${extra[@]}" >> "$log" 2>&1
  )
}

append_metrics() {
  local phase="$1"
  local config="$2"
  local bench="$3"
  local input="$4"
  local policy="$5"
  local perf="$6"
  local rc="$7"
  local log="$8"

  local instrs cycles ipc sched_idle ibuf_stalls scrb_stalls scrb_lsu load_lat
  local dc_reads dc_read_miss dc_read_hit dc_write_miss dc_write_hit
  local l2_read_miss l2_read_hit mem_req mem_reads mem_writes mem_lat coal_miss coal_hit
  local dc_read_mpki l2_read_mpki

  instrs="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/\1/p' "$log")"
  cycles="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/\2/p' "$log")"
  ipc="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/\3/p' "$log")"
  sched_idle="$(last_match 's/^PERF: scheduler idle=([0-9]+).*/\1/p' "$log")"
  ibuf_stalls="$(last_match 's/^PERF: ibuffer stalls=([0-9]+).*/\1/p' "$log")"
  scrb_stalls="$(last_match 's/^PERF: scoreboard stalls=([0-9]+).*/\1/p' "$log")"
  scrb_lsu="$(last_match 's/^PERF: scoreboard stalls=.*lsu=([0-9]+)%.*/\1/p' "$log")"
  load_lat="$(last_match 's/^PERF: load latency=([0-9]+) cycles.*/\1/p' "$log")"
  dc_reads="$(last_match 's/^PERF: core[0-9]+: dcache reads=([0-9]+).*/\1/p' "$log")"
  dc_read_miss="$(last_match 's/^PERF: core[0-9]+: dcache read misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\1/p' "$log")"
  dc_read_hit="$(last_match 's/^PERF: core[0-9]+: dcache read misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\2/p' "$log")"
  dc_write_miss="$(last_match 's/^PERF: core[0-9]+: dcache write misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\1/p' "$log")"
  dc_write_hit="$(last_match 's/^PERF: core[0-9]+: dcache write misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\2/p' "$log")"
  l2_read_miss="$(last_match 's/^PERF: l2cache read misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\1/p' "$log")"
  l2_read_hit="$(last_match 's/^PERF: l2cache read misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\2/p' "$log")"
  mem_req="$(last_match 's/^PERF: memory requests=([0-9]+) \(reads=([0-9]+), writes=([0-9]+)\).*/\1/p' "$log")"
  mem_reads="$(last_match 's/^PERF: memory requests=([0-9]+) \(reads=([0-9]+), writes=([0-9]+)\).*/\2/p' "$log")"
  mem_writes="$(last_match 's/^PERF: memory requests=([0-9]+) \(reads=([0-9]+), writes=([0-9]+)\).*/\3/p' "$log")"
  mem_lat="$(last_match 's/^PERF: memory latency=([0-9]+) cycles.*/\1/p' "$log")"
  coal_miss="$(last_match 's/^PERF: core[0-9]+: coalescer misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\1/p' "$log")"
  coal_hit="$(last_match 's/^PERF: core[0-9]+: coalescer misses=([0-9]+).*hit ratio=([-+0-9]+)%.*/\2/p' "$log")"

  dc_read_mpki="$(mpki "$dc_read_miss" "$instrs")"
  l2_read_mpki="$(mpki "$l2_read_miss" "$instrs")"

  {
    csv_field "$phase"; printf ','
    csv_field "$config"; printf ','
    csv_field "$bench"; printf ','
    csv_field "$input"; printf ','
    csv_field "$policy"; printf ','
    csv_field "$perf"; printf ','
    csv_field "$rc"; printf ','
    csv_field "$instrs"; printf ','
    csv_field "$cycles"; printf ','
    csv_field "$ipc"; printf ','
    csv_field "$sched_idle"; printf ','
    csv_field "$ibuf_stalls"; printf ','
    csv_field "$scrb_stalls"; printf ','
    csv_field "$scrb_lsu"; printf ','
    csv_field "$load_lat"; printf ','
    csv_field "$dc_reads"; printf ','
    csv_field "$dc_read_miss"; printf ','
    csv_field "$dc_read_mpki"; printf ','
    csv_field "$dc_read_hit"; printf ','
    csv_field "$dc_write_miss"; printf ','
    csv_field "$dc_write_hit"; printf ','
    csv_field "$l2_read_miss"; printf ','
    csv_field "$l2_read_mpki"; printf ','
    csv_field "$l2_read_hit"; printf ','
    csv_field "$mem_req"; printf ','
    csv_field "$mem_reads"; printf ','
    csv_field "$mem_writes"; printf ','
    csv_field "$mem_lat"; printf ','
    csv_field "$coal_miss"; printf ','
    csv_field "$coal_hit"; printf ','
    csv_field "$log"; printf '\n'
  } >> "$METRICS_CSV"
}

OUT_ROOT="$(next_run_dir "$ROOT_DIR/worklogs/experiments/rr_gto_study")"
mkdir -p "$OUT_ROOT"
METRICS_CSV="$OUT_ROOT/metrics.csv"
SUMMARY="$OUT_ROOT/SUMMARY.md"

cat > "$METRICS_CSV" <<'CSV'
phase,config,bench,input,policy,perf,rc,instrs,cycles,ipc,scheduler_idle,ibuffer_stalls,scoreboard_stalls,scoreboard_lsu_pct,load_latency,dcache_reads,dcache_read_misses,dcache_read_mpki,dcache_read_hit_pct,dcache_write_misses,dcache_write_hit_pct,l2_read_misses,l2_read_mpki,l2_read_hit_pct,memory_requests,memory_reads,memory_writes,memory_latency,coalescer_misses,coalescer_hit_pct,log
CSV

{
  echo "# RR/GTO Scheduler Study"
  echo
  echo "Timestamp: $(date)"
  echo
  echo "- phases: \`$PHASES_STR\`"
  echo "- policies: \`$POLICIES_STR\`"
  echo "- scheduler: fixed RR (\`VORTEX_SCHED=$SCHED_RR\`)"
  echo "- metrics: \`$METRICS_CSV\`"
  echo "- outputs: \`$OUT_ROOT\`"
  echo
} > "$SUMMARY"

echo "Study output dir: $OUT_ROOT"

if has_phase micro; then
  echo "=== phase: micro ==="
  MICRO_DIR="$OUT_ROOT/micro"
  mkdir -p "$MICRO_DIR"
  read -r -a MICRO_PERFS_ARR <<< "$MICRO_PERFS"

  for label in "${POLICIES_ARR[@]}"; do
    arb="${ARBITER[$label]}"
    conf="-DPERF_ENABLE -DNUM_CORES=$MICRO_CORES -DNUM_WARPS=$MICRO_WARPS -DNUM_THREADS=$MICRO_THREADS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb $EXTRA_CONFIGS"
    cfg_dir="$MICRO_DIR/$label"
    mkdir -p "$cfg_dir"
    build_log="$cfg_dir/build.log"
    echo "  [$label] build micro config"
    if ! build_config "$conf" "$build_log"; then
      echo "    build FAIL: $build_log"
      continue
    fi
    clean_bench gto_arith_chain "$build_log"

    for iters in $MICRO_ITERS_LIST; do
      args="-n${MICRO_GLOBAL_SIZE} -l${MICRO_LOCAL_SIZE} -i${iters}"
      for perf in "${MICRO_PERFS_ARR[@]}"; do
        log="$cfg_dir/gto_arith_chain.i${iters}.perf${perf}.log"
        echo "  [$label/gto_arith_chain/iters=$iters/perf$perf]"
        run_blackbox "$conf" gto_arith_chain "$args" "$perf" "$MICRO_CORES" "$MICRO_WARPS" "$MICRO_THREADS" "$log" "$MICRO_CPL_DUMP"
        rc=$?
        append_metrics micro "iters=$iters,warps=$MICRO_WARPS,threads=$MICRO_THREADS" gto_arith_chain "$args" "$label" "$perf" "$rc" "$log"
      done
    done
  done

  {
    echo "## Micro Phase"
    echo
    echo "- bench: \`gto_arith_chain\`"
    echo "- iters: \`$MICRO_ITERS_LIST\`"
    echo "- cpl dump: \`$MICRO_CPL_DUMP\`"
    echo
  } >> "$SUMMARY"
fi

if has_phase trace; then
  echo "=== phase: trace ==="
  TRACE_DIR="$OUT_ROOT/trace"
  mkdir -p "$TRACE_DIR"
  TRACE_LOG="$TRACE_DIR/trace_phase.log"
  TRACE_EXTRA_CONFIGS="-DVORTEX_SCHED=$SCHED_RR $EXTRA_CONFIGS"

  (
    cd "$ROOT_DIR" || exit 1
    env \
      POLICIES="$POLICIES_STR" \
      BENCHES="$TRACE_BENCHES" \
      WARPS="$TRACE_WARPS" \
      THREADS="$TRACE_THREADS" \
      CORES="$TRACE_CORES" \
      PERFS="$TRACE_PERFS" \
      BFS_GRAPH="$TRACE_BFS_GRAPH" \
      SGEMM_N="$TRACE_SGEMM_N" \
      SGEMM_TILE="$TRACE_SGEMM_TILE" \
      EXTRA_CONFIGS="$TRACE_EXTRA_CONFIGS" \
      TIMEOUT_SEC="$TRACE_TIMEOUT_SEC" \
      VX_CPL_DUMP=1 \
      ./worklogs/scripts/trace_small_aware_schedulers.sh
  ) | tee "$TRACE_LOG"
  trace_rc=${PIPESTATUS[0]}
  trace_root="$(sed -nE 's/^DONE: (.*)$/\1/p' "$TRACE_LOG" | tail -1)"
  {
    echo "## Trace Phase"
    echo
    echo "- status: \`$trace_rc\`"
    echo "- trace benches: \`$TRACE_BENCHES\`"
    echo "- trace root: \`${trace_root:-unknown}\`"
    echo "- trace phase log: \`$TRACE_LOG\`"
    echo
  } >> "$SUMMARY"
fi

if has_phase macro; then
  echo "=== phase: macro ==="
  MACRO_DIR="$OUT_ROOT/macro"
  mkdir -p "$MACRO_DIR"
  read -r -a MACRO_PERFS_ARR <<< "$MACRO_PERFS"

  for cache_size in $CACHE_SIZES; do
    for lsu_blocks in $LSU_BLOCKS_LIST; do
      cfg_name="dcache${cache_size}_lsu${lsu_blocks}_bank${DCACHE_BANKS}"
      for label in "${POLICIES_ARR[@]}"; do
        arb="${ARBITER[$label]}"
        cfg_dir="$MACRO_DIR/$cfg_name/$label"
        mkdir -p "$cfg_dir"
        conf="-DPERF_ENABLE -DNUM_CORES=$MACRO_CORES -DNUM_WARPS=$MACRO_WARPS -DNUM_THREADS=$MACRO_THREADS -DDCACHE_SIZE=$cache_size -DNUM_LSU_BLOCKS=$lsu_blocks -DDCACHE_NUM_BANKS=$DCACHE_BANKS -DVORTEX_SCHED=$SCHED_RR -DVORTEX_ARBITER=$arb $EXTRA_CONFIGS"
        build_log="$cfg_dir/build.log"

        echo "  [$cfg_name/$label] build macro config"
        if ! build_config "$conf" "$build_log"; then
          echo "    build FAIL: $build_log"
          continue
        fi

        for bench in $MACRO_BENCHES; do
          clean_bench "$bench" "$build_log"
          case "$bench" in
            sgemm3)
              for n in $SGEMM_N_LIST; do
                args="-n${n} -t${SGEMM_TILE}"
                for perf in "${MACRO_PERFS_ARR[@]}"; do
                  log="$cfg_dir/sgemm3.n${n}.t${SGEMM_TILE}.perf${perf}.log"
                  echo "  [$cfg_name/$label/sgemm3/n=$n/perf$perf]"
                  run_blackbox "$conf" sgemm3 "$args" "$perf" "$MACRO_CORES" "$MACRO_WARPS" "$MACRO_THREADS" "$log" 0
                  rc=$?
                  append_metrics macro "$cfg_name" sgemm3 "$args" "$label" "$perf" "$rc" "$log"
                done
              done
              ;;
            bfs)
              for graph in $BFS_GRAPHS; do
                if [ "${graph#/}" = "$graph" ]; then
                  graph_path="$ROOT_DIR/tests/opencl/bfs/$graph"
                else
                  graph_path="$graph"
                fi
                if [ ! -f "$graph_path" ]; then
                  echo "    missing BFS graph: $graph_path"
                  continue
                fi
                args="$graph_path"
                graph_name="$(basename "$graph_path" .txt)"
                for perf in "${MACRO_PERFS_ARR[@]}"; do
                  log="$cfg_dir/bfs.${graph_name}.perf${perf}.log"
                  echo "  [$cfg_name/$label/bfs/$graph_name/perf$perf]"
                  run_blackbox "$conf" bfs "$args" "$perf" "$MACRO_CORES" "$MACRO_WARPS" "$MACRO_THREADS" "$log" 0
                  rc=$?
                  append_metrics macro "$cfg_name" bfs "$graph_name" "$label" "$perf" "$rc" "$log"
                done
              done
              ;;
            hotspot)
              for size in $HOTSPOT_SIZES; do
                temp_file="temp_${size}"
                power_file="power_${size}"
                if [ ! -f "$ROOT_DIR/tests/opencl/hotspot/$temp_file" ] || [ ! -f "$ROOT_DIR/tests/opencl/hotspot/$power_file" ]; then
                  echo "    missing hotspot input: $temp_file/$power_file"
                  continue
                fi
                args="$size $HOTSPOT_PYRAMID $HOTSPOT_ITERS $temp_file $power_file $HOTSPOT_OUTPUT"
                for perf in "${MACRO_PERFS_ARR[@]}"; do
                  log="$cfg_dir/hotspot.${size}.iter${HOTSPOT_ITERS}.perf${perf}.log"
                  echo "  [$cfg_name/$label/hotspot/size=$size/iters=$HOTSPOT_ITERS/perf$perf]"
                  run_blackbox "$conf" hotspot "$args" "$perf" "$MACRO_CORES" "$MACRO_WARPS" "$MACRO_THREADS" "$log" 0
                  rc=$?
                  append_metrics macro "$cfg_name" hotspot "size=${size},iters=${HOTSPOT_ITERS}" "$label" "$perf" "$rc" "$log"
                done
              done
              ;;
            *)
              echo "    unsupported macro bench '$bench' (supported: sgemm3 bfs hotspot)"
              ;;
          esac
        done
      done
    done
  done

  {
    echo "## Macro Phase"
    echo
    echo "- benches: \`$MACRO_BENCHES\`"
    echo "- cache sizes: \`$CACHE_SIZES\`"
    echo "- lsu blocks: \`$LSU_BLOCKS_LIST\`"
    echo "- dcache banks: \`$DCACHE_BANKS\`"
    echo "- sgemm n: \`$SGEMM_N_LIST\`, tile: \`$SGEMM_TILE\`"
    echo "- bfs graphs: \`$BFS_GRAPHS\`"
    echo "- hotspot sizes: \`$HOTSPOT_SIZES\`, pyramid: \`$HOTSPOT_PYRAMID\`, iters: \`$HOTSPOT_ITERS\`"
    echo
  } >> "$SUMMARY"
fi

{
  echo "## Reading The Results"
  echo
  echo "- Plot \`ipc\` vs \`config\` to see RR/GTO curve shape across cache pressure."
  echo "- Plot \`dcache_read_mpki\` and \`l2_read_mpki\` next to IPC."
  echo "- For the trace phase, inspect each trace run's \`analysis/summary.txt\` plus \`CPL_DUMP READY_HIST\`, \`DIVERGE\`, and \`WARP_DIST\` in \`run.log\`."
  echo
} >> "$SUMMARY"

echo
echo "DONE: $OUT_ROOT"
echo "Metrics CSV: $METRICS_CSV"
echo "Summary: $SUMMARY"
