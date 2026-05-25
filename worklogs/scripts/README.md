# Worklog Scripts

This directory contains helper scripts for local scheduler experiments. The
generated run directories are ignored by git under `worklogs/trace_runs/`.

## `trace_small_aware_schedulers.sh`

Runs small simx workloads with warp-scheduler issue tracing enabled. This is
for scheduler behavior validation, not full benchmark IPC evaluation.

The script builds a small simx/runtime configuration, runs each requested
policy/workload, writes `issue_trace.csv`, and invokes:

```bash
sim/simx/analyze_warp_sched_trace.py
```

Artifacts are written to:

```text
worklogs/trace_runs/run<N>/<policy>/<bench>/
```

Each benchmark directory contains:

```text
issue_trace.csv
run.log
analyze.log
analysis/summary.txt
analysis/mismatch_report.csv
analysis/wid_timeline.png
analysis/pc_timeline.png
analysis/score_timeline.png
```

The run root also contains `SUMMARY.md`.

### Defaults

```text
CORES=1
WARPS=8
THREADS=8
POLICIES="RR GTO gCAWS"
BENCHES="bfs sgemm3"
SGEMM_N=24
SGEMM_TILE=8
TIMEOUT_SEC=900
```

`THREADS` is the Vortex warp size. The total hardware lanes per core are
`WARPS * THREADS`.

### Tiny SGEMM Trace

Useful when the full trace is too large:

```bash
POLICIES=gCAWS \
BENCHES=sgemm3 \
WARPS=8 \
THREADS=2 \
SGEMM_N=4 \
SGEMM_TILE=2 \
MEM_LATENCY=4 \
CACHE_LATENCY=1 \
TIMEOUT_SEC=300 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

For a 2x2 tile, make sure `WARPS * THREADS >= 4`.

### Kernel-Body Focus

To focus analysis on the SGEMM worker region instead of startup/runtime code,
pass analyzer filters through `ANALYZE_ARGS`.

Example:

```bash
ANALYZE_ARGS="--from-event WSPAWN:3 --to-event WSPAWN:4 --pc-from 0x1c4 --pc-to 0x248" \
POLICIES=gCAWS \
BENCHES=sgemm3 \
WARPS=8 \
THREADS=2 \
SGEMM_N=4 \
SGEMM_TILE=2 \
MEM_LATENCY=4 \
CACHE_LATENCY=1 \
TIMEOUT_SEC=300 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

`WSPAWN` is a warp-spawn event inside the runtime/kernel execution path, not a
unique global kernel-launch marker. For SGEMM traces seen so far, `WSPAWN:3`
has typically been the useful worker-body spawn, but confirm with
`analysis/event_markers_all.csv` for each run.

### Split By WSPAWN

```bash
ANALYZE_ARGS="--split-by-wspawn" \
POLICIES=gCAWS \
BENCHES=sgemm3 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

This writes per-spawn analysis directories under `analysis/wspawn_*` and an
index at `analysis/wspawn_splits.csv`.

### Useful Knobs

```text
POLICIES="RR GTO gCAWS"
BENCHES="bfs sgemm3"
WARPS=8
THREADS=2
SGEMM_N=4
SGEMM_TILE=2
MEM_LATENCY=4
CACHE_LATENCY=1
PERF=1
EXTRA_CONFIGS="-D..."
ANALYZE_ARGS="..."
TIMEOUT_SEC=300
```

`MEM_LATENCY` enables the simx fixed-memory-latency path. If it is unset, the
run uses the normal memory simulator path.

`CACHE_LATENCY` controls the simx cache port latency used by the configured
cache hierarchy. If it is unset, the default is `2`.

## `sweep_aware_schedulers.sh`

Runs broader aware-scheduler sweeps. Use this for comparison-style experiments
after the trace behavior has been checked with the small trace script.
