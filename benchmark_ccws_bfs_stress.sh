#!/usr/bin/env bash

set -u
set -o pipefail

# ============================================================
# BFS-focused CCWS stress sweep
#
# This sweep intentionally increases L1D pressure and warp interference
# to make CCWS effects easier to see on BFS.
#
# Usage:
#   chmod +x benchmark_ccws_bfs_stress.sh
#   ./benchmark_ccws_bfs_stress.sh
#
# Optional overrides:
#   WARPS_LIST="16 32" ./benchmark_ccws_bfs_stress.sh
#   CACHE_NAMES="c8k_w2" CACHE_SIZES="8192" CACHE_WAYS="2" ./benchmark_ccws_bfs_stress.sh
#   BFS_ARGS="..." ./benchmark_ccws_bfs_stress.sh
# ============================================================

BENCH="${BENCH:-bfs}"
CORES="${CORES:-1}"
THREADS="${THREADS:-16}"
PERF="${PERF:-3}"
DRIVER="${DRIVER:-simx}"

read -r -a WARPS_ARR <<< "${WARPS_LIST:-16 32}"
read -r -a CACHE_NAMES_ARR <<< "${CACHE_NAMES:-c8k_w2 c16k_w4}"
read -r -a CACHE_SIZES_ARR <<< "${CACHE_SIZES:-8192 16384}"
read -r -a CACHE_WAYS_ARR  <<< "${CACHE_WAYS:-2 4}"

if [[ ${#CACHE_NAMES_ARR[@]} -ne ${#CACHE_SIZES_ARR[@]} ||
      ${#CACHE_NAMES_ARR[@]} -ne ${#CACHE_WAYS_ARR[@]} ]]; then
  echo "CACHE_NAMES, CACHE_SIZES, and CACHE_WAYS must have the same number of entries."
  exit 1
fi

read -r -a CAPS_ARR <<< "${CCWS_CAPS:-2 4 8}"
read -r -a K_ARR    <<< "${CCWS_KS:-8 16}"
read -r -a VTA_ARR  <<< "${CCWS_VTAS:-16 32}"

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
OUT_DIR="ccws_bfs_stress_${TIMESTAMP}"
mkdir -p "${OUT_DIR}"

SUMMARY="${OUT_DIR}/summary.csv"
echo "benchmark,policy,config_name,cache_name,cores,warps,threads,status,logfile" > "${SUMMARY}"

run_one() {
  local policy="$1"
  local config_name="$2"
  local cache_name="$3"
  local cache_configs="$4"
  local warps="$5"
  local extra_configs="$6"

  local log_dir="${OUT_DIR}/${BENCH}/${cache_name}/w${warps}"
  mkdir -p "${log_dir}"

  local log_file="${log_dir}/${BENCH}_${policy}_${config_name}_${cache_name}_c${CORES}w${warps}t${THREADS}.log"
  local configs="${cache_configs} ${extra_configs}"

  echo "============================================================"
  echo "[RUN] benchmark=${BENCH}"
  echo "[RUN] policy=${policy}"
  echo "[RUN] config=${config_name}"
  echo "[RUN] cache=${cache_name}"
  echo "[RUN] warps=${warps}, threads=${THREADS}"
  echo "[RUN] configs=${configs}"
  echo "[RUN] log=${log_file}"
  echo "============================================================"

  if [[ -n "${BFS_ARGS:-}" ]]; then
    env -u DEBUG CONFIGS="${configs}" \
      ./ci/blackbox.sh \
        --driver="${DRIVER}" \
        --app="${BENCH}" \
        --cores="${CORES}" \
        --warps="${warps}" \
        --threads="${THREADS}" \
        --perf="${PERF}" \
        --args="${BFS_ARGS}" \
        2>&1 | tee "${log_file}"
  else
    env -u DEBUG CONFIGS="${configs}" \
      ./ci/blackbox.sh \
        --driver="${DRIVER}" \
        --app="${BENCH}" \
        --cores="${CORES}" \
        --warps="${warps}" \
        --threads="${THREADS}" \
        --perf="${PERF}" \
        2>&1 | tee "${log_file}"
  fi

  local exit_code=${PIPESTATUS[0]}

  if [[ ${exit_code} -eq 0 ]]; then
    echo "${BENCH},${policy},${config_name},${cache_name},${CORES},${warps},${THREADS},PASS,${log_file}" >> "${SUMMARY}"
  else
    echo "${BENCH},${policy},${config_name},${cache_name},${CORES},${warps},${THREADS},FAIL,${log_file}" >> "${SUMMARY}"
  fi

  echo
}

run_cache_warps_point() {
  local cache_name="$1"
  local cache_configs="$2"
  local warps="$3"

  run_one \
    "GTO" \
    "baseline_gto" \
    "${cache_name}" \
    "${cache_configs}" \
    "${warps}" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::GTO"

  for cap in "${CAPS_ARR[@]}"; do
    for k in "${K_ARR[@]}"; do
      for vta in "${VTA_ARR[@]}"; do
        run_one \
          "CCWS" \
          "dyn_k${k}_vta${vta}_cap${cap}" \
          "${cache_name}" \
          "${cache_configs}" \
          "${warps}" \
          "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
           -DVX_CCWS_DYNAMIC_LLDS=1 \
           -DVX_CCWS_BASE_LLS=100 \
           -DVX_CCWS_K_THROTTLE=${k} \
           -DVX_CCWS_VTA_SIZE=${vta} \
           -DVX_CCWS_LLS_CUTOFF=0 \
           -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=${cap} \
           -DVX_CCWS_LLS_DECAY_PERIOD=4 \
           -DVX_CCWS_LLS_DECAY_STEP=1"
      done
    done
  done
}

echo "============================================================"
echo "Starting BFS CCWS stress sweep"
echo "Output directory: ${OUT_DIR}"
echo "Benchmark=${BENCH}, Driver=${DRIVER}, Cores=${CORES}, Threads=${THREADS}, Perf=${PERF}"
echo "Warps: ${WARPS_ARR[*]}"
echo "Cache names: ${CACHE_NAMES_ARR[*]}"
echo "Caps: ${CAPS_ARR[*]}"
echo "K_THROTTLE: ${K_ARR[*]}"
echo "VTA sizes: ${VTA_ARR[*]}"
echo "============================================================"
echo

for cache_idx in "${!CACHE_NAMES_ARR[@]}"; do
  cache_name="${CACHE_NAMES_ARR[cache_idx]}"
  cache_configs="-DDCACHE_SIZE=${CACHE_SIZES_ARR[cache_idx]} -DDCACHE_NUM_WAYS=${CACHE_WAYS_ARR[cache_idx]}"

  for warps in "${WARPS_ARR[@]}"; do
    run_cache_warps_point "${cache_name}" "${cache_configs}" "${warps}"
  done
done

echo "============================================================"
echo "BFS stress sweep complete"
echo "Summary: ${SUMMARY}"
echo "Logs: ${OUT_DIR}"
echo "============================================================"

if [[ -f "plot_sweep_results.py" ]]; then
  python3 plot_sweep_results.py "${OUT_DIR}" --normalize-to GTO
fi
