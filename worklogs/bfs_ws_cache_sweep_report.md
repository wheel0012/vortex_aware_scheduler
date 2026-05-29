# BFS Working-Set Cache Sweep Report

## Scope

This report summarizes the BFS working-set-aware dcache sweep with the current userpc locality counters.

Raw CSV:

- `worklogs/bfs_ws_cache_sweep.csv`

Run range:

- `run54-run88`

Configuration:

- Benchmark: `bfs`
- Inputs: `graph32`, `graph64`, `graph128`, `graph256`, `graph512`
- Policies: `RR`, `GTO`, `gCAWS`
- Shape: `CORES=1`, `WARPS=16`, `THREADS=16`
- Perf class: `2`
- User PC window: `0x80000094..0x80000274`
- Trace mode: `TRACE_USERPC_ONLY=1`
- Analyzer: `--no-plots --lite`
- Dcache sizes: `512`, `1024`, `2048`, `4096`, `8192`, `16384`, `32768` bytes
- Scheduler policy matched the issue arbiter: `SCHED_POLICY_MATCH_ARBITER=1`

## Working-Set Estimates

The BFS input file stores:

- `Node { int starting; int no_of_edges; }`: `8B * nodes`
- edge list: `4B * edges`
- three char arrays: `3B * nodes`
- cost array: `4B * nodes`
- over flag: `1B`

| Input | Nodes | Edges | Estimated bytes |
|---|---:|---:|---:|
| `graph32` | 32 | 188 | 1233 |
| `graph64` | 64 | 376 | 2465 |
| `graph128` | 128 | 754 | 4937 |
| `graph256` | 256 | 1496 | 9825 |
| `graph512` | 512 | 3026 | 19785 |

These estimates are lower than the actual address-stream footprint seen by L1 because the trace window includes repeated kernel launches, multiple buffers, runtime-visible allocation regions, and coalesced LSU traffic.

## Miss Trend

RR read-miss trend:

| Input | 512B | 1KiB | 2KiB | 4KiB | 8KiB | 16KiB | 32KiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| `graph32` | 430 | 423 | 388 | 370 | 363 | 363 | 363 |
| `graph64` | 868 | 847 | 824 | 762 | 743 | 737 | 737 |
| `graph128` | 1727 | 1718 | 1679 | 1624 | 1520 | 1506 | 1498 |
| `graph256` | 3441 | 3421 | 3428 | 3334 | 3216 | 3036 | 3002 |
| `graph512` | 3834 | 3695 | 3646 | 3506 | 3205 | 2845 | 2940 |

RR non-cold miss trend:

| Input | 512B | 1KiB | 2KiB | 4KiB | 8KiB | 16KiB | 32KiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| `graph32` | 344 | 337 | 318 | 306 | 302 | 302 | 302 |
| `graph64` | 697 | 682 | 669 | 631 | 617 | 612 | 612 |
| `graph128` | 1386 | 1379 | 1360 | 1325 | 1263 | 1254 | 1251 |
| `graph256` | 2776 | 2762 | 2777 | 2706 | 2632 | 2529 | 2506 |
| `graph512` | 3171 | 3055 | 3028 | 2902 | 2651 | 2369 | 2450 |

Observation:

- Cold misses are not the main story. Most read misses are non-cold misses.
- Cache growth reduces both read misses and non-cold misses, but the curve is gradual.
- `graph32` flattens around 8KiB.
- `graph64` flattens around 16KiB.
- `graph128` still improves slightly at 32KiB.
- `graph256` and `graph512` are still not perfectly comfortable, though `graph512` improves strongly at 16KiB.

## Policy Results

GTO wins every graph/cache point in this sweep.

| Input | Cache range | Best-policy pattern |
|---|---|---|
| `graph32` | all caches | GTO wins by about 2.8-3.0x over RR span. |
| `graph64` | all caches | GTO wins by about 2.1x over RR span. |
| `graph128` | all caches | GTO wins by about 1.6x over RR span. |
| `graph256` | all caches | GTO wins by about 1.4x over RR span. |
| `graph512` | all caches | GTO wins by about 1.3x over RR span. |

Selected points:

| Input | Cache | RR span/miss/non-cold | GTO span/miss/non-cold | gCAWS span/miss/non-cold |
|---|---:|---:|---:|---:|
| `graph32` | 1KiB | 29693 / 423 / 337 | 9944 / 403 / 318 | 28924 / 417 / 331 |
| `graph64` | 4KiB | 35493 / 762 / 631 | 16931 / 753 / 624 | 34036 / 755 / 627 |
| `graph128` | 8KiB | 45997 / 1520 / 1263 | 27990 / 1518 / 1266 | 42286 / 1514 / 1260 |
| `graph256` | 16KiB | 67316 / 3036 / 2529 | 48573 / 2979 / 2507 | 66124 / 3034 / 2526 |
| `graph512` | 32KiB | 76181 / 2940 / 2450 | 56057 / 2952 / 2489 | 73751 / 3044 / 2534 |

## Interpretation

1. BFS is the cleanest scheduler-sensitive benchmark so far.

   Unlike SGEMM3, the scheduler effect is large and stable. Unlike kmeans, the effect remains visible as input size increases through `graph512`.

2. GTO does not win by dramatically reducing L1 misses.

   At many points, policy miss counts are nearly identical. For `graph128`, 8KiB dcache has RR/GTO/gCAWS read misses of `1520/1518/1514`, but spans of `45997/27990/42286`. The span improvement is mostly latency exposure/hiding and issue ordering, not fewer L1 misses.

3. Cache capacity helps, but does not remove scheduler sensitivity.

   Increasing cache reduces non-cold misses, especially for larger graphs, but GTO remains best at every cache size. This means the scheduler-sensitive portion is not just a below-cache/above-cache threshold.

4. Larger inputs reduce the relative speedup.

   GTO is roughly 3x faster than RR at `graph32`, about 2x at `graph64`, about 1.6x at `graph128`, and about 1.3-1.4x at `graph256/512`. That matches the earlier suspicion: as the input grows, global memory pressure and saturation dilute the scheduler effect.

5. The locality profile is sparse.

   The line-local ratio is low, mostly `4-8%`, and the 64+ line-stride ratio in the CSV is high. BFS repeatedly revisits lines, but much of that reuse still misses, so non-cold misses remain high.

## Suggested Final Figure Set

For a compact result section:

- Use `graph32/64/128/256/512`.
- Plot span cycles by policy at `dcache=8KiB` or `16KiB`.
- Plot RR read miss and non-cold miss versus dcache size.
- Plot GTO speedup over RR versus graph size.

This gives a clean story: cache pressure changes gradually, but scheduler policy still changes userpc span dramatically.
