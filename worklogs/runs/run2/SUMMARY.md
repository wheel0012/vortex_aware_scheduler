# Sweep (4 policy) — GTO / RR / gCAWS / iPAWS x 7 workloads

Timestamp: 2026. 05. 14. (목) 04:06:38 KST (resumed)

## Sim configure

- cores=1, warps=32, threads=32, l2cache=on, perf=2 (CLASS_MEM)
- base flags: `-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0`
- VORTEX_SCHED: 1=GTO, 2=RR, 3=gCAWS, 4=iPAWS

## Workload inputs (scaled for ~12x L1 thrash)

| Workload | Args |
|---|---|
| bfs | `/data/URP_26_spring/bbosseong/vortex_aware_scheduler/tests/opencl/bfs/graph16k.txt` |
| kmeans | `-f100 -p50000` |
| hotspot | `512 1 2 temp_512 power_512 output.out` |
| sgemm3 | `-n128` |
| blackscholes | `(Makefile default OPTS)` |
| vecadd | `-n100000` |
| spmv | `-i /data/URP_26_spring/bbosseong/vortex_aware_scheduler/tests/opencl/spmv/Dubcova3.mtx,/data/URP_26_spring/bbosseong/vortex_aware_scheduler/tests/opencl/spmv/Dubcova3.vec` |

## Results (perf=2, CLASS_MEM — accurate cache stats)

Columns:
- **IPC** = instrs / cycles.
- **Speedup_RR** = IPC_policy / IPC_RR.
- **dc_read_hit** = L1 dcache read hit ratio.
- **MPKI** = dcache read_misses × 1000 / instrs.
- **MPKI_RR** = MPKI_policy / MPKI_RR.

### bfs

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 837335 | 334845 | 2.500664 | 0.981 | 62% | 12885 | 15.39 | 0.997 |
| RR | 837463 | 328649 | 2.548199 | 1.000 | 62% | 12925 | 15.43 | 1.000 |
| gCAWS | 837383 | 322752 | 2.594509 | 1.018 | 61% | 13082 | 15.62 | 1.012 |
| iPAWS | 837335 | 331458 | 2.526217 | 0.991 | 62% | 12886 | 15.39 | 0.997 |

### kmeans

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 207342771 | 30606794 | 6.774404 | 1.001 | 50% | 1610618 | 7.77 | 1.001 |
| RR | 207342771 | 30651565 | 6.764508 | 1.000 | 50% | 1609304 | 7.76 | 1.000 |
| gCAWS | 207342771 | 31169842 | 6.652031 | 0.983 | 49% | 1611877 | 7.77 | 1.001 |
| iPAWS | 207342771 | 30646391 | 6.765651 | 1.000 | 50% | 1607116 | 7.75 | 0.999 |

### hotspot

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 29296463 | 4377490 | 6.692525 | 0.987 | 81% | 221783 | 7.57 | 0.916 |
| RR | 29296463 | 4320468 | 6.780854 | 1.000 | 79% | 242072 | 8.26 | 1.000 |
| gCAWS | 29295119 | 5154044 | 5.683909 | 0.838 | 72% | 329725 | 11.26 | 1.363 |
| iPAWS | 29296463 | 4375814 | 6.695089 | 0.987 | 79% | 243030 | 8.30 | 1.005 |

### sgemm3

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 1054572718 | 92405132 | 11.412491 | 1.001 | 76% | 5848948 | 5.55 | 1.002 |
| RR | 1054572366 | 92497310 | 11.401114 | 1.000 | 76% | 5846348 | 5.54 | 1.000 |
| gCAWS | 1054576494 | 89685690 | 11.758581 | 1.031 | 77% | 5626815 | 5.34 | 0.964 |
| iPAWS | 1054572334 | 92401342 | 11.412954 | 1.001 | 76% | 5855035 | 5.55 | 1.002 |

### blackscholes

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 198637522 | 112179504 | 1.770711 | 1.000 | 4% | 13879599 | 69.87 | 1.001 |
| RR | 198637522 | 112193240 | 1.770495 | 1.000 | 4% | 13871598 | 69.83 | 1.000 |
| gCAWS | 198637522 | 112179516 | 1.770711 | 1.000 | 4% | 13873436 | 69.84 | 1.000 |
| iPAWS | 198637522 | 112193118 | 1.770496 | 1.000 | 4% | 13871601 | 69.83 | 1.000 |

### vecadd

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 3217303 | 2063436 | 1.559197 | 0.995 | 46% | 55616 | 17.29 | 0.997 |
| RR | 3217143 | 2052058 | 1.567764 | 1.000 | 46% | 55807 | 17.35 | 1.000 |
| gCAWS | 3217367 | 2084565 | 1.543424 | 0.984 | 46% | 56239 | 17.48 | 1.007 |
| iPAWS | 3216887 | 2071473 | 1.552947 | 0.991 | 46% | 55307 | 17.19 | 0.991 |

### spmv

| Policy | instrs | cycles | IPC | Speedup_RR | dc_read_hit | dc_read_misses | MPKI | MPKI_RR |
|---|---|---|---|---|---|---|---|---|
| GTO | 67364206 | 12318534 | 5.468524 | 0.999 | 36% | 1332753 | 19.78 | 1.001 |
| RR | 67364206 | 12306812 | 5.473733 | 1.000 | 36% | 1330929 | 19.76 | 1.000 |
| gCAWS | 67364206 | 12326956 | 5.464788 | 0.998 | 36% | 1327912 | 19.71 | 0.997 |
| iPAWS | 67364206 | 12322948 | 5.466566 | 0.999 | 36% | 1331175 | 19.76 | 1.000 |

