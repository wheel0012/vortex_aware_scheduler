#!/usr/bin/env bash

set -u
set -o pipefail

# ============================================================
# Vortex CCWS grouped sweep script
#
# Usage:
#   chmod +x benchmark_ccws_groups.sh
#   ./benchmark_ccws_groups.sh
#
# Optional overrides:
#   GROUP=HCS ./benchmark_ccws_groups.sh
#   GROUP=CI ./benchmark_ccws_groups.sh
#   WARPS=32 THREADS=16 ./benchmark_ccws_groups.sh
#   HCS_BENCHMARKS="bfs kmeans" ./benchmark_ccws_groups.sh
#   CI_BENCHMARKS="sgemm hotspot" ./benchmark_ccws_groups.sh
# ============================================================

COMMON_CONFIGS="${COMMON_CONFIGS:--DDCACHE_SIZE=32768 -DDCACHE_NUM_WAYS=8}"

CORES="${CORES:-1}"
WARPS="${WARPS:-16}"
THREADS="${THREADS:-16}"
PERF="${PERF:-3}"
DRIVER="${DRIVER:-simx}"
GROUP="${GROUP:-ALL}"

read -r -a HCS_LIST <<< "${HCS_BENCHMARKS:-bfs kmeans spmv}"
read -r -a CI_LIST  <<< "${CI_BENCHMARKS:-sgemm hotspot blackscholes vecadd}"

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
OUT_DIR="ccws_group_sweep_${TIMESTAMP}"
mkdir -p "${OUT_DIR}"

SUMMARY="${OUT_DIR}/summary.csv"
echo "group,benchmark,policy,config_name,cores,warps,threads,status,logfile" > "${SUMMARY}"

bench_exists() {
  local bench="$1"
  [[ -d "tests/opencl/${bench}" || -d "tests/regression/${bench}" || -d "tests/${bench}" ]]
}

run_one() {
  local group="$1"
  local bench="$2"
  local policy="$3"
  local config_name="$4"
  local extra_configs="$5"

  if ! bench_exists "${bench}"; then
    echo "[SKIP] ${group}/${bench}: benchmark directory not found"
    echo "${group},${bench},${policy},${config_name},${CORES},${WARPS},${THREADS},SKIP," >> "${SUMMARY}"
    return 0
  fi

  local log_dir="${OUT_DIR}/${group}/${bench}"
  mkdir -p "${log_dir}"

  local log_file="${log_dir}/${bench}_${policy}_${config_name}_c${CORES}w${WARPS}t${THREADS}.log"
  local configs="${COMMON_CONFIGS} ${extra_configs}"

  echo "============================================================"
  echo "[RUN] group=${group}"
  echo "[RUN] benchmark=${bench}"
  echo "[RUN] policy=${policy}"
  echo "[RUN] config=${config_name}"
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
      2>&1 | tee "${log_file}"

  local exit_code=${PIPESTATUS[0]}

  if [[ ${exit_code} -eq 0 ]]; then
    echo "${group},${bench},${policy},${config_name},${CORES},${WARPS},${THREADS},PASS,${log_file}" >> "${SUMMARY}"
  else
    echo "${group},${bench},${policy},${config_name},${CORES},${WARPS},${THREADS},FAIL,${log_file}" >> "${SUMMARY}"
  fi

  echo
}

run_baselines_for_bench() {
  local group="$1"
  local bench="$2"

  run_one \
    "${group}" \
    "${bench}" \
    "GTO" \
    "baseline_gto" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::GTO"
}

run_ccws_for_bench() {
  local group="$1"
  local bench="$2"

  run_one \
    "${group}" \
    "${bench}" \
    "CCWS" \
    "P0_dyn_base100_k8_vta16_nocap" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_DYNAMIC_LLDS=1 \
     -DVX_CCWS_BASE_LLS=100 \
     -DVX_CCWS_K_THROTTLE=8 \
     -DVX_CCWS_VTA_SIZE=16 \
     -DVX_CCWS_LLS_CUTOFF=0 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=0 \
     -DVX_CCWS_LLS_DECAY_PERIOD=1 \
     -DVX_CCWS_LLS_DECAY_STEP=1"

  run_one \
    "${group}" \
    "${bench}" \
    "CCWS" \
    "P1_dyn_base100_k8_vta16_cap8" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_DYNAMIC_LLDS=1 \
     -DVX_CCWS_BASE_LLS=100 \
     -DVX_CCWS_K_THROTTLE=8 \
     -DVX_CCWS_VTA_SIZE=16 \
     -DVX_CCWS_LLS_CUTOFF=0 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=8 \
     -DVX_CCWS_LLS_DECAY_PERIOD=1 \
     -DVX_CCWS_LLS_DECAY_STEP=1"

  run_one \
    "${group}" \
    "${bench}" \
    "CCWS" \
    "P2_dyn_base100_k8_vta16_cap4" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_DYNAMIC_LLDS=1 \
     -DVX_CCWS_BASE_LLS=100 \
     -DVX_CCWS_K_THROTTLE=8 \
     -DVX_CCWS_VTA_SIZE=16 \
     -DVX_CCWS_LLS_CUTOFF=0 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=4 \
     -DVX_CCWS_LLS_DECAY_PERIOD=1 \
     -DVX_CCWS_LLS_DECAY_STEP=1"
}

run_group() {
  local group="$1"
  shift
  local benches=("$@")

  echo "############################################################"
  echo "# Group: ${group}"
  echo "# Benchmarks: ${benches[*]}"
  echo "############################################################"

  for bench in "${benches[@]}"; do
    run_baselines_for_bench "${group}" "${bench}"
    run_ccws_for_bench "${group}" "${bench}"
  done
}

echo "============================================================"
echo "Starting Vortex CCWS grouped sweep"
echo "Output directory: ${OUT_DIR}"
echo "Group=${GROUP}, Driver=${DRIVER}, Cores=${CORES}, Warps=${WARPS}, Threads=${THREADS}, Perf=${PERF}"
echo "Common configs: ${COMMON_CONFIGS}"
echo "============================================================"
echo

case "${GROUP}" in
  HCS)
    run_group "HCS" "${HCS_LIST[@]}"
    ;;
  CI)
    run_group "CI" "${CI_LIST[@]}"
    ;;
  ALL)
    run_group "HCS" "${HCS_LIST[@]}"
    run_group "CI" "${CI_LIST[@]}"
    ;;
  *)
    echo "Invalid GROUP=${GROUP}. Use HCS, CI, or ALL."
    exit 1
    ;;
esac

echo "============================================================"
echo "Grouped sweep complete"
echo "Summary: ${SUMMARY}"
echo "Logs: ${OUT_DIR}"
echo "============================================================"

if [[ -f "plot_sweep_results.py" ]]; then
  python3 plot_sweep_results.py "${OUT_DIR}" --normalize-to GTO
fi
