# Vortex Aware Scheduler Sweep

Timestamp: Sun May 24 08:48:23 KST 2026

## Configuration

- root: `/home/so/vortex_aware_scheduler`
- build: `/home/so/vortex_aware_scheduler/build`
- cores=1, warps=32, threads=32, l2cache=on
- perf classes: `2`
- base flags: `-DPERF_ENABLE `
- VORTEX_ARBITER: Priority=0, GTO=1, RR=2, Matrix=3, gCAWS=4

## Workloads

| Workload | Args |
|---|---|
| bfs | `/home/so/vortex_aware_scheduler/tests/opencl/bfs/graph4k.txt` |
| kmeans | `-f100 -p1000` |
| hotspot | `128 1 2 temp_128 power_128 output.out` |
| sgemm3 | `-n96` |
| blackscholes | `(Makefile default OPTS)` |
| vecadd | `-n10000` |
| streamcluster | `2 4 4 16 16 16 none output.txt 1 -t gpu -d 0` |


## Results

IPC speedup is normalized to RR for each workload/perf class when RR exists.

## perf=2

### bfs

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | 395152 | 268950 | 1.469240 | 1.000 | 36% | 14245 | 36.05 |
| GTO | 395152 | 266020 | 1.485422 | 1.011 | 36% | 14163 | 35.84 |
| gCAWS | 395152 | 262366 | 1.506110 | 1.025 | 36% | 14122 | 35.74 |

### kmeans

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | 5432482 | 685460 | 7.925309 | 1.000 | 44% | 83082 | 15.29 |
| GTO | 5432482 | 684179 | 7.940147 | 1.002 | 44% | 83088 | 15.29 |
| gCAWS | 5432482 | 686942 | 7.908211 | 0.998 | 44% | 83019 | 15.28 |

### hotspot

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | ? | ? | ? | ? | ?% | ? | ? |
| GTO | ? | ? | ? | ? | ?% | ? | ? |
| gCAWS | ? | ? | ? | ? | ?% | ? | ? |

### sgemm3

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | 14788490 | 2658827 | 5.562036 | 1.000 | 47% | 306603 | 20.73 |
| GTO | 14788490 | 2652239 | 5.575851 | 1.002 | 47% | 307528 | 20.80 |
| gCAWS | 14788490 | 2664753 | 5.549666 | 0.998 | 47% | 305946 | 20.69 |

### blackscholes

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | 298994 | 290532 | 1.029126 | 1.000 | 9% | 20686 | 69.19 |
| GTO | 298994 | 291938 | 1.024170 | 0.995 | 9% | 20677 | 69.16 |
| gCAWS | 298994 | 291980 | 1.024022 | 0.995 | 9% | 20675 | 69.15 |

### vecadd

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | 568873 | 487073 | 1.167942 | 1.000 | 23% | 25392 | 44.64 |
| GTO | 568873 | 490275 | 1.160314 | 0.993 | 23% | 25413 | 44.67 |
| gCAWS | 568873 | 494879 | 1.149519 | 0.984 | 23% | 25376 | 44.61 |

### streamcluster

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| RR | ? | ? | ? | ? | ?% | ? | ? |
| GTO | ? | ? | ? | ? | ?% | ? | ? |
| gCAWS | ? | ? | ? | ? | ?% | ? | ? |

