# BFS Mini L1 Miss Sweep Report

## Scope

This report summarizes the BFS mini-input L1 miss sweep from `worklogs/trace_runs/run5` through `run28`.

Raw CSV:

- `worklogs/bfs_mini_l1_miss_sweep.csv`

Configuration:

- Benchmark: `bfs`
- Inputs: `graph32`, `graph64`, `graph128`, `graph256`
- Policies: `RR`, `GTO`, `gCAWS`
- Shape: `CORES=1`, `WARPS=16`, `THREADS=16`
- Perf class: `2`
- User PC window: `0x80000094..0x80000274`
- Trace mode: `TRACE_USERPC_ONLY=1`
- Analyzer: `--no-plots --lite`
- Dcache sizes: `512`, `1024`, `2048`, `4096`, `8192`, `16384` bytes

Metric definitions:

- `span`: userpc issue-window span cycles.
- `D$ read miss`: userpc dcache read misses counted in memory, not from trace rows.
- `D$ write miss`: always `0` in this sweep because the selected BFS window is read-dominant.

## Input Working-Set Estimate

The BFS graph file contains node metadata plus an edge list. A rough global working-set estimate is:

```text
node metadata: 8B * nodes
edge list:     4B * edges
node arrays:   4 arrays * 4B * nodes
```

| Input | Nodes | Edges | Rough bytes |
|---|---:|---:|---:|
| graph32 | 32 | 188 | 1520 |
| graph64 | 64 | 376 | 3040 |
| graph128 | 128 | 754 | 6088 |
| graph256 | 256 | 1496 | 12128 |

This estimate is useful as a guide, not an exact L1 footprint. The actual kernel touches only part of the structures in the selected PC window, and access order can still cause conflict/capacity misses even when the rough byte count is below dcache size.

## Miss Trend by Input

The table uses RR read misses as the baseline for each dcache size. Policy-to-policy miss counts were very close, so RR is enough to show the working-set trend.

| Input | 512B | 1KiB | 2KiB | 4KiB | 8KiB | 16KiB |
|---|---:|---:|---:|---:|---:|---:|
| graph32 | 430 | 423 | 388 | 370 | 363 | 363 |
| graph64 | 868 | 847 | 824 | 762 | 743 | 737 |
| graph128 | 1727 | 1718 | 1679 | 1624 | 1520 | 1506 |
| graph256 | 3441 | 3421 | 3428 | 3334 | 3216 | 3036 |

Observation:

- Misses scale roughly with graph size.
- Increasing dcache reduces misses, but the curve is gradual rather than a sharp cliff.
- For graph32, misses flatten by about 8KiB.
- For graph64, misses keep improving until 16KiB but only slightly after 8KiB.
- For graph128 and graph256, even 16KiB does not make the working set fully comfortable.

## Policy Results

### graph32

| Dcache | RR span/miss | GTO span/miss | gCAWS span/miss | Best span |
|---:|---:|---:|---:|---|
| 512 | 29813 / 430 | 10392 / 410 | 29029 / 430 | GTO |
| 1024 | 29693 / 423 | 9944 / 403 | 28924 / 417 | GTO |
| 2048 | 29717 / 388 | 10272 / 379 | 28899 / 381 | GTO |
| 4096 | 29723 / 370 | 10292 / 349 | 28908 / 363 | GTO |
| 8192 | 29759 / 363 | 10292 / 342 | 28915 / 349 | GTO |
| 16384 | 29759 / 363 | 10292 / 342 | 28915 / 349 | GTO |

graph32 is small enough that miss count changes modestly with cache size, but span is dominated by scheduler behavior. GTO consistently gives the shortest span.

### graph64

| Dcache | RR span/miss | GTO span/miss | gCAWS span/miss | Best span |
|---:|---:|---:|---:|---|
| 512 | 35630 / 868 | 17498 / 862 | 34932 / 869 | GTO |
| 1024 | 35603 / 847 | 17229 / 832 | 50796 / 843 | GTO |
| 2048 | 35544 / 824 | 17125 / 819 | 34746 / 810 | GTO |
| 4096 | 35493 / 762 | 16931 / 753 | 34036 / 755 | GTO |
| 8192 | 35815 / 743 | 16795 / 745 | 33562 / 743 | GTO |
| 16384 | 35789 / 737 | 16831 / 739 | 33999 / 745 | GTO |

graph64 has a clear cache-size miss trend, but span barely changes for RR/GTO. GTO still wins strongly. The `gCAWS` 1KiB point is an outlier with high span despite similar miss count.

### graph128

| Dcache | RR span/miss | GTO span/miss | gCAWS span/miss | Best span |
|---:|---:|---:|---:|---|
| 512 | 46182 / 1727 | 28828 / 1736 | 43892 / 1761 | GTO |
| 1024 | 46168 / 1718 | 28380 / 1706 | 43059 / 1758 | GTO |
| 2048 | 46206 / 1679 | 28163 / 1661 | 42956 / 1709 | GTO |
| 4096 | 46161 / 1624 | 28694 / 1581 | 42964 / 1628 | GTO |
| 8192 | 45997 / 1520 | 27990 / 1518 | 42286 / 1514 | GTO |
| 16384 | 45988 / 1506 | 28329 / 1504 | 42879 / 1516 | GTO |

graph128 shows a gradual miss reduction as cache grows, but span remains mostly policy-dependent. GTO is consistently best.

### graph256

| Dcache | RR span/miss | GTO span/miss | gCAWS span/miss | Best span |
|---:|---:|---:|---:|---|
| 512 | 67595 / 3441 | 48978 / 3414 | 67177 / 3446 | GTO |
| 1024 | 68161 / 3421 | 48704 / 3380 | 65903 / 3421 | GTO |
| 2048 | 67298 / 3428 | 49398 / 3424 | 66332 / 3473 | GTO |
| 4096 | 68161 / 3334 | 48450 / 3313 | 66403 / 3384 | GTO |
| 8192 | 67115 / 3216 | 49175 / 3157 | 65878 / 3272 | GTO |
| 16384 | 67316 / 3036 | 48573 / 2979 | 66124 / 3034 | GTO |

graph256 is still not comfortably held by 16KiB. Misses drop from about 3.4k to 3.0k, but span remains high and policy-dependent. GTO remains best across the cache sweep.

## Conclusions

1. The new userpc L1 miss counter is working.

   The initial smoke run showed `graph32`, 1KiB dcache, RR with `D$ read miss=423`. The full sweep reports nonzero read misses for all inputs and cache sizes.

2. These mini BFS inputs still have meaningful L1 pressure.

   Even graph32 has hundreds of userpc read misses in this PC window. graph256 has roughly 3k userpc read misses even with 16KiB dcache.

3. Cache size reduces misses, but does not explain most span differences.

   Within a fixed input, policy miss counts are usually close. For example graph128 at 8KiB has RR/GTO/gCAWS read misses of `1520/1518/1514`, but spans are `45997/27990/42286`. That means scheduler behavior is mostly changing how latency is hidden or exposed, not the number of L1 misses.

4. GTO is the strongest policy in this specific mini-BFS sweep.

   GTO wins every graph/cache point in this run set. This differs from earlier 32KiB graph64-256 results where gCAWS sometimes won, so this should be treated as the behavior after the latest gCAWS update and current miss-counter patch.

5. The useful working-set sweep range is valid.

   The chosen cache sizes cover below-working-set through moderately-above-working-set for graph32/64, and below-to-near-working-set for graph128/256. For graph256, larger caches such as 32KiB and 64KiB would be useful to see the full flattening point.

## Suggested Next Runs

- Repeat graph64/128/256 at dcache `32768` and `65536` to find where misses flatten.
- Add `graph512` only after confirming trace volume is acceptable.
- For final claims, rerun each selected point at least twice because previous kmeans/BFS runs showed occasional outliers.
