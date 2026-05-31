#!/usr/bin/env bash
# Unified-policy sweep: same arbiter at BOTH the schedule (fetch) stage and
# the issue stage.  Tests the hypothesis that paper-like gCAWS effect
# requires the policy to act at the fetch arbiter (active warp limit) in
# Vortex's split fetch/issue pipeline.
#
# Workloads: all opencl tests we can reasonably make WG=256 (8 warps/WG).
# Single-thread-WG workloads (sgemm, vecadd, saxpy, psort, conv3, sfilter)
# are skipped — they don't fill even a single warp.
#
# Launch in background, SSH-survivable:
#   nohup ./worklogs/scripts/run_unified_sweep.sh \
#     > worklogs/runs/unified_sweep/console.log 2>&1 &
#   disown

set -u
set -o pipefail

ROOT_DIR="/data/URP_26_spring/bbosseong/vortex_aware_scheduler"
BUILD_DIR="$ROOT_DIR/build"
LOG_ROOT="${LOG_ROOT:-$ROOT_DIR/worklogs/runs/unified_sweep}"
INPUT_SIZE="${INPUT_SIZE:-small}"

CORES=1
WARPS=32
THREADS=32
TIMEOUT_SEC=7200
BASE_FLAGS="-DPERF_ENABLE -DNUM_LSU_BLOCKS=2 -DDCACHE_NUM_BANKS=4 -DDCACHE_NUM_WAYS=8 ${EXTRA_FLAGS:-}"

# Same policy value applied to both -DVORTEX_ARBITER and -DVORTEX_SCHED_POLICY.
declare -A POLICY_ID=( [RR]=2 [GTO]=1 [gCAWS]=4 )

# Workloads. Each entry is "app:args" or "tag:app:args".
# kmeans/bfs/sgemm3 are the high-confidence WG=256 set; hotspot/sgemm2 need
# source edits (handled below). spmv/lbm/streamcluster use defaults; we keep
# them because irregular access pattern (spmv) or 3D stencil (lbm) provide
# divergence from the lockstep regime.
declare -A BENCH_APP=(
  [kmeans]=kmeans
  [bfs]=bfs
  [sgemm3]=sgemm3
  [hotspot]=hotspot
  [sgemm2]=sgemm2
  [spmv]=spmv
)
if [ "$INPUT_SIZE" = "big" ]; then
  declare -A BENCH_ARGS=(
    [kmeans]="-f100 -p50000"
    [bfs]="$ROOT_DIR/tests/opencl/bfs/graph128k.txt"
    [sgemm3]="-n128 -t16"
    [hotspot]="128 1 2 temp_128 power_128 output.out"
    [sgemm2]="-n128"
    [spmv]="-i $ROOT_DIR/tests/opencl/spmv/Dubcova3.mtx,$ROOT_DIR/tests/opencl/spmv/Dubcova3.vec"
  )
else
  declare -A BENCH_ARGS=(
    [kmeans]="-f100 -p2000"
    [bfs]="$ROOT_DIR/tests/opencl/bfs/graph16k.txt"
    [sgemm3]="-n32 -t16"
    [hotspot]="64 1 2 temp_64 power_64 output.out"
    [sgemm2]="-n32"
    [spmv]="-i $ROOT_DIR/tests/opencl/spmv/1138_bus.mtx,$ROOT_DIR/tests/opencl/spmv/1138_bus.vec"
  )
fi
POLICIES=(RR GTO gCAWS)
BENCHES=(kmeans bfs sgemm3 hotspot sgemm2 spmv)
PERF=2

mkdir -p "$LOG_ROOT"
SUMMARY="$LOG_ROOT/summary.tsv"
: > "$SUMMARY"
echo -e "policy\tbench\trc\tIPC\tmlat\tsb_skew\tdiff0/1\trs0%\tcycles" >> "$SUMMARY"

# Source-level WG=256 setup (idempotent sed).
sed -i -E 's|^([[:space:]]*)#define BLOCK_SIZE [0-9]+.*$|\1#define BLOCK_SIZE 16|' \
  "$ROOT_DIR/tests/opencl/hotspot/hotspot.h" 2>/dev/null || true
sed -i -E 's|^([[:space:]]*)#define TILE_SIZE [0-9]+.*$|\1#define TILE_SIZE 16|' \
  "$ROOT_DIR/tests/opencl/sgemm2/common.h" 2>/dev/null || true

# Force build cache invalidation for the touched workloads so the new
# BLOCK_SIZE/TILE_SIZE actually takes effect.
if [ -d "$BUILD_DIR/tests/opencl/hotspot" ]; then
  cp "$ROOT_DIR/tests/opencl/hotspot/hotspot.h" "$BUILD_DIR/tests/opencl/hotspot/hotspot.h" 2>/dev/null || true
fi
if [ -d "$BUILD_DIR/tests/opencl/sgemm2" ]; then
  cp "$ROOT_DIR/tests/opencl/sgemm2/common.h" "$BUILD_DIR/tests/opencl/sgemm2/common.h" 2>/dev/null || true
fi

for label in "${POLICIES[@]}"; do
  pid="${POLICY_ID[$label]}"
  # Unified: same value to both arbiter knobs.
  conf="$BASE_FLAGS -DVORTEX_ARBITER=$pid -DVORTEX_SCHED_POLICY=$pid"
  cfg_dir="$LOG_ROOT/$label"
  mkdir -p "$cfg_dir"
  : > "$cfg_dir/build.log"

  echo "===== [$label] building (VORTEX_ARBITER=$pid, VORTEX_SCHED_POLICY=$pid) ====="
  CONFIGS="$conf" make -C "$BUILD_DIR/sim/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  sim build FAIL"; tail -20 "$cfg_dir/build.log"; continue; }
  CONFIGS="$conf" make -C "$BUILD_DIR/runtime/simx" -j$(nproc) >> "$cfg_dir/build.log" 2>&1 \
    || { echo "  rt build FAIL"; continue; }

  # rebuild affected test programs (hotspot/sgemm2) so they pick up the
  # updated headers; the simx-host runtime caches kernel binaries otherwise.
  make -C "$BUILD_DIR/tests/opencl/hotspot" clean >> "$cfg_dir/build.log" 2>&1 || true
  make -C "$BUILD_DIR/tests/opencl/sgemm2"  clean >> "$cfg_dir/build.log" 2>&1 || true

  for bench in "${BENCHES[@]}"; do
    app="${BENCH_APP[$bench]}"
    args="${BENCH_ARGS[$bench]:-}"
    blog="$cfg_dir/${bench}.log"
    : > "$blog"
    extra=()
    [ -n "$args" ] && extra=(--args="$args")

    echo ">>> [$label/$bench] app=$app args=\"$args\""
    (
      cd "$BUILD_DIR" && \
      CONFIGS="$conf" timeout "$TIMEOUT_SEC" ./ci/blackbox.sh \
        --driver=simx --app="$app" \
        --cores="$CORES" --warps="$WARPS" --threads="$THREADS" \
        --l2cache --perf="$PERF" \
        "${extra[@]}" >> "$blog" 2>&1
    )
    rc=$?
    ipc=$(grep "^PERF: instrs=" "$blog" | tail -1 | grep -oE "IPC=[0-9.]+" | cut -d= -f2)
    cyc=$(grep "^PERF: instrs=" "$blog" | tail -1 | grep -oE "cycles=[0-9]+" | cut -d= -f2)
    mlat=$(grep "memory latency=" "$blog" | tail -1 | grep -oE "latency=[0-9]+" | cut -d= -f2)
    skew=$(grep "WARP_DIST STATS" "$blog" | tail -1 | grep -oE "max/mean=[0-9.]+" | cut -d= -f2)
    div0=$(grep "DIVERGE core=0 slot=0" "$blog" | tail -1 | grep -oE "diff%=[0-9.]+" | cut -d= -f2)
    div1=$(grep "DIVERGE core=0 slot=1" "$blog" | tail -1 | grep -oE "diff%=[0-9.]+" | cut -d= -f2)
    rs0=$(grep "READY_HIST core=0 slot=0" "$blog" | tail -1 | grep -oE "size0=[0-9]+\([0-9]+%\)" | grep -oE "\([0-9]+%" | tr -d '(%')
    echo "  [$label/$bench] rc=$rc IPC=${ipc:-?} mlat=${mlat:-?} sb_skew=${skew:-?} diff%=${div0:-?}/${div1:-?} rs0=${rs0:-?}% cyc=${cyc:-?}"
    echo -e "$label\t$bench\t$rc\t${ipc:-?}\t${mlat:-?}\t${skew:-?}\t${div0:-?}/${div1:-?}\t${rs0:-?}\t${cyc:-?}" >> "$SUMMARY"
  done
done

echo
echo "DONE: $LOG_ROOT"
echo "summary:"
column -t -s $'\t' "$SUMMARY"
