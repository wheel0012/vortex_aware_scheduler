# SGEMM3, kmeans, SSSP Working-Set Cache Sweep Report

## Scope

This report summarizes the working-set-aware dcache sweep for `sgemm3`, `kmeans`, and `sssp`.

Raw CSV:

- `worklogs/ws_cache_sweep_sgemm3_kmeans_sssp.csv`

Run ranges:

- `run6-run26`: `sgemm3`
- `run27-run41`: `kmeans`
- `run42-run53`: `sssp`

Smoke runs `run1-run5` were excluded from the CSV and analysis.

Common configuration:

- Policies: `RR`, `GTO`, `gCAWS`
- Shape: `CORES=1`, `WARPS=16`, `THREADS=16`
- Perf class: `2`
- Analyzer: `--no-plots --lite`
- Trace mode: `TRACE_USERPC_ONLY=1`
- Scheduler policy matched the issue arbiter: `SCHED_POLICY_MATCH_ARBITER=1`

User PC windows:

- `sgemm3`: `0x80000450..0x800008ff`
- `kmeans`: `0x80000094..0x80000230`
- `sssp`: `0x80000460..0x80000960`

## Working-Set Estimates

These are rough global-data estimates for choosing cache sizes. They are not exact L1 residency requirements because the selected PC window sees interleaved requests, writebacks, runtime buffers, and coalesced LSU traffic.

### SGEMM3

Formula: `A + B + C = 3 * N * N * 4B`. Local tile storage is `2 * tile^2 * 4B = 128B` for `tile=4`.

| Input | Estimated global bytes | Dcache sweep |
|---|---:|---|
| `N8_T4` | 768 | 512B, 1KiB, 2KiB, 4KiB, 8KiB, 16KiB, 32KiB |
| `N16_T4` | 3072 | 512B, 1KiB, 2KiB, 4KiB, 8KiB, 16KiB, 32KiB |
| `N32_T4` | 12288 | 512B, 1KiB, 2KiB, 4KiB, 8KiB, 16KiB, 32KiB |

### kmeans

Formula: `feature + clusters + membership = points * features * 4B + clusters * features * 4B + points * 4B`.

| Input | Estimated bytes | Dcache sweep |
|---|---:|---|
| `p32_f32_c8` | 5248 | 1KiB, 4KiB, 8KiB, 16KiB, 32KiB |
| `p64_f32_c8` | 9472 | 1KiB, 4KiB, 8KiB, 16KiB, 32KiB |
| `p128_f32_c8` | 17920 | 1KiB, 4KiB, 8KiB, 16KiB, 32KiB |

### SSSP

Formula: `row + col + data + vector1 + vector2 + stop = (nodes+1)*4B + edges*8B + nodes*8B + 4B`.

| Input | Nodes / edges | Estimated bytes | Dcache sweep |
|---|---:|---:|---|
| `tiny` | 6 / 9 | 152 | 512B, 2KiB, 8KiB, 32KiB |
| `star_chain16` | 16 / 29 | 432 | 512B, 2KiB, 8KiB, 32KiB |
| `star_chain64` | 64 / 125 | 1776 | 512B, 2KiB, 8KiB, 32KiB |

The generated SSSP inputs are under `worklogs/generated_inputs/sssp/`, which is ignored by git.

## SGEMM3 Results

RR miss trend:

| Input | Cache | RR span | RR read miss | RR non-cold miss | RR reuse hit |
|---|---:|---:|---:|---:|---:|
| `N8_T4` | 512B | 65693 | 3162 | 2736 | 12% |
| `N8_T4` | 32KiB | 65562 | 2846 | 2417 | 22% |
| `N16_T4` | 512B | 123765 | 8891 | 7977 | 17% |
| `N16_T4` | 32KiB | 126010 | 6804 | 5867 | 39% |
| `N32_T4` | 512B | 276993 | 30238 | 29245 | 15% |
| `N32_T4` | 32KiB | 275155 | 13881 | 12914 | 62% |

Best span:

| Input | Cache range | Best-policy pattern |
|---|---|---|
| `N8_T4` | all caches | GTO wins all points, but only by about 5% over RR. |
| `N16_T4` | 512B-8KiB | gCAWS often wins narrowly. |
| `N16_T4` | 16KiB-32KiB | GTO wins narrowly. |
| `N32_T4` | all caches | GTO wins all points, around 9-14% over RR. |

Interpretation:

- Cache size clearly reduces read and non-cold misses, especially for `N32_T4`.
- Span does not track the miss curve tightly. For example `N32_T4` RR read misses drop from `30238` at 512B to `13881` at 32KiB, but span stays around `270k-279k`.
- This matches the earlier intuition about SGEMM3: tiling and compute dominate enough that cache capacity changes are visible in misses but muted in scheduler span.
- The line-local ratio stays low, roughly `7-15%`, and `64+ line stride` is high. The global request stream still jumps between regions even when the logical matrix footprint fits.

## kmeans Results

RR miss trend:

| Input | Cache | RR span | RR read miss | RR non-cold miss | RR reuse hit |
|---|---:|---:|---:|---:|---:|
| `p32_f32_c8` | 1KiB | 89316 | 4449 | 3761 | 23% |
| `p32_f32_c8` | 32KiB | 86444 | 3067 | 2580 | 47% |
| `p64_f32_c8` | 1KiB | 88846 | 5526 | 4848 | 30% |
| `p64_f32_c8` | 32KiB | 88937 | 3129 | 2640 | 62% |
| `p128_f32_c8` | 1KiB | 93356 | 7624 | 6886 | 38% |
| `p128_f32_c8` | 32KiB | 91723 | 4503 | 3985 | 64% |

Best span:

| Input | Cache range | Best policy |
|---|---|---|
| `p32_f32_c8` | all caches | GTO |
| `p64_f32_c8` | all caches | GTO |
| `p128_f32_c8` | all caches | GTO |

Interpretation:

- kmeans shows the cleanest GTO advantage among the three benchmarks in this sweep.
- GTO wins every kmeans input/cache point, often by about 17-25% over RR.
- Miss counts are very close across policies at the same input/cache point. For `p64_f32_c8` at 32KiB, RR/GTO/gCAWS read misses are `3129/3059/3045`, but spans are `88937/67287/86910`.
- That means the scheduler effect is not primarily “fewer L1 misses”; it is mostly latency exposure/hiding and issue ordering.
- Cache growth improves reuse hit noticeably. `p64_f32_c8` RR reuse hit rises from `30%` at 1KiB to `62%` at 32KiB.

## SSSP Results

RR miss trend:

| Input | Cache | RR span | RR read miss | RR non-cold miss | RR reuse hit |
|---|---:|---:|---:|---:|---:|
| `tiny` | 512B | 58831 | 2364 | 1998 | 8% |
| `tiny` | 32KiB | 58913 | 2236 | 1919 | 12% |
| `star_chain16` | 512B | 58838 | 2367 | 2000 | 8% |
| `star_chain16` | 32KiB | 58910 | 2236 | 1920 | 12% |
| `star_chain64` | 512B | 58926 | 2385 | 2012 | 8% |
| `star_chain64` | 32KiB | 58964 | 2246 | 1930 | 12% |

Best span:

| Input | Cache range | Best-policy pattern |
|---|---|---|
| `tiny` | all caches | RR |
| `star_chain16` | all caches | RR |
| `star_chain64` | all caches | RR |

Interpretation:

- SSSP behaves almost cache-insensitive in this selected window.
- RR consistently wins or ties, GTO is slower, and gCAWS is usually close to RR except for a few outliers.
- Misses drop slightly as cache grows, but reuse hit remains low: about `8-12%`.
- The line-local ratio is also very low, around `5-6%`, and the `64+ line stride` ratio is around `90%` in the CSV. This selected SSSP window is dominated by sparse, far-apart accesses and kernel/runtime interleaving.
- The estimated graph working sets are tiny, yet non-cold misses remain around `1900-2000`. That strongly suggests the selected broad PC window is capturing repeated accesses across several buffers/regions and conflict-like behavior, not simply the graph data footprint.

## Cross-Benchmark Conclusions

1. Cold miss exclusion was useful.

   In all three benchmarks, most read misses are non-cold misses. This means the useful signal is not just first-touch traffic; it is reuse that fails to hit in L1.

2. Cache-size effects and scheduler effects are separable.

   SGEMM3 and kmeans both show miss reductions as cache grows, but span often changes much less than misses. kmeans in particular has nearly identical miss counts across policies while GTO has much shorter span.

3. kmeans is the best scheduler-sensitive benchmark here.

   It has a stable GTO win across input sizes and cache sizes. The memory behavior changes with cache size, but the policy ranking is robust.

4. SGEMM3 is useful for cache behavior, less clean for scheduler claims.

   Larger `N32_T4` shows GTO wins, but the miss curve and span curve are not tightly coupled. This supports the idea that compute/tiling masks part of the scheduler effect.

5. SSSP needs a narrower kernel window for stronger cache-working-set interpretation.

   The current broad `0x460..0x960` window works and produces stable metrics, but even tiny graphs show many non-cold misses. For a cleaner SSSP cache study, the next step should be symbol-specific PC windows, especially isolating `spmv_min_dot_plus_kernel` from `vector_assign`, `vector_diff`, and initialization.

## Suggested Next Step

For final figures, use:

- kmeans `p32/p64/p128`, dcache `1KiB/16KiB/32KiB`, policies `RR/GTO/gCAWS`.
- SGEMM3 `N32_T4`, dcache `512B/8KiB/16KiB/32KiB`, policies `RR/GTO/gCAWS`.
- SSSP only after narrowing the PC window to the SpMV kernel body.
