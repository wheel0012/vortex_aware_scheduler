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
MANIFEST_HEADER="run\tstatus\tpolicy\tpoints\tfeatures\tclusters\tloops\tl1d_kib\tthreads\twarps\tcycles\tipc\tdcache_reads\tdcache_read_misses\tdcache_hit_ratio\tuserpc_span\tstreak_count\tstreak_avg\tstreak_p50\tstreak_p90\tstreak_max\tsame_wid_issue_rate\twid_switch_rate\tuserpc_dcache_reads\tuserpc_dcache_read_misses\tuserpc_avg_reuse_distance\tlane_reads\tlane_hits\tlane_avg_reuse_distance\tlane_access_intra\tlane_access_inter\tlane_hit_intra\tlane_hit_inter\tlane_same_reuse_accesses\tlane_same_reuse_hits\tlane_same_reuse_hit_rate\tlane_inter_reuse_accesses\tlane_inter_reuse_hits\tlane_inter_reuse_hit_rate\tlog"
if [ ! -f "$MANIFEST" ]; then
  printf "%b\n" "$MANIFEST_HEADER" > "$MANIFEST"
elif [ "$(sed -n '1p' "$MANIFEST")" != "$(printf "%b" "$MANIFEST_HEADER")" ]; then
  MANIFEST="$OUT_ROOT/manifest_streak.tsv"
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

      {
        echo "run=$run_name"
        echo "policy=$policy"
        echo "points=$POINTS features=$features clusters=$CLUSTERS loops=$LOOPS"
        echo "l1d_kib=$l1d_kib line_size=$LINE_SIZE ways=$WAYS l1_latency=$L1_LATENCY"
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
          --perf=2 --args="$args"
      ) >> "$log" 2>&1
      status=$?

      cycles="$(grep -E '^PERF: instrs=' "$log" | tail -1 | sed -E 's/.*cycles=([0-9]+).*/\1/')"
      ipc="$(grep -E '^PERF: instrs=' "$log" | tail -1 | sed -E 's/.*IPC=([0-9.]+).*/\1/')"
      dcache_reads="$(grep -E '^PERF: core0: dcache reads=' "$log" | tail -1 | sed -E 's/.*reads=([0-9]+).*/\1/')"
      dcache_read_misses="$(grep -E '^PERF: core0: dcache read misses=' "$log" | tail -1 | sed -E 's/.*misses=([0-9]+).*/\1/')"
      dcache_hit_ratio="$(grep -E '^PERF: core0: dcache read misses=' "$log" | tail -1 | sed -E 's/.*hit ratio=([0-9-]+)%.*/\1/')"
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

      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$run_name" "$status" "$policy" "$POINTS" "$features" "$CLUSTERS" "$LOOPS" "$l1d_kib" "$THREADS" "$WARPS" \
        "${cycles:-na}" "${ipc:-na}" "${dcache_reads:-na}" "${dcache_read_misses:-na}" "${dcache_hit_ratio:-na}" \
        "${userpc_span:-na}" "${streak_count:-na}" "${streak_avg:-na}" "${streak_p50:-na}" "${streak_p90:-na}" "${streak_max:-na}" \
        "${same_wid_issue_rate:-na}" "${wid_switch_rate:-na}" \
        "${userpc_dcache_reads:-na}" "${userpc_dcache_read_misses:-na}" "${userpc_avg_reuse_distance:-na}" \
        "${lane_reads:-na}" "${lane_hits:-na}" "${lane_avg_reuse_distance:-na}" \
        "${lane_access_intra:-na}" "${lane_access_inter:-na}" "${lane_hit_intra:-na}" "${lane_hit_inter:-na}" \
        "${lane_same_reuse_accesses:-na}" "${lane_same_reuse_hits:-na}" "${lane_same_reuse_hit_rate:-na}" \
        "${lane_inter_reuse_accesses:-na}" "${lane_inter_reuse_hits:-na}" "${lane_inter_reuse_hit_rate:-na}" \
        "$log" >> "$MANIFEST"
    done
  done
done

echo "DONE: $OUT_ROOT"
echo "Manifest: $MANIFEST"
