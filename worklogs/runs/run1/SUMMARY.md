# Sweep — GTO / gCAWS / iPAWS x 7 workloads

Timestamp: 2026. 05. 13. (수) 23:09:22 KST

## Sim configure

- cores=1, warps=32, threads=32, l2cache=on, perf=3
- base flags: `-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0`
- VORTEX_SCHED: 1=GTO, 3=gCAWS, 4=iPAWS

## Workload inputs

| Workload | Args |
|---|---|
| bfs | `(Makefile default OPTS)` |
| kmeans | `-f100 -p5000` |
| hotspot | `512 1 2 temp_512 power_512 output.out` |
| sgemm3 | `-n128` |
| blackscholes | `(Makefile default OPTS)` |
| vecadd | `-n100000` |
| spmv | `-i /data/URP_26_spring/bbosseong/vortex_aware_scheduler/tests/opencl/spmv/Dubcova3.mtx,/data/URP_26_spring/bbosseong/vortex_aware_scheduler/tests/opencl/spmv/Dubcova3.vec` |

Note: blackscholes `optionCount` is hardcoded at `128*128` in main.cc (~448KB working set; default was `16*16`).

## Results

Columns:
- **IPC** = instrs / cycles.
- **Speedup** = IPC_policy / IPC_GTO (GTO 기준).
- **dc_read_hit** = L1 dcache read hit ratio (per core0).
- **dc_read_misses** = L1 dcache read misses (per core0).
- **MPKI** = dcache read misses × 1000 / instrs.

Note: l2 cache 통계 (l2_read_misses, l2_read_hit) 는 sim 의 카운터 이슈로 정책 무관 동일값/음수 나옴 → 표에서 제외.

### bfs

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 358231 | 235190 | 1.523156 | 1.000 | 64% | 55725 | 155.56 |
| gCAWS | 358231 | 244879 | 1.462890 | 0.960 | 69% | 47334 | 132.13 |
| iPAWS | 358231 | 235133 | 1.523525 | 1.000 | 63% | 55657 | 155.37 |

### kmeans

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 20930239 | 2643982 | 7.916181 | 1.000 | 96% | 78947 | 3.77 |
| gCAWS | 20930239 | 2818592 | 7.425778 | 0.938 | 99% | 21032 | 1.00 |
| iPAWS | 20930239 | 2652449 | 7.890911 | 0.997 | 96% | 94577 | 4.52 |

### hotspot

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 29296463 | 4377490 | 6.692525 | 1.000 | 67% | 1111401 | 37.94 |
| gCAWS | 29295119 | 5154044 | 5.683909 | 0.849 | 73% | 1108079 | 37.82 |
| iPAWS | 29296463 | 4375814 | 6.695089 | 1.000 | 67% | 1130662 | 38.59 |

### sgemm3

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 1054572718 | 92405132 | 11.412491 | 1.000 | 78% | 20776361 | 19.70 |
| gCAWS | 1054576494 | 89685690 | 11.758581 | 1.030 | 73% | 25090995 | 23.79 |
| iPAWS | 1054572334 | 92401342 | 11.412954 | 1.000 | 78% | 21196262 | 20.10 |

### blackscholes

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 198637522 | 112179504 | 1.770711 | 1.000 | 87% | 23034598 | 115.96 |
| gCAWS | 198637522 | 112179516 | 1.770711 | 1.000 | 86% | 23054382 | 116.06 |
| iPAWS | 198637522 | 112193118 | 1.770496 | 1.000 | 86% | 23033815 | 115.96 |

### vecadd

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 3217303 | 2063436 | 1.559197 | 1.000 | 94% | 114358 | 35.54 |
| gCAWS | 3217367 | 2084565 | 1.543424 | 0.990 | 94% | 136515 | 42.43 |
| iPAWS | 3216887 | 2071473 | 1.552947 | 0.996 | 94% | 105791 | 32.89 |

### spmv

| Policy | instrs | cycles | IPC | Speedup | dc_read_hit | dc_read_misses | MPKI |
|---|---|---|---|---|---|---|---|
| GTO | 67364206 | 12318534 | 5.468524 | 1.000 | 38% | 8480686 | 125.89 |
| gCAWS | 67364206 | 12326956 | 5.464788 | 0.999 | 36% | 8537374 | 126.73 |
| iPAWS | 67364206 | 12322948 | 5.466566 | 1.000 | 37% | 8495643 | 126.12 |

