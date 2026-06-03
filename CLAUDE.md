# CLAUDE.md — vortex-aware-scheduler 작업 가이드

**시작 전 반드시 이 파일부터 읽는다.** 추측 금지. 모르면 CAWA·iPAWS PDF 확인 후 그래도 모르면 "모르겠다"라고 한다.

로컬 paper:
- `CAWA_Coordinated_warp_scheduling_and_Cache_Prioritization_for_critical_warp_acceleration_of_GPGPU_workloads.pdf`
- `iPAWS_Instruction-issue_pattern-based_adaptive_warp_scheduling_for_GPGPUs.pdf`

## 1. 연구 목표

**RR / GTO / gCAWS 스케줄러 간 차이가 가장 잘 드러나는 환경을 찾는다.** 찾으면 그 환경에서 deep analysis (perf=1 + warp issue 시각화).

## 2. 성능 지표

| 지표 | 비교 기준 |
|---|---|
| **IPC** | RR baseline 대비 % 변화 |
| **L1D MPKI** | RR baseline 대비 % 변화 |

기본은 **perf=2**(simx, low overhead). sweet spot 환경 확정 후에만 perf=1(자세한 카운터).

## 3. 고정 Configuration (절대 안 바꿈)

- 1 core
- **32 warp** (`--warps=32`)
- **16 thread** (`--threads=16`)
- L1 only (L2 disable — `-DL2_ENABLE` 없음)
- LSU block = 1 (`-DNUM_LSU_BLOCKS=1`)
- **L1 cache size = 16KB** (`-DDCACHE_SIZE=16384`) — paper-match, sweep 안 함
- **L1 ways = 4** (`-DDCACHE_NUM_WAYS=4`)
- **Bank size = 4 고정** (`-DDCACHE_NUM_BANKS=4`)

## 4. 정책

- RR (`VORTEX_ARBITER=2`)
- GTO (`VORTEX_ARBITER=1`)
- gCAWS (`VORTEX_ARBITER=4`)

## 5. 워크로드 + Input Sweep (작은 거 → 큰 거, 작을수록 촘촘)

| Workload | Input sweep |
|---|---|
| **kmeans** | `-p {256, 512, 1024, 2048, 4096}` (nfeatures = `-f96` 고정) |
| **bfs** | extreme hub graph N ∈ `{4096, 8192, 16384, 32768}` (`graph{N}_heavyhub.txt` 또는 generator) |
| **sgemm3** | `-n {32, 64, 96, 128, 192, 256}` (모두 max-tile=16 배수) |

## 6. Sweep 차원 (워크로드/입력별)

**Bank=4 고정. WG outer × MSHR inner 표:**

kmeans/bfs (4 WG × 4 MSHR = 16 cell):
| ↓ WG / MSHR → | 2 | 4 | 8 | 16 |
|---|---|---|---|---|
| **WG=1** | ▢ | ▢ | ▢ | ▢ |
| **small** | ▢ | ▢ | ▢ | ▢ |
| **medium** | ▢ | ▢ | ▢ | ▢ |
| **large** | ▢ | ▢ | ▢ | ▢ |

sgemm3 (3 WG × 4 MSHR = 12 cell, tile=1 제외):
| ↓ WG / MSHR → | 2 | 4 | 8 | 16 |
|---|---|---|---|---|
| **small** | ▢ | ▢ | ▢ | ▢ |
| **medium** | ▢ | ▢ | ▢ | ▢ |
| **large** | ▢ | ▢ | ▢ | ▢ |

진행 순서: **WG fix → MSHR sweep (4)** → 다음 WG → 반복. 한 cell = 3 정책 = 3 run.

### WG 값 정의 (workload별)

| Workload | WG=1 | WG=small | WG=medium | WG=large | override 방법 |
|---|---|---|---|---|---|
| kmeans | 1 | 64 | 128 | 256 | `-DRD_WG_SIZE_1=N` |
| bfs | 1 | 64 | 128 | 256 | `-DMAX_THREADS_PER_BLOCK=N` (source 패치 필요) |
| sgemm3 | — | tile=4 (WG=16) | tile=8 (WG=64) | tile=16 (WG=256) | 런타임 `-t` |

→ kmeans/bfs: 16 cell × 3 정책 = 48 run/input. sgemm3: 12 cell × 3 = 36 run/input.

### MSHR 값

`-DDCACHE_MSHR_SIZE=N -DL2_MSHR_SIZE=N` N ∈ {2, 4, 8, 16}.

## 7. 실행 순서 (자동)

```
for workload in [kmeans, bfs, sgemm3]:
  for input_size in inputs[workload]:        # 작은거부터
    for wg_level in [small, medium, large]:
      for mshr in [2, 4, 8, 16]:
        for policy in [RR, GTO, gCAWS]:
          build + run + record (IPC, L1D MPKI)
```

모든 sweep은 background script + nohup+disown. summary.tsv 자동 누적.

총량: 36 run × 5 input × 3 workload ≈ **540 run**. 평균 5분 × 540 = ~45시간 (unattended).

## 8. perf=1 + 시각화 (sweet spot 발견 후)

- VX_CPL_DUMP=1 + perf=1
- KERNEL_END marker (이미 있음)
- CRIT_SNAP 간격 100~1000 cycle (sweet spot 확인 후 결정)
- per-scheduler warp issue 패턴 시각화

## 9. 자동화 원칙

- 모든 sweep은 `/tmp/run_*.sh` + `nohup ... & disown`
- console.log → `worklogs/runs/*.console.log`
- summary.tsv per run dir
- 한 번 launch하면 끝까지 자동 진행

## 10. 금지 사항

- 추측 금지. 코드 확인 또는 paper 확인.
- 부정확한 정보 전달 금지. 확신 없으면 "모르겠다".
- 고정 config 손대지 않음 (L1 size, L2 enable, NUM_WARPS, NUM_THREADS, Bank size, LSU block).
- 사용자 명시 없이 임의 코드 수정/커밋 금지 (단 WG override 위한 #ifndef 추가 등 명백한 setup 패치는 제외).

## 11. 진행 상태 추적

각 sweep dir에 `summary.tsv`:
```
policy  rc  IPC  cycles  L1_MPKI  l1_hit  mlat  dc_mshr  n_kend
```

RR/GTO/gCAWS 한 행씩. Δ는 별도 표로 정리.

## 12. 빌드 옵션 (참고 template)

```bash
BASE="-DPERF_ENABLE -DVX_NSTALL_MODE=1 -DVORTEX_SCHED_POLICY=2 \
-DNUM_LSU_BLOCKS=1 -DDCACHE_NUM_WAYS=4 -DDCACHE_SIZE=16384 \
-DDCACHE_NUM_BANKS=4 -DDCACHE_MSHR_SIZE=$MSHR -DL2_MSHR_SIZE=$MSHR"
# L2_ENABLE 없음 = L2 disabled
# WG override per workload:
#   kmeans: -DRD_WG_SIZE_1=$WG_THREADS
#   bfs:    -DMAX_THREADS_PER_BLOCK=$WG_THREADS
#   sgemm3: 런타임 -t $TILE (WG_threads = tile²)
```

런타임:
```bash
./blackbox.sh --driver=simx --app=$APP --cores=1 --warps=32 --threads=16 --perf=2 --args="..."
```

## 13. WSPAWN_WARPS_PER_BLOCK 메모

- default 0 (모든 warp 한 cycle에 spawn)
- 우리 simx GTO/gCAWS는 spawn_time 안 씀 (commit 1b0209e3) → 영향 없음
- 따로 override 안 함

## 14. 변경 로그

- 1b0209e3: GTO/GCAWS simplification (spawn_time 제거)
- 262b5ef2: bfs hub graph generator
- (다음) bfs main.cc — MAX_THREADS_PER_BLOCK `#ifndef` guard 추가 (WG override 가능하게)

## 15. 모르겠으면

- 코드 확인 (`grep -rn`)
- PDF 확인 (CAWA·iPAWS)
- 그래도 모르겠으면 "모르겠다"라고 한다. **추측 금지.**
