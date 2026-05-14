# Sweep (4 policy) — GTO / RR / gCAWS / iPAWS x 6 workloads

Timestamp: 2026. 05. 14. (목) 12:53:17 KST

## Sim configure

- cores=1, warps=32, threads=32, l2cache=on, perf=2 (CLASS_MEM)
- base flags: `-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0`
- VORTEX_SCHED: 1=GTO, 2=RR, 3=gCAWS, 4=iPAWS

## Workload inputs (sweet-spot: working set 3-25x L1, within L2)

| Workload | Args |
|---|---|
| bfs | `/data/URP_26_spring/bbosseong/vortex_aware_scheduler/tests/opencl/bfs/graph4k.txt` |
| kmeans | `-f100 -p1000` |
| hotspot | `128 1 2 temp_128 power_128 output.out` |
| sgemm3 | `-n96` |
| blackscholes | `(Makefile default OPTS)` |
| vecadd | `-n10000` |

Note: bfs uses freshly-generated `graph16k.txt` (16384 nodes, avg 6 edges).
kmeans scaled to 50K points (~200KB working set, 12x over L1).
blackscholes `optionCount` hardcoded at 128*128 in main.cc.
Cache stats from perf=2 (CLASS_MEM). Pipeline stats omitted (would need separate perf=1 run).

