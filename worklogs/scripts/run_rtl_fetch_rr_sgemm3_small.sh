#!/usr/bin/env bash
# Compare the RTL fetch scheduler change on a tiny sgemm3 input.
#
# What this checks:
#   1) simx_rr_fetch:
#        simx fetch policy is explicitly RR, issue arbiter is RR.
#   2) rtlsim_priority_fetch:
#        RTL fetch scheduler keeps the historical fixed-priority policy,
#        issue arbiter is RR.
#   3) rtlsim_rr_fetch:
#        RTL fetch scheduler uses FETCH_SCHED_RR, issue arbiter is RR.
#
# The goal is a small functional/perf sanity check, not a cycle-exact simx vs
# RTL proof.  Compare correctness, instruction count, IPC direction, and
# scheduler idle/stall counters first.
#
# Output:
#   worklogs/experiments/rtl_fetch_rr_sgemm3_small/run<N>/
#
# Useful overrides:
#   PERFS="1 2" ./worklogs/scripts/run_rtl_fetch_rr_sgemm3_small.sh
#   WARPS=32 THREADS=1 ./worklogs/scripts/run_rtl_fetch_rr_sgemm3_small.sh
#   EXTRA_CONFIGS="-DDCACHE_SIZE=4096" ./worklogs/scripts/run_rtl_fetch_rr_sgemm3_small.sh

set -u
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

CORES="${CORES:-1}"
WARPS="${WARPS:-16}"
THREADS="${THREADS:-1}"
SGEMM_N="${SGEMM_N:-4}"
SGEMM_TILE="${SGEMM_TILE:-2}"
PERFS="${PERFS:-1 2}"
TIMEOUT_SEC="${TIMEOUT_SEC:-900}"
EXTRA_CONFIGS="${EXTRA_CONFIGS:-}"

SCHED_RR=2
ISSUE_RR=2

OUT_BASE="$ROOT_DIR/worklogs/experiments/rtl_fetch_rr_sgemm3_small"

next_run_dir() {
  local base="$1"
  local n=1
  mkdir -p "$base"
  while [ -d "$base/run$n" ]; do
    n=$((n + 1))
  done
  echo "$base/run$n"
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

append_metrics() {
  local label="$1"
  local driver="$2"
  local fetch_policy="$3"
  local perf="$4"
  local rc="$5"
  local config="$6"
  local log="$7"

  local instrs cycles ipc sched_idle sched_stalls ibuf_stalls scrb_stalls load_lat
  local passed failed

  instrs="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/\1/p' "$log")"
  cycles="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/\2/p' "$log")"
  ipc="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/\3/p' "$log")"
  sched_idle="$(last_match 's/^PERF: scheduler idle=([0-9]+).*/\1/p' "$log")"
  sched_stalls="$(last_match 's/^PERF: scheduler stalls=([0-9]+).*/\1/p' "$log")"
  ibuf_stalls="$(last_match 's/^PERF: ibuffer stalls=([0-9]+).*/\1/p' "$log")"
  scrb_stalls="$(last_match 's/^PERF: scoreboard stalls=([0-9]+).*/\1/p' "$log")"
  load_lat="$(last_match 's/^PERF: load latency=([0-9]+) cycles.*/\1/p' "$log")"

  passed="$(grep -Eic 'passed|verify.*pass|correct' "$log" 2>/dev/null || true)"
  failed="$(grep -Eic '(^|[^[:alpha:]])failed([^[:alpha:]]|$)|opencl error|incorrect|mismatch|cl_build_program_failure|segmentation fault' "$log" 2>/dev/null || true)"

  {
    csv_field "$label"; printf ','
    csv_field "$driver"; printf ','
    csv_field "$fetch_policy"; printf ','
    csv_field "$perf"; printf ','
    csv_field "$rc"; printf ','
    csv_field "$instrs"; printf ','
    csv_field "$cycles"; printf ','
    csv_field "$ipc"; printf ','
    csv_field "$sched_idle"; printf ','
    csv_field "$sched_stalls"; printf ','
    csv_field "$ibuf_stalls"; printf ','
    csv_field "$scrb_stalls"; printf ','
    csv_field "$load_lat"; printf ','
    csv_field "$passed"; printf ','
    csv_field "$failed"; printf ','
    csv_field "$config"; printf ','
    csv_field "$log"; printf '\n'
  } >> "$METRICS_CSV"
}

run_case() {
  local label="$1"
  local driver="$2"
  local fetch_policy="$3"
  local config="$4"
  local case_dir="$OUT_DIR/$label"
  local tmp_dir="$case_dir/tmp"
  local pocl_cache_dir="$case_dir/pocl-cache"
  local args="-n${SGEMM_N} -t${SGEMM_TILE}"

  mkdir -p "$case_dir" "$tmp_dir" "$pocl_cache_dir"

  echo "[$label] driver=$driver fetch=$fetch_policy"
  echo "  CONFIGS=$config"

  for perf in $PERFS; do
    local log="$case_dir/sgemm3.n${SGEMM_N}.t${SGEMM_TILE}.perf${perf}.log"
    : > "$log"
    {
      echo ">>> label=$label driver=$driver fetch=$fetch_policy perf=$perf"
      echo ">>> args=\"$args\""
      echo ">>> CONFIGS=\"$config\""
      echo ">>> TMPDIR=\"$tmp_dir\""
      echo ">>> POCL_CACHE_DIR=\"$pocl_cache_dir\""
    } >> "$log"

    make -C "$BUILD_DIR/tests/opencl/sgemm3" clean >> "$log" 2>&1 || true

    (
      cd "$BUILD_DIR" || exit 1
      env -u DEBUG \
        CCACHE_DISABLE=1 \
        TMPDIR="$tmp_dir" \
        POCL_CACHE_DIR="$pocl_cache_dir" \
        CONFIGS="$config" \
        timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
        --driver="$driver" \
        --app=sgemm3 \
        --cores="$CORES" \
        --warps="$WARPS" \
        --threads="$THREADS" \
        --l2cache \
        --perf="$perf" \
        --args="$args" >> "$log" 2>&1
    )
    local rc=$?
    append_metrics "$label" "$driver" "$fetch_policy" "$perf" "$rc" "$config" "$log"

    local ipc
    ipc="$(last_match 's/^PERF: instrs=([0-9]+), cycles=([0-9]+), IPC=([-+0-9.]+)/instrs=\1 cycles=\2 IPC=\3/p' "$log")"
    echo "  perf$perf rc=$rc ${ipc:-no PERF line}"
  done
}

if [ ! -x "$BUILD_DIR/ci/blackbox.sh" ]; then
  echo "Missing $BUILD_DIR/ci/blackbox.sh. Run ./configure first, or set BUILD_DIR." >&2
  exit 1
fi

if [ $((SGEMM_TILE * SGEMM_TILE)) -gt $((WARPS * THREADS)) ]; then
  echo "sgemm3 tile ${SGEMM_TILE}x${SGEMM_TILE} needs at least $((SGEMM_TILE * SGEMM_TILE)) hardware threads." >&2
  echo "Current WARPS*THREADS=$((WARPS * THREADS))." >&2
  exit 1
fi

OUT_DIR="$(next_run_dir "$OUT_BASE")"
mkdir -p "$OUT_DIR"
METRICS_CSV="$OUT_DIR/metrics.csv"
SUMMARY="$OUT_DIR/SUMMARY.md"

cat > "$METRICS_CSV" <<'CSV'
label,driver,fetch_policy,perf,rc,instrs,cycles,ipc,scheduler_idle,scheduler_stalls,ibuffer_stalls,scoreboard_stalls,load_latency,pass_markers,fail_markers,config,log
CSV

cat > "$SUMMARY" <<EOF
# RTL Fetch RR sgemm3 Small Sanity

- input: \`sgemm3 -n${SGEMM_N} -t${SGEMM_TILE}\`
- cores/warps/threads: \`${CORES}/${WARPS}/${THREADS}\`
- perf classes: \`${PERFS}\`
- metrics: \`$METRICS_CSV\`
- note: simx and rtlsim are not expected to be cycle-exact; this is for correctness and scheduler-policy sanity.

EOF

SIMX_RR_CONFIG="-DVORTEX_SCHED_POLICY=$SCHED_RR -DVORTEX_ARBITER=$ISSUE_RR $EXTRA_CONFIGS"
RTL_PRIORITY_CONFIG="-DISSUE_ARB_RR $EXTRA_CONFIGS"
RTL_RR_CONFIG="-DISSUE_ARB_RR -DFETCH_SCHED_RR $EXTRA_CONFIGS"

echo "Output dir: $OUT_DIR"
run_case "simx_rr_fetch" "simx" "RR" "$SIMX_RR_CONFIG"
run_case "rtlsim_priority_fetch" "rtlsim" "priority" "$RTL_PRIORITY_CONFIG"
run_case "rtlsim_rr_fetch" "rtlsim" "RR" "$RTL_RR_CONFIG"

{
  echo "## Quick View"
  echo
  echo '```csv'
  cat "$METRICS_CSV"
  echo '```'
} >> "$SUMMARY"

echo
echo "DONE: $OUT_DIR"
echo "Metrics CSV: $METRICS_CSV"
echo "Summary: $SUMMARY"
