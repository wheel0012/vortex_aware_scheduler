#!/usr/bin/env bash

set -u
set -o pipefail

# ============================================================
# CCWS large-input / small-cache benchmark sweep
#
# Focus:
#   Make MPKI differences easier to see by combining larger
#   benchmark inputs with smaller L1D caches.
#
# Default benchmarks:
#   bfs     : largest in-repo graph file
#   kmeans  : synthetic larger point/feature set
#   sgemm3  : matrix multiply with a small local work-group tile
#
# Usage:
#   chmod +x benchmark_ccws_large_input_sweep.sh
#   ./benchmark_ccws_large_input_sweep.sh
#
# Useful overrides:
#   BENCHMARKS="bfs kmeans" ./benchmark_ccws_large_input_sweep.sh
#   BFS_INPUTS="/abs/path/graph65536.txt /abs/path/graph1MW_6.txt" ./benchmark_ccws_large_input_sweep.sh
#   BFS_INPUT_NAMES="g64k g1mw" ./benchmark_ccws_large_input_sweep.sh
#   KMEANS_ARGS="-f256 -p8192" ./benchmark_ccws_large_input_sweep.sh
#   SGEMM3_ARGS="-n128 -t4" ./benchmark_ccws_large_input_sweep.sh
#   CACHE_NAMES="c8k_w2 c16k_w4" CACHE_SIZES="8192 16384" CACHE_WAYS="2 4" ./benchmark_ccws_large_input_sweep.sh
#   INCLUDE_TINY_CACHE=1 ./benchmark_ccws_large_input_sweep.sh
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${ROOT_DIR:-${SCRIPT_DIR}}"
cd "${ROOT_DIR}"

CORES="${CORES:-1}"
WARPS="${WARPS:-32}"
THREADS="${THREADS:-16}"
PERF="${PERF:-3}"
DRIVER="${DRIVER:-simx}"

read -r -a BENCH_ARR <<< "${BENCHMARKS:-bfs kmeans sgemm3}"

KMEANS_ARGS="${KMEANS_ARGS:--f256 -p4096}"
SGEMM3_ARGS="${SGEMM3_ARGS:--n64 -t4}"

read -r -a BFS_INPUT_NAMES_ARR <<< "${BFS_INPUT_NAMES:-g4k g64k g1mw}"
read -r -a BFS_INPUT_ARGS_ARR  <<< "${BFS_INPUTS:-${ROOT_DIR}/tests/opencl/bfs/graph4096.txt ${ROOT_DIR}/tests/opencl/bfs/graph65536.txt ${ROOT_DIR}/tests/opencl/bfs/graph1MW_6.txt}"

if [[ ${#BFS_INPUT_NAMES_ARR[@]} -ne ${#BFS_INPUT_ARGS_ARR[@]} ]]; then
  echo "BFS_INPUT_NAMES and BFS_INPUTS must have the same number of entries."
  exit 1
fi

if [[ "${INCLUDE_TINY_CACHE:-0}" == "1" ]]; then
  read -r -a CACHE_NAMES_ARR <<< "${CACHE_NAMES:-c4k_w1 c8k_w2 c16k_w4}"
  read -r -a CACHE_SIZES_ARR <<< "${CACHE_SIZES:-4096 8192 16384}"
  read -r -a CACHE_WAYS_ARR  <<< "${CACHE_WAYS:-1 2 4}"
else
  read -r -a CACHE_NAMES_ARR <<< "${CACHE_NAMES:-c8k_w2 c16k_w4}"
  read -r -a CACHE_SIZES_ARR <<< "${CACHE_SIZES:-8192 16384}"
  read -r -a CACHE_WAYS_ARR  <<< "${CACHE_WAYS:-2 4}"
fi

if [[ ${#CACHE_NAMES_ARR[@]} -ne ${#CACHE_SIZES_ARR[@]} ||
      ${#CACHE_NAMES_ARR[@]} -ne ${#CACHE_WAYS_ARR[@]} ]]; then
  echo "CACHE_NAMES, CACHE_SIZES, and CACHE_WAYS must have the same number of entries."
  exit 1
fi

read -r -a CCWS_K_ARR   <<< "${CCWS_KS:-16 32}"
read -r -a CCWS_VTA_ARR <<< "${CCWS_VTAS:-16 32}"
read -r -a CCWS_CAP_ARR <<< "${CCWS_CAPS:-2 4}"

BASE_LLS="${BASE_LLS:-100}"
DECAY_PERIOD="${DECAY_PERIOD:-1}"
DECAY_STEP="${DECAY_STEP:-1}"
LLS_CUTOFF="${LLS_CUTOFF:-0}"

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
OUT_DIR="ccws_large_input_sweep_${TIMESTAMP}"
mkdir -p "${OUT_DIR}"

SUMMARY="${OUT_DIR}/summary.csv"
echo "benchmark,input_name,args,policy,config_name,cache_name,dcache_size,dcache_ways,cores,warps,threads,status,instrs,cycles,ipc,dcache_read_misses,dcache_write_misses,dcache_total_misses,read_mpki,write_mpki,total_mpki,mem_requests,mem_reads,mem_writes,mem_latency,mem_bank_util,ccws_vta_hits,ccws_throttled_loads,ccws_fallback_issues,ccws_max_lls,logfile" > "${SUMMARY}"

bench_exists() {
  local bench="$1"
  [[ -d "tests/opencl/${bench}" || -d "tests/regression/${bench}" || -d "tests/${bench}" ]]
}

csv_escape() {
  local value="$1"
  value="${value//\"/\"\"}"
  printf '"%s"' "${value}"
}

last_match() {
  local pattern="$1"
  local file="$2"
  grep -E "${pattern}" "${file}" | tail -n 1 || true
}

extract_number() {
  local line="$1"
  local sed_expr="$2"
  if [[ -z "${line}" ]]; then
    echo ""
  else
    sed -E "${sed_expr}" <<< "${line}"
  fi
}

mpki() {
  local misses="$1"
  local instrs="$2"
  awk -v m="${misses:-0}" -v i="${instrs:-0}" 'BEGIN { if (i > 0) printf "%.3f", (m * 1000.0) / i; else printf "" }'
}

summarize_log() {
  local bench="$1"
  local input_name="$2"
  local args="$3"
  local policy="$4"
  local config_name="$5"
  local cache_name="$6"
  local cache_size="$7"
  local cache_ways="$8"
  local status="$9"
  local log_file="${10}"

  local instr_line read_line write_line mem_line vta_line throttled_line fallback_line max_lls_line
  instr_line=$(last_match '^PERF: instrs=' "${log_file}")
  read_line=$(last_match 'dcache read misses=' "${log_file}")
  write_line=$(last_match 'dcache write misses=' "${log_file}")
  mem_line=$(last_match '^PERF: memory requests=' "${log_file}")
  vta_line=$(last_match '^PERF: ccws vta hits=' "${log_file}")
  throttled_line=$(last_match '^PERF: ccws throttled loads=' "${log_file}")
  fallback_line=$(last_match '^PERF: ccws fallback issues=' "${log_file}")
  max_lls_line=$(last_match '^PERF: ccws max lls=' "${log_file}")

  local instrs cycles ipc read_misses write_misses total_misses
  instrs=$(extract_number "${instr_line}" 's/.*instrs=([0-9]+), cycles=.*/\1/')
  cycles=$(extract_number "${instr_line}" 's/.*cycles=([0-9]+), IPC=.*/\1/')
  ipc=$(extract_number "${instr_line}" 's/.*IPC=([0-9.]+).*/\1/')

  if [[ "${status}" == "PASS" && -z "${instrs}" ]]; then
    status="NO_PERF"
  fi

  read_misses=$(extract_number "${read_line}" 's/.*dcache read misses=([0-9]+).*/\1/')
  write_misses=$(extract_number "${write_line}" 's/.*dcache write misses=([0-9]+).*/\1/')
  total_misses=$(( ${read_misses:-0} + ${write_misses:-0} ))

  local read_mpki write_mpki total_mpki
  read_mpki=$(mpki "${read_misses:-0}" "${instrs:-0}")
  write_mpki=$(mpki "${write_misses:-0}" "${instrs:-0}")
  total_mpki=$(mpki "${total_misses:-0}" "${instrs:-0}")

  local mem_requests mem_reads mem_writes mem_latency mem_bank_util
  mem_requests=$(extract_number "${mem_line}" 's/.*memory requests=([0-9]+).*/\1/')
  mem_reads=$(extract_number "${mem_line}" 's/.*reads=([0-9]+).*/\1/')
  mem_writes=$(extract_number "${mem_line}" 's/.*writes=([0-9]+).*/\1/')
  mem_latency=$(extract_number "$(last_match '^PERF: memory latency=' "${log_file}")" 's/.*memory latency=([0-9]+).*/\1/')
  mem_bank_util=$(extract_number "$(last_match '^PERF: memory bank stalls=' "${log_file}")" 's/.*utilization=([0-9]+)%.*/\1/')

  local vta_hits throttled_loads fallback_issues max_lls
  vta_hits=$(extract_number "${vta_line}" 's/.*ccws vta hits=([0-9]+).*/\1/')
  throttled_loads=$(extract_number "${throttled_line}" 's/.*ccws throttled loads=([0-9]+).*/\1/')
  fallback_issues=$(extract_number "${fallback_line}" 's/.*ccws fallback issues=([0-9]+).*/\1/')
  max_lls=$(extract_number "${max_lls_line}" 's/.*ccws max lls=([0-9]+).*/\1/')

  local escaped_args
  escaped_args=$(csv_escape "${args}")
  echo "${bench},${input_name},${escaped_args},${policy},${config_name},${cache_name},${cache_size},${cache_ways},${CORES},${WARPS},${THREADS},${status},${instrs},${cycles},${ipc},${read_misses},${write_misses},${total_misses},${read_mpki},${write_mpki},${total_mpki},${mem_requests},${mem_reads},${mem_writes},${mem_latency},${mem_bank_util},${vta_hits:-0},${throttled_loads:-0},${fallback_issues:-0},${max_lls:-0},${log_file}" >> "${SUMMARY}"
}

run_one() {
  local bench="$1"
  local input_name="$2"
  local args="$3"
  local policy="$4"
  local config_name="$5"
  local cache_name="$6"
  local cache_size="$7"
  local cache_ways="$8"
  local extra_configs="$9"

  if ! bench_exists "${bench}"; then
    echo "[SKIP] ${bench}: benchmark directory not found"
    return 0
  fi

  local log_dir="${OUT_DIR}/${bench}/${input_name}/${cache_name}"
  mkdir -p "${log_dir}"

  if [[ "${bench}" == "bfs" && ! -f "${args}" ]]; then
    local log_file="${log_dir}/${bench}_${input_name}_${policy}_${config_name}_${cache_name}_c${CORES}w${WARPS}t${THREADS}.log"
    echo "[SKIP] ${bench}/${input_name}: input file not found: ${args}" | tee "${log_file}"
    summarize_log "${bench}" "${input_name}" "${args}" "${policy}" "${config_name}" "${cache_name}" "${cache_size}" "${cache_ways}" "MISSING_INPUT" "${log_file}"
    echo
    return 0
  fi

  local log_file="${log_dir}/${bench}_${input_name}_${policy}_${config_name}_${cache_name}_c${CORES}w${WARPS}t${THREADS}.log"
  local configs="-DDCACHE_SIZE=${cache_size} -DDCACHE_NUM_WAYS=${cache_ways} ${extra_configs}"

  echo "============================================================"
  echo "[RUN] benchmark=${bench}"
  echo "[RUN] input=${input_name}"
  echo "[RUN] args=${args}"
  echo "[RUN] policy=${policy}"
  echo "[RUN] config=${config_name}"
  echo "[RUN] cache=${cache_name} (${cache_size}B/${cache_ways}way)"
  echo "[RUN] configs=${configs}"
  echo "[RUN] log=${log_file}"
  echo "============================================================"

  env -u DEBUG CONFIGS="${configs}" \
    ./ci/blackbox.sh \
      --driver="${DRIVER}" \
      --app="${bench}" \
      --cores="${CORES}" \
      --warps="${WARPS}" \
      --threads="${THREADS}" \
      --perf="${PERF}" \
      --args="${args}" \
      2>&1 | tee "${log_file}"

  local exit_code=${PIPESTATUS[0]}
  local status="PASS"
  if [[ ${exit_code} -ne 0 ]]; then
    status="FAIL"
  fi

  summarize_log "${bench}" "${input_name}" "${args}" "${policy}" "${config_name}" "${cache_name}" "${cache_size}" "${cache_ways}" "${status}" "${log_file}"
  echo
}

run_cache_point() {
  local bench="$1"
  local input_name="$2"
  local args="$3"
  local cache_name="$4"
  local cache_size="$5"
  local cache_ways="$6"

  run_one \
    "${bench}" \
    "${input_name}" \
    "${args}" \
    "GTO" \
    "baseline_gto" \
    "${cache_name}" \
    "${cache_size}" \
    "${cache_ways}" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::GTO"

  for k in "${CCWS_K_ARR[@]}"; do
    for vta in "${CCWS_VTA_ARR[@]}"; do
      for cap in "${CCWS_CAP_ARR[@]}"; do
        run_one \
          "${bench}" \
          "${input_name}" \
          "${args}" \
          "CCWS" \
          "dyn_base${BASE_LLS}_k${k}_vta${vta}_cap${cap}" \
          "${cache_name}" \
          "${cache_size}" \
          "${cache_ways}" \
          "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
           -DVX_CCWS_DYNAMIC_LLDS=1 \
           -DVX_CCWS_BASE_LLS=${BASE_LLS} \
           -DVX_CCWS_K_THROTTLE=${k} \
           -DVX_CCWS_VTA_SIZE=${vta} \
           -DVX_CCWS_LLS_CUTOFF=${LLS_CUTOFF} \
           -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=${cap} \
           -DVX_CCWS_LLS_DECAY_PERIOD=${DECAY_PERIOD} \
           -DVX_CCWS_LLS_DECAY_STEP=${DECAY_STEP}"
      done
    done
  done
}

echo "============================================================"
echo "Starting CCWS large-input sweep"
echo "Output directory: ${OUT_DIR}"
echo "Benchmarks: ${BENCH_ARR[*]}"
echo "BFS inputs: ${BFS_INPUT_NAMES_ARR[*]}"
echo "KMEANS_ARGS=${KMEANS_ARGS}"
echo "SGEMM3_ARGS=${SGEMM3_ARGS}"
echo "Caches: ${CACHE_NAMES_ARR[*]}"
echo "CCWS K: ${CCWS_K_ARR[*]}"
echo "CCWS VTA: ${CCWS_VTA_ARR[*]}"
echo "CCWS caps: ${CCWS_CAP_ARR[*]}"
echo "Cores=${CORES}, Warps=${WARPS}, Threads=${THREADS}, Perf=${PERF}"
echo "============================================================"
echo

for bench in "${BENCH_ARR[@]}"; do
  if [[ "${bench}" == "bfs" ]]; then
    for input_idx in "${!BFS_INPUT_NAMES_ARR[@]}"; do
      for cache_idx in "${!CACHE_NAMES_ARR[@]}"; do
        run_cache_point \
          "${bench}" \
          "${BFS_INPUT_NAMES_ARR[input_idx]}" \
          "${BFS_INPUT_ARGS_ARR[input_idx]}" \
          "${CACHE_NAMES_ARR[cache_idx]}" \
          "${CACHE_SIZES_ARR[cache_idx]}" \
          "${CACHE_WAYS_ARR[cache_idx]}"
      done
    done
  else
    input_name="large"
    args=""
    if [[ "${bench}" == "kmeans" ]]; then
      args="${KMEANS_ARGS}"
    elif [[ "${bench}" == "sgemm3" ]]; then
      args="${SGEMM3_ARGS}"
    fi
    for cache_idx in "${!CACHE_NAMES_ARR[@]}"; do
      run_cache_point \
        "${bench}" \
        "${input_name}" \
        "${args}" \
        "${CACHE_NAMES_ARR[cache_idx]}" \
        "${CACHE_SIZES_ARR[cache_idx]}" \
        "${CACHE_WAYS_ARR[cache_idx]}"
    done
  fi
done

echo "============================================================"
echo "Large-input sweep complete"
echo "Summary: ${SUMMARY}"
echo "Logs: ${OUT_DIR}"
echo "============================================================"
