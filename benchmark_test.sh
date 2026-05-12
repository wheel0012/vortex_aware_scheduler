#!/usr/bin/env bash

set -u
set -o pipefail

# ============================================================
# Vortex CCWS sweep script
# Usage:
#   chmod +x run_ccws_sweep.sh
#   ./run_ccws_sweep.sh
# ============================================================

# ------------------------------------------------------------
# Common hardware / cache configuration
# ------------------------------------------------------------
#COMMON_CONFIGS="-DDCACHE_SIZE=131072 -DDCACHE_NUM_WAYS=8 -DL3_CACHE_SIZE=262144 -DL3_NUM_WAYS=16"
COMMON_CONFIGS=""

CORES=1
WARPS=16
THREADS=16
PERF=3
DRIVER="simx"

# ------------------------------------------------------------
# Benchmarks
# 1st sweep set
# ------------------------------------------------------------
BENCHMARKS=(
  bfs
  kmeans
  sgemm
  spmv
  hotspot
  gaussian
)

# ------------------------------------------------------------
# Output directory
# ------------------------------------------------------------
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
OUT_DIR="sweep_results_${TIMESTAMP}"
mkdir -p "${OUT_DIR}"

SUMMARY="${OUT_DIR}/summary.csv"

echo "benchmark,policy,config_name,cores,warps,threads,status,logfile" > "${SUMMARY}"

# ------------------------------------------------------------
# Helper function
# ------------------------------------------------------------
run_one() {
  local bench="$1"
  local policy="$2"
  local config_name="$3"
  local extra_configs="$4"

  local log_dir="${OUT_DIR}/${bench}"
  mkdir -p "${log_dir}"

  local log_file="${log_dir}/${bench}_${policy}_${config_name}_c${CORES}w${WARPS}t${THREADS}.log"

  echo "============================================================"
  echo "[RUN] benchmark=${bench}"
  echo "[RUN] policy=${policy}"
  echo "[RUN] config=${config_name}"
  echo "[RUN] log=${log_file}"
  echo "============================================================"

  local configs="${COMMON_CONFIGS} ${extra_configs}"

  CONFIGS="${configs}" \
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
    echo "${bench},${policy},${config_name},${CORES},${WARPS},${THREADS},PASS,${log_file}" >> "${SUMMARY}"
  else
    echo "${bench},${policy},${config_name},${CORES},${WARPS},${THREADS},FAIL,${log_file}" >> "${SUMMARY}"
  fi

  echo
}

# ============================================================
# Baseline policies
# ============================================================

run_baselines_for_bench() {
  local bench="$1"

  # Static / Fixed
  run_one \
    "${bench}" \
    "STATIC" \
    "baseline_static" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::Static"

  # Round Robin
  run_one \
    "${bench}" \
    "RR" \
    "baseline_rr" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::RR"

  # GTO
  run_one \
    "${bench}" \
    "GTO" \
    "baseline_gto" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::GTO"
}

# ============================================================
# CCWS configs
# ============================================================

run_ccws_for_bench() {
  local bench="$1"

  # C0: default / weak
  run_one \
    "${bench}" \
    "CCWS" \
    "C0_vta8_lld10_p1_k4" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_VTA_SIZE=8 \
     -DVX_CCWS_LLD_SCORE=10 \
     -DVX_CCWS_LLS_DECAY_PERIOD=1 \
     -DVX_CCWS_LLS_DECAY_STEP=1 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=4 \
     -DVX_CCWS_LLS_CUTOFF=0"

  # C1: medium
  run_one \
    "${bench}" \
    "CCWS" \
    "C1_vta16_lld64_p8_k4" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_VTA_SIZE=16 \
     -DVX_CCWS_LLD_SCORE=64 \
     -DVX_CCWS_LLS_DECAY_PERIOD=8 \
     -DVX_CCWS_LLS_DECAY_STEP=1 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=4 \
     -DVX_CCWS_LLS_CUTOFF=0"

  # C2: aggressive
  run_one \
    "${bench}" \
    "CCWS" \
    "C2_vta16_lld128_p16_k2" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_VTA_SIZE=16 \
     -DVX_CCWS_LLD_SCORE=128 \
     -DVX_CCWS_LLS_DECAY_PERIOD=16 \
     -DVX_CCWS_LLS_DECAY_STEP=1 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=2 \
     -DVX_CCWS_LLS_CUTOFF=0"

  # C3: very aggressive
  run_one \
    "${bench}" \
    "CCWS" \
    "C3_vta32_lld128_p16_k1" \
    "-DVX_SCHED_POLICY=WarpSchedulePolicy::CCWS \
     -DVX_CCWS_VTA_SIZE=32 \
     -DVX_CCWS_LLD_SCORE=128 \
     -DVX_CCWS_LLS_DECAY_PERIOD=16 \
     -DVX_CCWS_LLS_DECAY_STEP=1 \
     -DVX_CCWS_MAX_ACTIVE_LOAD_WARPS=1 \
     -DVX_CCWS_LLS_CUTOFF=0"
}

# ============================================================
# Main sweep
# ============================================================

echo "============================================================"
echo "Starting Vortex CCWS sweep"
echo "Output directory: ${OUT_DIR}"
echo "Cores=${CORES}, Warps=${WARPS}, Threads=${THREADS}, Perf=${PERF}"
echo "============================================================"
echo

for bench in "${BENCHMARKS[@]}"; do
  echo "############################################################"
  echo "# Benchmark: ${bench}"
  echo "############################################################"

  run_baselines_for_bench "${bench}"
  run_ccws_for_bench "${bench}"
done

echo "============================================================"
echo "Sweep complete"
echo "Summary: ${SUMMARY}"
echo "Logs: ${OUT_DIR}"
echo "============================================================"

if [[ -f "plot_sweep_results.py" ]]; then
  python3 plot_sweep_results.py "${OUT_DIR}" --normalize-to GTO
fi
