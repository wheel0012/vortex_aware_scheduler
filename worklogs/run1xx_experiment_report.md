# run100-run118 Trace Experiment Report

## Scope

This report reconstructs the `worklogs/trace_runs/run1xx` experiments from their saved `SUMMARY.md` files and analysis outputs.

The meaningful scheduler experiments are:

- `run100-run103`: BFS input-size sweep.
- `run104-run109`: hotspot input/cache-size sweep.
- `run110-run111`: SSSP trace integration check.

The later runs are instrumentation smoke tests while adding userpc dcache miss counters:

- `run112-run118`: build/runtime validation and counter smoke tests. These should not be used as scheduler-comparison data.

Primary performance metric below is userpc issue-window span cycles. Speedup is `RR span / policy span`, so larger is better.

## Common Setup

For `run100-run111`:

- Shape: `CORES=1`, `WARPS=16`, `THREADS=16`.
- L2 enabled.
- Perf class: `2`.
- Memory latency: ramulator default.
- Cache latency: `2`.
- Policies: `RR`, `GTO`, `gCAWS` for runs with policy comparison.
- `SCHED_POLICY_MATCH_ARBITER=1`.
- Analyzer: `--no-plots --lite`.
- Trace filtering: `TRACE_USERPC_ONLY=1` for `run100-run109`; `run111` used full trace with explicit userpc window.

## BFS Sweep: run100-run103

Configuration:

- Benchmark: `bfs`.
- Dcache size: `32768`.
- Userpc window: `0x80000094..0x80000274`.
- Inputs: generated BFS graphs from 64 to 512 nodes.

| Run | Input | RR span | GTO span | gCAWS span | GTO speedup | gCAWS speedup | Best |
|---|---|---:|---:|---:|---:|---:|---|
| run100 | graph64 | 35789 | 16831 | 16124 | 2.13x | 2.22x | gCAWS |
| run101 | graph128 | 45967 | 28294 | 25104 | 1.62x | 1.83x | gCAWS |
| run102 | graph256 | 68026 | 48442 | 43771 | 1.40x | 1.55x | gCAWS |
| run103 | graph512 | 76181 | 56057 | 62426 | 1.36x | 1.22x | GTO |

Observation:

BFS clearly exposes scheduler differences in this PC window. gCAWS is best for graph64-256, while GTO overtakes it at graph512. As input grows, the advantage over RR shrinks, which matches the saturation hypothesis: the window becomes increasingly dominated by dependency/memory readiness rather than pure arbitration choice.

Selected stall signals:

- `run101-run103` show very low ready-hit ratios for RR/GTO and high LSU scoreboard share, often around 96-99%.
- gCAWS reduces span strongly for graph128/256, likely by avoiding some of the worst ready-set starvation patterns, but at graph512 the benefit weakens and GTO wins.

## Hotspot Sweep: run104-run109

Configuration:

- Benchmark: `hotspot`.
- Userpc window: `0x80000348..0x80000900`.
- Inputs: sizes `64` and `128`, `iters=1`, `sim_time=2`.
- Dcache sizes: `4096`, `16384`, `65536`.

| Run | Size | Dcache | RR span | GTO span | gCAWS span | GTO speedup | gCAWS speedup | Best |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| run104 | 64 | 4096 | 167749 | 171696 | 171351 | 0.98x | 0.98x | RR |
| run105 | 128 | 4096 | 413182 | 418810 | 403394 | 0.99x | 1.02x | gCAWS |
| run106 | 64 | 16384 | 153035 | 162473 | 158157 | 0.94x | 0.97x | RR |
| run107 | 128 | 16384 | 366947 | 392763 | 365649 | 0.93x | 1.00x | gCAWS |
| run108 | 64 | 65536 | 153636 | 160886 | 157551 | 0.95x | 0.98x | RR |
| run109 | 128 | 65536 | 367785 | 384678 | 364653 | 0.96x | 1.01x | gCAWS |

Observation:

Hotspot does not show a strong positive scheduler effect. For size64, RR is consistently best. For size128, gCAWS is only marginally better than RR, roughly 0-2%. GTO is consistently worse.

This supports the earlier intuition: hotspot has more regular, streaming-like reuse/eviction behavior in this window, so sophisticated warp selection has little room to help. The dominant behavior is not a scheduler-friendly irregular dependency pattern. Cache size changes improve absolute span from 4096 to 16384, but going to 65536 gives little additional benefit, suggesting the chosen working set pressure is already mostly relieved by 16 KiB or bottlenecked elsewhere.

## SSSP Check: run110-run111

Configuration:

- Benchmark: `sssp`.
- Input: `tests/opencl/sssp/tiny.coo`, source `0`.
- Goal: add SSSP to trace bench and find a usable userpc window.

| Run | PC mode | Trace status | Rows | Issued | Span | UserPC IPC | Notes |
|---|---|---|---:|---:|---:|---:|---|
| run110 | `USER_PC_AUTO=1` | missing trace | 0 | ? | ? | ? | Auto window did not produce usable trace output. |
| run111 | explicit `0x460..0x960` | ok | 873496 | 13696 | 58913 | 0.232478 | Explicit PC window worked. |

Observation:

SSSP was successfully added to the trace script, but auto PC inference was not sufficient for this benchmark. Explicit PC windowing is required for now.

## Instrumentation Smoke Runs: run112-run118

These runs were produced while adding userpc dcache miss counting. Treat them as validation artifacts, not experiment data.

| Run | Purpose | Result |
|---|---|---|
| run112 | SSSP tiny with small `WARPS=4`, sandbox build path | build failed due ccache read-only sandbox behavior. |
| run113 | SSSP tiny after escalated build | runtime failed with OpenCL `vector_init` error `-54`. |
| run114 | BFS graph32 during first miss-counter patch | trace existed, but run returned failure due an intermediate destructor-order segfault. |
| run115 | BFS graph32 after fixing segfault | ok; new summary columns appeared, `D$ read/write miss = 0/0`. |
| run116 | BFS graph32 repeat after moving aggregate miss print to Socket | ok; `D$ read/write miss = 0/0`. |
| run117 | BFS graph32 with full PC window | ok; `D$ read/write miss = 0/0`, and userpc dcache requests were still 0. |
| run118 | Tiny kmeans miss-counter smoke attempt | missing trace; workload failed to launch kernels with OpenCL `-54`. |

Important caveat:

The dcache miss counters were added after `run100-run111`, so older runs cannot be used to analyze userpc dcache miss counts. Their `perf2.dcache_reads` values are mostly unavailable or zero in the old logs. Use new runs after the miss-counter patch for cache-miss analysis.

## Conclusions

1. BFS is the best current benchmark for exposing scheduler differences.

   The graph64-512 sweep shows clear RR vs GTO/gCAWS separation. gCAWS is strongest at smaller graph sizes, while GTO becomes best at graph512.

2. The scheduler effect decreases as BFS input grows.

   Speedup over RR falls from about `2.22x` at graph64 to `1.22-1.36x` at graph512. The stall metrics point toward LSU scoreboard pressure dominating larger inputs.

3. Hotspot is not a good positive-case benchmark for these schedulers.

   RR is best for size64, and gCAWS only barely helps for size128. Cache-size sweep changes absolute time but not the scheduler story.

4. SSSP is wired into the trace script, but needs explicit PC windows.

   `run111` is the useful SSSP baseline. `USER_PC_AUTO` did not work in `run110`.

5. For future cache-miss analysis, rerun the meaningful sweeps with the new miss counters.

   The right next experiment is to repeat BFS graph64-512 and hotspot size64/128 after the miss-counter patch, then compare span, scoreboard LSU share, and `D$ read/write miss` side by side.
