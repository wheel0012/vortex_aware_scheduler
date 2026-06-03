#!/usr/bin/env bash
# KMeans locality-distance sweep for RR/GTO without scheduler trace.
#
# Output:
#   worklogs/experiments/kmeans_distance_runs/run<N>/run.log
#   worklogs/experiments/kmeans_distance_runs/manifest.tsv

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
OUT_ROOT="${OUT_ROOT:-$ROOT_DIR/worklogs/experiments/kmeans_distance_runs}"

POINTS="${POINTS:-512}"
FEATURES_STR="${FEATURES:-32 64 128}"
L1D_KIB_STR="${L1D_KIB:-16 32 64}"
POLICIES_STR="${POLICIES:-RR GTO}"
CLUSTERS="${CLUSTERS:-5}"
LOOPS="${LOOPS:-1}"
CORES="${CORES:-1}"
WARPS="${WARPS:-32}"
THREADS="${THREADS:-32}"
ISSUE_WIDTH="${ISSUE_WIDTH:-1}"
LSU_BLOCKS="${LSU_BLOCKS:-1}"
IBUF_SIZE="${IBUF_SIZE:-64}"
LSUQ_IN_SIZE="${LSUQ_IN_SIZE:-8}"
LSUQ_OUT_SIZE="${LSUQ_OUT_SIZE:-32}"
LINE_SIZE="${LINE_SIZE:-128}"
WAYS="${WAYS:-16}"
L1_LATENCY="${L1_LATENCY:-2}"
L2_ENABLE="${L2_ENABLE:-0}"
L2_CACHE_SIZE="${L2_CACHE_SIZE:-}"
L2_LATENCY="${L2_LATENCY:-}"
TIMEOUT_SEC="${TIMEOUT_SEC:-900}"
USER_PC_BASE="${USER_PC_BASE:-0x80000000}"
USER_PC_FROM="${USER_PC_FROM:-0x80000094}"
USER_PC_TO="${USER_PC_TO:-0x80000230}"
EXTRA_CONFIGS="${EXTRA_CONFIGS:-}"

declare -A ARBITER=(
  [GTO]=1
  [RR]=2
)

read -r -a FEATURES_ARR <<< "$FEATURES_STR"
read -r -a L1D_KIB_ARR <<< "$L1D_KIB_STR"
read -r -a POLICIES_ARR <<< "$POLICIES_STR"

if [ ! -x "$BUILD_DIR/ci/blackbox.sh" ]; then
  echo "Missing $BUILD_DIR/ci/blackbox.sh. Run ./configure first, or set BUILD_DIR." >&2
  exit 1
fi

next_run_id() {
  local max_id=0
  local path base id
  while IFS= read -r path; do
    base="$(basename "$path")"
    id="${base#run}"
    case "$id" in
      ''|*[!0-9]*) continue ;;
    esac
    if [ "$id" -gt "$max_id" ]; then
      max_id="$id"
    fi
  done < <(find "$OUT_ROOT" -maxdepth 1 -type d -name 'run[0-9]*' 2>/dev/null)
  echo $((max_id + 1))
}

reserve_run_dir() {
  local id="$1"
  local name dir
  while :; do
    name="run${id}"
    dir="$OUT_ROOT/$name"
    if mkdir "$dir" 2>/dev/null; then
      RUN_NAME="$name"
      RUN_DIR="$dir"
      NEXT_RUN_ID=$((id + 1))
      return 0
    fi
    id=$((id + 1))
  done
}

extract_last_number() {
  local pattern="$1"
  local file="$2"
  grep -E "$pattern" "$file" | tail -1 | sed -E "s/.*$pattern.*/\\1/"
}

mkdir -p "$OUT_ROOT"
MANIFEST="$OUT_ROOT/manifest.tsv"
MANIFEST_HEADER="run\tstatus\tpolicy\tpoints\tfeatures\tclusters\tloops\tl1d_kib\tl2_enabled\tl2_size\tl2_latency\tthreads\twarps\tcycles\tipc\tl1d_reads\tl1d_read_misses\tl1d_hit_ratio\tl2_reads\tl2_read_misses\tl2_hit_ratio\tuserpc_span\tstreak_count\tstreak_avg\tstreak_p50\tstreak_p90\tstreak_max\tsame_wid_issue_rate\twid_switch_rate\tuserpc_l1d_reads\tuserpc_l1d_read_misses\tuserpc_l1d_hits\tuserpc_l2_hits\tuserpc_memory_misses\tuserpc_avg_reuse_distance\tlane_l1d_reads\tlane_l1d_hits\tlane_avg_reuse_distance\tlane_access_intra\tlane_access_inter\tlane_hit_intra\tlane_hit_inter\tlane_l1_same_reuse_accesses\tlane_l1_same_reuse_hits\tlane_l1_same_reuse_hit_rate\tlane_l1_inter_reuse_accesses\tlane_l1_inter_reuse_hits\tlane_l1_inter_reuse_hit_rate\tlane_l2_same_reuse_hits\tlane_l2_same_reuse_hit_rate\tlane_l2_same_requests\tlane_l2_same_misses\tlane_l2_same_request_hit_rate\tlane_l2_thread_local_hits\tlane_l2_thread_local_hit_rate\tlane_l2_thread_local_requests\tlane_l2_thread_local_misses\tlane_l2_thread_local_request_hit_rate\tlane_l2_intra_reuse_hits\tlane_l2_intra_reuse_hit_rate\tlane_l2_intra_requests\tlane_l2_intra_misses\tlane_l2_intra_request_hit_rate\tlane_l2_inter_reuse_hits\tlane_l2_inter_reuse_hit_rate\tlane_l2_inter_requests\tlane_l2_inter_misses\tlane_l2_inter_request_hit_rate\tlane_mem_same_reuse_misses\tlane_mem_same_reuse_miss_rate\tlane_mem_inter_reuse_misses\tlane_mem_inter_reuse_miss_rate\tlog"
if [ ! -f "$MANIFEST" ]; then
  printf "%b\n" "$MANIFEST_HEADER" > "$MANIFEST"
elif [ "$(sed -n '1p' "$MANIFEST")" != "$(printf "%b" "$MANIFEST_HEADER")" ]; then
  MANIFEST="$OUT_ROOT/manifest_streak.tsv"
  if [ -f "$MANIFEST" ] && [ "$(sed -n '1p' "$MANIFEST")" != "$(printf "%b" "$MANIFEST_HEADER")" ]; then
    MANIFEST="$OUT_ROOT/manifest_reuse.tsv"
  fi
  if [ ! -f "$MANIFEST" ]; then
    printf "%b\n" "$MANIFEST_HEADER" > "$MANIFEST"
  fi
fi

NEXT_RUN_ID="$(next_run_id)"
for features in "${FEATURES_ARR[@]}"; do
  for l1d_kib in "${L1D_KIB_ARR[@]}"; do
    for policy in "${POLICIES_ARR[@]}"; do
      if [ -z "${ARBITER[$policy]+x}" ]; then
        echo "Unknown policy: $policy" >&2
        exit 1
      fi

      reserve_run_dir "$NEXT_RUN_ID"
      run_name="$RUN_NAME"
      run_dir="$RUN_DIR"
      mkdir -p "$run_dir/pocl"
      log="$run_dir/run.log"
      args="-p${POINTS} -f${features} -n${CLUSTERS} -m${CLUSTERS} -l${LOOPS}"
      l1d_size=$((l1d_kib * 1024))
      arb="${ARBITER[$policy]}"
      configs="-DNUM_CORES=${CORES} -DNUM_WARPS=${WARPS} -DNUM_THREADS=${THREADS} -DISSUE_WIDTH=${ISSUE_WIDTH} -DNUM_LSU_BLOCKS=${LSU_BLOCKS} -DIBUF_SIZE=${IBUF_SIZE} -DLSUQ_IN_SIZE=${LSUQ_IN_SIZE} -DLSUQ_OUT_SIZE=${LSUQ_OUT_SIZE} -DMEM_BLOCK_SIZE=${LINE_SIZE} -DPLATFORM_MEMORY_DATA_SIZE=${LINE_SIZE} -DDCACHE_SIZE=${l1d_size} -DDCACHE_NUM_WAYS=${WAYS} -DSIMX_CACHE_LATENCY=${L1_LATENCY} -DVORTEX_ARBITER=${arb} ${EXTRA_CONFIGS}"
      blackbox_l2_args=()
      if [ "$L2_ENABLE" != "0" ]; then
        configs="$configs -DL2_ENABLE"
        blackbox_l2_args=(--l2cache)
      fi
      if [ -n "$L2_CACHE_SIZE" ]; then
        configs="$configs -DL2_CACHE_SIZE=${L2_CACHE_SIZE}"
      fi
      if [ -n "$L2_LATENCY" ]; then
        configs="$configs -DSIMX_L2_CACHE_LATENCY=${L2_LATENCY}"
      fi

      {
        echo "run=$run_name"
        echo "policy=$policy"
        echo "points=$POINTS features=$features clusters=$CLUSTERS loops=$LOOPS"
        echo "l1d_kib=$l1d_kib line_size=$LINE_SIZE ways=$WAYS l1_latency=$L1_LATENCY"
        echo "l2_enable=$L2_ENABLE l2_size=${L2_CACHE_SIZE:-default} l2_latency=${L2_LATENCY:-same_as_l1}"
        echo "cores=$CORES warps=$WARPS threads=$THREADS issue_width=$ISSUE_WIDTH lsu_blocks=$LSU_BLOCKS"
        echo "extra_configs=${EXTRA_CONFIGS:-none}"
        echo "userpc=${USER_PC_FROM}..${USER_PC_TO}"
        echo "args=$args"
        echo "configs=$configs"
      } > "$log"

      echo ">>> $run_name policy=$policy p=$POINTS f=$features l1d=${l1d_kib}KiB"
      (
        cd "$BUILD_DIR" && \
        CCACHE_DISABLE=1 \
        TMPDIR="$run_dir/pocl" \
        POCL_CACHE_DIR="$run_dir/pocl" \
        VX_USER_PC_BASE="$USER_PC_BASE" \
        VX_USER_PC_FROM="$USER_PC_FROM" \
        VX_USER_PC_TO="$USER_PC_TO" \
        CONFIGS="$configs" \
        timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
          --driver=simx --app=kmeans \
          --cores="$CORES" --warps="$WARPS" --threads="$THREADS" \
          "${blackbox_l2_args[@]}" --perf=2 --args="$args"
      ) >> "$log" 2>&1
      status=$?

      cycles="$(grep -E '^PERF: instrs=' "$log" | tail -1 | sed -E 's/.*cycles=([0-9]+).*/\1/')"
      ipc="$(grep -E '^PERF: instrs=' "$log" | tail -1 | sed -E 's/.*IPC=([0-9.]+).*/\1/')"
      l1d_reads="$(grep -E '^PERF: core0: dcache reads=' "$log" | tail -1 | sed -E 's/.*reads=([0-9]+).*/\1/')"
      l1d_read_misses="$(grep -E '^PERF: core0: dcache read misses=' "$log" | tail -1 | sed -E 's/.*misses=([0-9]+).*/\1/')"
      l1d_hit_ratio="$(grep -E '^PERF: core0: dcache read misses=' "$log" | tail -1 | sed -E 's/.*hit ratio=([0-9-]+)%.*/\1/')"
      l2_reads="$(grep -E '^PERF: l2cache reads=' "$log" | tail -1 | sed -E 's/.*reads=([0-9]+).*/\1/')"
      l2_read_misses="$(grep -E '^PERF: l2cache read misses=' "$log" | tail -1 | sed -E 's/.*misses=([0-9]+).*/\1/')"
      l2_hit_ratio="$(grep -E '^PERF: l2cache read misses=' "$log" | tail -1 | sed -E 's/.*hit ratio=([0-9-]+)%.*/\1/')"
      userpc_span="$(grep -E '^\[USERPC_PERF core=0\] issued=' "$log" | tail -1 | sed -E 's/.* span=([0-9]+).*/\1/')"
      issue_streak_line="$(grep -E '^\[USERPC_PERF core=0\] issue_streak ' "$log" | tail -1)"
      streak_count="$(echo "$issue_streak_line" | sed -E 's/.* count=([0-9]+).*/\1/')"
      streak_avg="$(echo "$issue_streak_line" | sed -E 's/.* avg=([0-9.]+).*/\1/')"
      streak_p50="$(echo "$issue_streak_line" | sed -E 's/.* p50=([0-9]+).*/\1/')"
      streak_p90="$(echo "$issue_streak_line" | sed -E 's/.* p90=([0-9]+).*/\1/')"
      streak_max="$(echo "$issue_streak_line" | sed -E 's/.* max=([0-9]+).*/\1/')"
      same_wid_issue_rate="$(echo "$issue_streak_line" | sed -E 's/.* same_wid_issue_rate=([0-9.]+)%.*/\1/')"
      wid_switch_rate="$(echo "$issue_streak_line" | sed -E 's/.* wid_switch_rate=([0-9.]+)%.*/\1/')"
      userpc_dcache_reads="$(grep -E '^\[USERPC_PERF core=0\] dcache ' "$log" | tail -1 | sed -E 's/.* reads=([0-9]+).*/\1/')"
      userpc_dcache_read_misses="$(grep -E '^\[USERPC_PERF core=0\] dcache ' "$log" | tail -1 | sed -E 's/.* read_misses=([0-9]+).*/\1/')"
      userpc_dcache_level_line="$(grep -E '^\[USERPC_PERF core=0\] dcache_level ' "$log" | tail -1)"
      userpc_l1_hits="$(echo "$userpc_dcache_level_line" | sed -E 's/.* l1_hits=([0-9]+).*/\1/')"
      userpc_l2_hits="$(echo "$userpc_dcache_level_line" | sed -E 's/.* l2_hits=([0-9]+).*/\1/')"
      userpc_memory_misses="$(echo "$userpc_dcache_level_line" | sed -E 's/.* memory_misses=([0-9]+).*/\1/')"
      userpc_avg_reuse_distance="$(grep -E '^\[USERPC_PERF core=0\] dcache_temporal ' "$log" | tail -1 | sed -E 's/.* avg_reuse_distance=([0-9]+).*/\1/')"
      lane_reads="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache ' "$log" | tail -1 | sed -E 's/.* reads=([0-9]+).*/\1/')"
      lane_hits="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache ' "$log" | tail -1 | sed -E 's/.* hits=([0-9]+).*/\1/')"
      lane_avg_reuse_distance="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache ' "$log" | tail -1 | sed -E 's/.* avg_reuse_distance=([0-9]+).*/\1/')"
      lane_access_intra="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_access_comp ' "$log" | tail -1 | sed -E 's/.* intra_warp(_temporal)?=([0-9]+).*/\2/')"
      lane_access_inter="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_access_comp ' "$log" | tail -1 | sed -E 's/.* inter_warp=([0-9]+).*/\1/')"
      lane_hit_intra="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_hit_comp ' "$log" | tail -1 | sed -E 's/.* intra_warp(_temporal)?=([0-9]+).*/\2/')"
      lane_hit_inter="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_hit_comp ' "$log" | tail -1 | sed -E 's/.* inter_warp=([0-9]+).*/\1/')"
      lane_temporal_reuse_line="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_temporal_reuse_hit_rate ' "$log" | tail -1)"
      lane_same_reuse_accesses="$(echo "$lane_temporal_reuse_line" | sed -E 's/.* same_warp_accesses=([0-9]+).*/\1/')"
      lane_same_reuse_hits="$(echo "$lane_temporal_reuse_line" | sed -E 's/.* same_warp_hits=([0-9]+).*/\1/')"
      lane_same_reuse_hit_rate="$(echo "$lane_temporal_reuse_line" | sed -E 's/.* same_warp_hit_rate=([0-9]+).*/\1/')"
      lane_inter_reuse_accesses="$(echo "$lane_temporal_reuse_line" | sed -E 's/.* inter_warp_accesses=([0-9]+).*/\1/')"
      lane_inter_reuse_hits="$(echo "$lane_temporal_reuse_line" | sed -E 's/.* inter_warp_hits=([0-9]+).*/\1/')"
      lane_inter_reuse_hit_rate="$(echo "$lane_temporal_reuse_line" | sed -E 's/.* inter_warp_hit_rate=([0-9]+).*/\1/')"
      lane_l2_temporal_reuse_line="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_l2_temporal_reuse_hit_rate ' "$log" | tail -1)"
      lane_l2_same_reuse_hits="$(echo "$lane_l2_temporal_reuse_line" | sed -E 's/.* same_warp_hits=([0-9]+).*/\1/')"
      lane_l2_same_reuse_hit_rate="$(echo "$lane_l2_temporal_reuse_line" | sed -E 's/.* same_warp_hit_rate=([0-9]+).*/\1/')"
      lane_l2_inter_reuse_hits="$(echo "$lane_l2_temporal_reuse_line" | sed -E 's/.* inter_warp_hits=([0-9]+).*/\1/')"
      lane_l2_inter_reuse_hit_rate="$(echo "$lane_l2_temporal_reuse_line" | sed -E 's/.* inter_warp_hit_rate=([0-9]+).*/\1/')"
      lane_l2_temporal_locality_line="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_l2_temporal_locality_hit_rate ' "$log" | tail -1)"
      lane_l2_thread_local_hits="$(echo "$lane_l2_temporal_locality_line" | sed -E 's/.* thread_local_hits=([0-9]+).*/\1/')"
      lane_l2_thread_local_hit_rate="$(echo "$lane_l2_temporal_locality_line" | sed -E 's/.* thread_local_hit_rate=([0-9]+).*/\1/')"
      lane_l2_intra_reuse_hits="$(echo "$lane_l2_temporal_locality_line" | sed -E 's/.* intra_warp_hits=([0-9]+).*/\1/')"
      lane_l2_intra_reuse_hit_rate="$(echo "$lane_l2_temporal_locality_line" | sed -E 's/.* intra_warp_hit_rate=([0-9]+).*/\1/')"
      lane_l2_request_line="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_l2_temporal_locality_requests ' "$log" | tail -1)"
      lane_l2_same_requests="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_same_warp_requests=([0-9]+).*/\1/')"
      lane_l2_same_misses="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_same_warp_misses=([0-9]+).*/\1/')"
      lane_l2_same_request_hit_rate="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_same_warp_hit_rate=([0-9]+).*/\1/')"
      lane_l2_thread_local_requests="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_thread_local_requests=([0-9]+).*/\1/')"
      lane_l2_thread_local_misses="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_thread_local_misses=([0-9]+).*/\1/')"
      lane_l2_thread_local_request_hit_rate="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_thread_local_hit_rate=([0-9]+).*/\1/')"
      lane_l2_intra_requests="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_intra_warp_requests=([0-9]+).*/\1/')"
      lane_l2_intra_misses="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_intra_warp_misses=([0-9]+).*/\1/')"
      lane_l2_intra_request_hit_rate="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_intra_warp_hit_rate=([0-9]+).*/\1/')"
      lane_l2_inter_requests="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_inter_warp_requests=([0-9]+).*/\1/')"
      lane_l2_inter_misses="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_inter_warp_misses=([0-9]+).*/\1/')"
      lane_l2_inter_request_hit_rate="$(echo "$lane_l2_request_line" | sed -E 's/.* l2_inter_warp_hit_rate=([0-9]+).*/\1/')"
      lane_mem_temporal_reuse_line="$(grep -E '^\[USERPC_PERF core=0\] lane_dcache_memory_temporal_reuse_miss_rate ' "$log" | tail -1)"
      lane_mem_same_reuse_misses="$(echo "$lane_mem_temporal_reuse_line" | sed -E 's/.* same_warp_misses=([0-9]+).*/\1/')"
      lane_mem_same_reuse_miss_rate="$(echo "$lane_mem_temporal_reuse_line" | sed -E 's/.* same_warp_miss_rate=([0-9]+).*/\1/')"
      lane_mem_inter_reuse_misses="$(echo "$lane_mem_temporal_reuse_line" | sed -E 's/.* inter_warp_misses=([0-9]+).*/\1/')"
      lane_mem_inter_reuse_miss_rate="$(echo "$lane_mem_temporal_reuse_line" | sed -E 's/.* inter_warp_miss_rate=([0-9]+).*/\1/')"

      row=(
        "$run_name" "$status" "$policy" "$POINTS" "$features" "$CLUSTERS" "$LOOPS" "$l1d_kib"
        "$L2_ENABLE" "${L2_CACHE_SIZE:-na}" "${L2_LATENCY:-na}" "$THREADS" "$WARPS"
        "${cycles:-na}" "${ipc:-na}" "${l1d_reads:-na}" "${l1d_read_misses:-na}" "${l1d_hit_ratio:-na}"
        "${l2_reads:-na}" "${l2_read_misses:-na}" "${l2_hit_ratio:-na}"
        "${userpc_span:-na}" "${streak_count:-na}" "${streak_avg:-na}" "${streak_p50:-na}" "${streak_p90:-na}" "${streak_max:-na}"
        "${same_wid_issue_rate:-na}" "${wid_switch_rate:-na}"
        "${userpc_dcache_reads:-na}" "${userpc_dcache_read_misses:-na}" "${userpc_l1_hits:-na}" "${userpc_l2_hits:-na}" "${userpc_memory_misses:-na}" "${userpc_avg_reuse_distance:-na}"
        "${lane_reads:-na}" "${lane_hits:-na}" "${lane_avg_reuse_distance:-na}"
        "${lane_access_intra:-na}" "${lane_access_inter:-na}" "${lane_hit_intra:-na}" "${lane_hit_inter:-na}"
        "${lane_same_reuse_accesses:-na}" "${lane_same_reuse_hits:-na}" "${lane_same_reuse_hit_rate:-na}"
        "${lane_inter_reuse_accesses:-na}" "${lane_inter_reuse_hits:-na}" "${lane_inter_reuse_hit_rate:-na}"
        "${lane_l2_same_reuse_hits:-na}" "${lane_l2_same_reuse_hit_rate:-na}"
        "${lane_l2_same_requests:-na}" "${lane_l2_same_misses:-na}" "${lane_l2_same_request_hit_rate:-na}"
        "${lane_l2_thread_local_hits:-na}" "${lane_l2_thread_local_hit_rate:-na}"
        "${lane_l2_thread_local_requests:-na}" "${lane_l2_thread_local_misses:-na}" "${lane_l2_thread_local_request_hit_rate:-na}"
        "${lane_l2_intra_reuse_hits:-na}" "${lane_l2_intra_reuse_hit_rate:-na}"
        "${lane_l2_intra_requests:-na}" "${lane_l2_intra_misses:-na}" "${lane_l2_intra_request_hit_rate:-na}"
        "${lane_l2_inter_reuse_hits:-na}" "${lane_l2_inter_reuse_hit_rate:-na}"
        "${lane_l2_inter_requests:-na}" "${lane_l2_inter_misses:-na}" "${lane_l2_inter_request_hit_rate:-na}"
        "${lane_mem_same_reuse_misses:-na}" "${lane_mem_same_reuse_miss_rate:-na}"
        "${lane_mem_inter_reuse_misses:-na}" "${lane_mem_inter_reuse_miss_rate:-na}"
        "$log"
      )
      (
        IFS=$'\t'
        printf "%s\n" "${row[*]}"
      ) >> "$MANIFEST"
    done
  done
done

echo "DONE: $OUT_ROOT"
echo "Manifest: $MANIFEST"
