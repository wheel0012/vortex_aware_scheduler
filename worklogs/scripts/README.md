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
analysis/userpc_summary.csv
analysis/userpc_issue_trace.csv
analysis/cycle_issue_trace.csv
analysis/wid_timeline.png
analysis/pc_timeline.png
analysis/score_timeline.png
device_elf/*.elf
device_elf/*.dump
```

The run root also contains `SUMMARY.md`.

### Defaults

```text
CORES=1
WARPS=64
THREADS=1
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

To collect both perf counter classes for the same trace shape, use `PERFS`.
Each perf class is written under a separate `perf<N>` directory:

```bash
PERFS="1 2" \
POLICIES="RR GTO" \
BENCHES=sgemm3 \
WARPS=16 \
THREADS=1 \
SGEMM_N=4 \
SGEMM_TILE=2 \
LSU_BLOCKS=1 \
DCACHE_BANKS=4 \
USER_FROM_EVENT=WSPAWN:3 \
USER_TO_EVENT=WSPAWN:4 \
USER_PC_FROM=0x1c4 \
USER_PC_TO=0x248 \
TIMEOUT_SEC=900 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

For example, GTO SGEMM outputs land in
`worklogs/trace_runs/run<N>/GTO/perf1/sgemm3/` and
`worklogs/trace_runs/run<N>/GTO/perf2/sgemm3/`. Use `PERF=1` when only one
perf class is needed; the legacy `<policy>/<bench>/` directory layout is kept
for a single perf class.

### Tiny BFS Trace

Use `graph32.txt` for a very small BFS trace, then move to `graph4k.txt` when
you want a wider frontier:

```bash
BFS_GRAPH=tests/opencl/bfs/graph32.txt \
POLICIES="RR GTO gCAWS" \
BENCHES=bfs \
WARPS=32 \
THREADS=1 \
LSU_BLOCKS=1 \
DCACHE_BANKS=4 \
TIMEOUT_SEC=300 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

`BFS_GRAPH` accepts an absolute path or a path relative to the repository root.
For `graph32.txt`, BFS uses local work size 32, so `WARPS * THREADS` must be at
least 32. Larger BFS inputs use local work size 256 by default.

To check whether the scheduler gap is mostly LSU/cache-bank pressure, rerun
the same trace with the 4-bank hardware tweak used by the kmeans experiments:

```bash
LSU_BLOCKS=1 \
DCACHE_BANKS=4 \
USER_FROM_EVENT=WSPAWN:3 \
USER_TO_EVENT=WSPAWN:4 \
USER_PC_FROM=0x1c4 \
USER_PC_TO=0x248 \
POLICIES="RR GTO gCAWS" \
BENCHES=sgemm3 \
WARPS=32 \
THREADS=1 \
SGEMM_N=4 \
SGEMM_TILE=2 \
TIMEOUT_SEC=900 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

This adds `-DNUM_LSU_BLOCKS=1 -DDCACHE_NUM_BANKS=4` to `CONFIGS`; the trace
summary records both values and the effective base flags.
`NUM_LSU_BLOCKS` must not exceed the default `ISSUE_WIDTH=ceil(WARPS/16)`, so
use at least `WARPS=32` for `LSU_BLOCKS=2` unless you also override
`ISSUE_WIDTH` explicitly.

### Kernel-Body Focus

To focus analysis on the SGEMM worker region instead of startup/runtime code,
pass analyzer filters through `ANALYZE_ARGS`.

Example:

```bash
USER_FROM_EVENT=WSPAWN:3 \
USER_TO_EVENT=WSPAWN:4 \
USER_PC_FROM=0x1c4 \
USER_PC_TO=0x248 \
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

`USER_PC_FROM` and `USER_PC_TO` are analyzer-side filters. Values below
`USER_PC_BASE` are treated as offsets from `USER_PC_BASE`; the default base is
`0x80000000`, so `0x1c4` means `0x800001c4`. This does not change simulator
scheduling. It only makes the analysis outputs monitor the selected user-code
region. The analyzer writes `userpc_summary.csv`, `userpc_issue_trace.csv`, and
`cycle_issue_trace.csv` in the analysis directory when available.

To infer the PC filter from the generated Vortex device ELF instead of typing
the offsets manually, enable `USER_PC_AUTO`:

```bash
USER_PC_AUTO=1 \
BFS_GRAPH=tests/opencl/bfs/graph32.txt \
POLICIES="RR GTO gCAWS" \
BENCHES=bfs \
WARPS=4 \
THREADS=8 \
TIMEOUT_SEC=300 \
./worklogs/scripts/trace_small_aware_schedulers.sh
```

By default this looks for `BFS_1 BFS_2` on BFS and `sgemm3` on SGEMM. Override
with `USER_PC_SYMBOLS="symbol_a symbol_b"` when a benchmark uses different
kernel names. The selected range is recorded in
`analysis/userpc_auto_window.txt`.

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
BFS_GRAPH=tests/opencl/bfs/graph32.txt
SGEMM_N=4
SGEMM_TILE=2
MEM_LATENCY=4
CACHE_LATENCY=1
ALL_LATENCY=1
PIPELINE_LATENCY=1
EXEC_LATENCY=1
ALU_LATENCY=1
FMA_LATENCY=1
LSU_BLOCKS=2
DCACHE_BANKS=4
USER_PC_BASE=0x80000000
USER_PC_FROM=0x1c4
USER_PC_TO=0x248
USER_PC_AUTO=1
USER_PC_SYMBOLS="BFS_1 BFS_2"
USER_FROM_EVENT=WSPAWN:3
USER_TO_EVENT=WSPAWN:4
PERF=1
PERFS="1 2"
INTERACTIVE_PLOTS=1
EXTRA_CONFIGS="-D..."
ANALYZE_ARGS="..."
TIMEOUT_SEC=300
```

`MEM_LATENCY` enables the simx fixed-memory-latency path. If it is unset, the
run uses the normal memory simulator path.

`CACHE_LATENCY` controls the simx cache port latency used by the configured
cache hierarchy. If it is unset, the default is `2`.

`ALL_LATENCY` is a convenience knob for latency-shape experiments. It fills in
`MEM_LATENCY`, `CACHE_LATENCY`, `PIPELINE_LATENCY`, and `EXEC_LATENCY` unless a
more specific knob is already set. For example, `ALL_LATENCY=1` makes the small
trace run use one-cycle memory/cache, simx pipeline, ALU/FPU/LSU/SFU defaults.

Pipeline knobs:

```text
ICACHE_REQ_LATENCY
OPERANDS_LATENCY
DISPATCH_LATENCY
PIPELINE_LATENCY
```

Execution-unit knobs:

```text
ALU_LATENCY
BRANCH_LATENCY
VOTE_LATENCY
SHFL_LATENCY
IMUL_LATENCY
IDIV_LATENCY
FPU_BASE_LATENCY
FPU_SIMPLE_LATENCY
FMA_LATENCY
FDIV_LATENCY
FSQRT_LATENCY
FCVT_LATENCY
LSU_LOAD_RSP_LATENCY
LSU_STORE_LATENCY
LSU_FENCE_LATENCY
SFU_BASE_LATENCY
SFU_OP_LATENCY
EXEC_LATENCY
```

Specific knobs override the broad ones, so this keeps all execution latency at
one cycle except FMA:

```bash
EXEC_LATENCY=1 FMA_LATENCY=4 BENCHES=sgemm3 ./worklogs/scripts/trace_small_aware_schedulers.sh
```

`INTERACTIVE_PLOTS=1` opens an interactive matplotlib WID timeline after the run
finishes. The script opens one window per policy/workload pair and only uses one
representative trace when both `PERFS="1 2"` are enabled. Close each plot window
to advance to the next one.

When `USER_PC_FROM` or `USER_PC_TO` is set, simx also prints CSR-free
`PERF: userpc ...` and `[USERPC_PERF ...]` blocks to `run.log` for that PC
window. The `PERF: userpc ...` lines mirror the normal perf summary with
scheduler idle/stall, ibuffer stall, scoreboard stall, ready-hit ratio,
ifetch/load/store counts, average ifetch/load latency, and IPC. The
`[USERPC_PERF ...]` lines keep the detailed scheduler diagnostics, including
not-ready fallback count, FU issue mix, scoreboard blocker mix, and per-warp
first/last issue cycles. The script forwards `USER_PC_BASE`, `USER_PC_FROM`,
and `USER_PC_TO` to simx as `VX_USER_PC_*` environment variables.

## `sweep_aware_schedulers.sh`

Runs broader aware-scheduler sweeps. Use this for comparison-style experiments
after the trace behavior has been checked with the small trace script.
