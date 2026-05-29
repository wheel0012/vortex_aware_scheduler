# run1-run99 Trace Experiment Report

## Scope

This report reconstructs `worklogs/trace_runs/run1` through `run99` from their saved `SUMMARY.md` files. It complements `worklogs/run1xx_experiment_report.md`.

The runs fall into four broad phases:

- `run1-run23`: trace script bring-up and early benchmark probes.
- `run24-run47`: kmeans, hotspot, and BFS userpc-window exploration.
- `run48-run80`: kmeans input/cache sweep with `RR`, `GTO`, and `gCAWS`.
- `run81-run99`: BFS input/cache sweep, plus a few interleaved kmeans reruns.

Metric note:

- For runs with a `Span cycles` column, the primary metric is userpc issue-window span cycles. Lower is better.
- Some early runs used an older summary schema or had `span=0`; those are useful only for trace/debug status, not performance comparison.
- Several runs have `missing_trace`, `run_rc_2`, or `build_fail`; they are listed as bring-up artifacts and should not be used as clean scheduler data.

## Phase A: Early Bring-Up, run1-run23

### sgemm3 PC-window bring-up: run1-run9

Configuration:

- Benchmark: `sgemm3`.
- Shape: `CORES=1`, `WARPS=16`, `THREADS=1`.
- Args: `-n4 -t2`.
- User window: event window `WSPAWN:3..WSPAWN:4`, PC `0x1c4..0x248`.
- Policies: mostly `RR`, `GTO`, `gCAWS`.
- Perf classes: `1 2`.

Results:

- `run1`, `run2`, `run4-run7`, and `run9` produced traces.
- `run3` and `run8` were build failures.
- The summaries report `span=0` and `IPC=0.00%`, so these runs are not reliable for performance comparison.

Interpretation:

These were trace/window bring-up runs. The important outcome is that the script could collect traces for small `sgemm3`, but this window/summary combination was not useful for span-based scheduler analysis.

### gto_arith_chain bring-up: run10-run16

Configuration:

- Benchmark: `gto_arith_chain`.
- Shape: `CORES=1`, `WARPS=16`, `THREADS=1`.
- Initial window reused the `sgemm3` PC/event window; later runs used `USER_PC_AUTO=1`.

Results:

| Run | Args | Status | Main observation |
|---|---|---|---|
| run10 | default | missing trace | Runtime returned `run_rc_1`. |
| run11 | default | ok but `span=0` | Trace rows appeared, but the selected PC window was wrong for useful span analysis. |
| run12 | `-n16 -l1 -i64` | ok | Auto PC produced usable issue counts; GTO had slightly shorter span than RR in rows, but old summary still showed IPC as `0.00%`. |
| run13-run16 | `-n4 -l1 -i16` | ok | All policies had the same span, `988`, so no scheduler separation. |

Interpretation:

The synthetic chain benchmark was useful to debug PC auto inference, but the small case was too regular to distinguish policies. Larger `run12` hinted that GTO could reduce span, but the old metric output was incomplete.

### BFS shape sweep: run17-run20

Configuration:

- Benchmark: `bfs`, input `graph32.txt`.
- `USER_PC_AUTO=1`, event from `WSPAWN:3`.
- Shapes:
  - `run17`: `WARPS=8`, `THREADS=4`
  - `run18/run20`: `WARPS=16`, `THREADS=2`
  - `run19`: `WARPS=16`, `THREADS=4`

Results from saved summaries:

| Run | Shape | RR rows | GTO rows | gCAWS rows | Best by row count |
|---|---|---:|---:|---:|---|
| run17 | 8x4 | 104154 | 106089 | 103487 | gCAWS |
| run18 | 16x2 | 141882 | 136019 | 137669 | GTO |
| run19 | 16x4 | 115685 | 114856 | 115202 | GTO |
| run20 | 16x2 | 141882 | 136019 | 137669 | GTO |

Interpretation:

These runs show that BFS can expose policy differences, but they predate the cleaner explicit-PC/span setup. Treat them as exploratory.

### Mixed benchmark probe: run21-run23

Configuration:

- `run21`: `gto_arith_chain`, `bfs`, `sgemm3`; `WARPS=32`, `THREADS=2`; userpc monitor disabled.
- `run22`: only `gto_arith_chain`, same shape.
- `run23`: `sgemm3` build failure.

Observation:

With userpc monitor disabled, these runs are useful for end-to-end trace sanity only. They do not provide the later userpc-window span metric.

## Phase B: First kmeans/hotspot/BFS Window Exploration, run24-run47

### kmeans auto-PC and size sweep: run24-run40

Configuration:

- Benchmark: `kmeans`.
- Early shape: `WARPS=16`, `THREADS=2`; later shape: `WARPS=16`, `THREADS=16`.
- Explicit kmeans PC window after bring-up: `0x80000094..0x80000230`.

Key results:

| Run | Args | Status | RR span | GTO span | Winner |
|---|---|---|---:|---:|---|
| run24 | `-p128 -f32 -n8 -m8 -l1` | build fail | ? | ? | n/a |
| run25 | same | missing trace | ? | ? | n/a |
| run26 | same | ok | 193883 | 226356 | RR |
| run27 | same | ok | 193883 | 256910 | RR |
| run28 | `-p512 -f100 -n5 -m5 -l1` | ok | 394441 | 388736 | GTO |
| run30 | `-p1024 -f100 -n5 -m5 -l1` | ok | 753820 | 760870 | RR |
| run31 | `-p768 -f100 -n5 -m5 -l1` | ok | 568593 | 568968 | RR, basically tie |
| run33 | `-p640 -f100 -n5 -m5 -l1` | ok | 487643 | 466214 | GTO |
| run34 | `-p896 -f100 -n5 -m5 -l1` | ok | 658827 | 646067 | GTO |
| run36 | `-p320 -f100 -n5 -m5 -l1` | ok | 290727 | 266417 | GTO |
| run37 | `-p384 -f100 -n5 -m5 -l1` | ok | 311794 | 291292 | GTO |
| run38 | `-p512 -f100 -n5 -m5 -l1` | ok | 394441 | 388736 | GTO |
| run39 | `-p768 -f100 -n5 -m5 -l1` | ok | 568593 | 568968 | RR, basically tie |
| run40 | `-p1024 -f100 -n5 -m5 -l1` | ok | 753820 | 760870 | RR |

Observation:

kmeans showed small-to-moderate GTO wins for mid-size inputs, especially `p320-p640`, but the effect faded or reversed near `p768-p1024`. That matches the later discussion: once the input becomes large enough, the scheduler effect is partly hidden by the broader memory/compute balance.

### hotspot first window sweep: run41-run44

Configuration:

- Benchmark: `hotspot`.
- Shape: `WARPS=16`, `THREADS=16`.
- Explicit hotspot window: `0x80000348..0x80000900`.

| Run | Size | Status | RR span | GTO span | Winner |
|---|---:|---|---:|---:|---|
| run41 | 64 | missing trace | ? | ? | n/a |
| run42 | 64 | ok | 153035 | 162473 | RR |
| run43 | 128 | ok | 366947 | 392763 | RR |
| run44 | 512 | ok | 5072565 | 5063394 | GTO, effectively tie |

Observation:

Hotspot did not show a clear positive GTO effect. For size64/128, RR won; size512 was essentially a tie with a tiny GTO edge. This agrees with the later hotspot sweeps.

### BFS first explicit-PC sweep: run45-run47

Configuration:

- Benchmark: `bfs`.
- Shape: `WARPS=16`, `THREADS=16`.
- Explicit BFS window: `0x80000094..0x80000274`.

| Run | Input | RR span | GTO span | Winner |
|---|---|---:|---:|---|
| run45 | graph32 | 29759 | 10292 | GTO |
| run46 | graph4096 | 232888 | 220033 | GTO |
| run47 | graph16k | 852227 | 868040 | RR |

Observation:

BFS was already the strongest candidate benchmark here. It gave a huge GTO win on graph32, a smaller win on graph4096, then reversed at graph16k. `run47` was one of the “too large / hard to interpret” runs because span and trace size became large and the result likely reflected saturation rather than pure scheduler behavior.

## Phase C: kmeans Cache/Input Sweep, run48-run80

Configuration:

- Benchmark: `kmeans`.
- Shape: `WARPS=16`, `THREADS=16`.
- PC window: `0x80000094..0x80000230`.
- Features/clusters/loops: `-f32 -n5 -m5 -l1`.
- Dcache sizes: `4096`, `8192`, `16384`, `32768`.
- Policies: `RR`, `GTO`, `gCAWS`.

Representative valid results:

| Dcache | Points | Run | RR span | GTO span | gCAWS span | Best |
|---:|---:|---|---:|---:|---:|---|
| 4096 | 64 | run49 | 77678 | 55425 | 55430 | GTO |
| 4096 | 128 | run50 | 61306 | 58857 | 80744 | GTO |
| 4096 | 192 | run53 | 89710 | 80300 | 78114 | gCAWS |
| 4096 | 256 | run55 | 100709 | 92819 | 90173 | gCAWS |
| 8192 | 64 | run57 | 77703 | 57564 | 55112 | gCAWS |
| 8192 | 128 | run59 | 81244 | 63146 | 58635 | gCAWS |
| 8192 | 192 | run61 | 89971 | 76632 | 89971 | GTO |
| 8192 | 256 | run63 | 101240 | ? | 87851 | gCAWS |
| 8192 | 320 | run66 | 111682 | 113134 | 113341 | RR |
| 16384 | 64 | run65 | 76224 | 56982 | 55112 | gCAWS |
| 16384 | 128 | run70 | 79329 | 62834 | 60353 | gCAWS |
| 16384 | 192 | run72 | 90225 | 77599 | 75909 | gCAWS |
| 16384 | 256 | run71 | 94263 | 112241 | 93216 | gCAWS |
| 16384 | 320 | run76 | 117796 | 110961 | 117796 | GTO |
| 32768 | 64 | run78 | 77452 | 55680 | ? | GTO |
| 32768 | 128 | run80 | 78102 | 61560 | 80744 | GTO |
| 32768 | 192 | run82 | 80300 | 78114 | 77127 | gCAWS |
| 32768 | 256 | run79 | 101792 | 95303 | 91828 | gCAWS |
| 32768 | 320 | run86 | 140016 | 110961 | 140016 | GTO |

Incomplete/noisy runs:

- `run51`, `run52`, `run56`, `run58`, `run62-run64`, `run67`, `run73-run75`, `run77`, and `run84` had at least one missing/build-failed/run-failed policy result.
- Some repeated point/cache combinations disagree slightly or swap policy labels due to reruns during script/debug work. Use the table above as a practical summary, not a statistically controlled sweep.

Observation:

kmeans does show scheduler separation, but the result is noisy. GTO/gCAWS often beat RR for `p64-p256`, especially when dcache is 8 KiB or larger. At `p320`, the result becomes less stable: RR wins in `run66`, GTO wins in `run76/run86`. This again suggests the sweep crosses from scheduler-sensitive behavior into a more saturated regime.

## Phase D: BFS Cache/Input Sweep, run81-run99

Configuration:

- Benchmark: `bfs`.
- Shape: `WARPS=16`, `THREADS=16`.
- PC window: `0x80000094..0x80000274`.
- Dcache sizes: `4096`, `8192`, `16384`, `32768`.
- Inputs: graph32, graph64, graph128, graph256, graph512.

Clean BFS results:

| Run | Dcache | Input | RR span | GTO span | gCAWS span | Best |
|---|---:|---|---:|---:|---:|---|
| run81 | 4096 | graph32 | 29723 | ? | 10522 | gCAWS |
| run83 | 4096 | graph64 | 16124 | ? | 16831 | RR |
| run85 | 4096 | graph128 | 25104 | 45967 | 28294 | RR |
| run87 | 4096 | graph256 | 68026 | 48450 | 48026 | gCAWS |
| run88 | 4096 | graph512 | 76809 | 57268 | 56281 | gCAWS |
| run89 | 8192 | graph32 | 29759 | 10292 | 10594 | GTO |
| run90 | 8192 | graph64 | 35815 | 16795 | 16137 | gCAWS |
| run91 | 8192 | graph128 | 45997 | 27990 | 24979 | gCAWS |
| run92 | 8192 | graph256 | 67115 | 49175 | 48761 | gCAWS |
| run93 | 8192 | graph512 | 76356 | 56419 | 54690 | gCAWS |
| run94 | 16384 | graph32 | 29759 | 10292 | 10594 | GTO |
| run95 | 16384 | graph64 | 35789 | 16831 | 16124 | gCAWS |
| run96 | 16384 | graph128 | 45988 | 28329 | 25122 | gCAWS |
| run97 | 16384 | graph256 | 67316 | 48573 | 58741 | GTO |
| run98 | 16384 | graph512 | 72595 | 56997 | 53884 | gCAWS |
| run99 | 32768 | graph32 | 29759 | 10292 | 10594 | GTO |

Continuation:

- The 32 KiB sweep continues in `run100-run103` in the separate run1xx report:
  - graph64: gCAWS best.
  - graph128: gCAWS best.
  - graph256: gCAWS best.
  - graph512: GTO best.

Observation:

BFS is the cleanest benchmark family in the run1-run99 set. Compared with kmeans, the BFS results are more repeatable and the scheduler effect is much larger. The strongest wins occur at graph32/64/128; larger graphs still show differences but begin to move toward saturation.

## Cross-Run Conclusions

1. BFS is the most useful benchmark for scheduler analysis.

   Across `run45-run47`, `run81-run99`, and later `run100-run103`, BFS repeatedly shows large RR vs GTO/gCAWS differences. This is the best candidate for the scheduler paper-style effect.

2. gCAWS and GTO both help BFS, but the winner depends on input/cache.

   gCAWS is often best for graph64-512 in the 8 KiB and 16 KiB cache sweeps, while GTO is consistently excellent at graph32 and wins some larger cases.

3. hotspot is mostly a negative/control benchmark.

   `run42-run44` and later `run104-run109` show RR is usually as good as or better than GTO, with gCAWS only marginally better in a few size128 cases.

4. kmeans is scheduler-sensitive but noisy.

   `run48-run80` show many GTO/gCAWS wins, but missing traces and inconsistent repeated combinations make it less clean than BFS. It is still worth using after the miss-counter patch, but results should be averaged or repeated.

5. Early sgemm3 and synthetic-chain runs were primarily tooling bring-up.

   `run1-run16` helped debug trace windows and PC inference, but most are not suitable for final performance claims.

6. The next useful reruns are:

   - BFS graph32-512 with dcache `4096`, `8192`, `16384`, `32768`, now with userpc dcache miss counters.
   - kmeans `p64-p320`, `f32`, dcache `8192` and `16384`, repeated enough to remove label/run noise.
   - hotspot size64/128 as a control case.
