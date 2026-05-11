# URP: Vortex GPGPU에 CAWA+iPAWS 적응형 스케줄러 적용

> 이 문서는 Vortex 본체 README와 분리된 **URP 프로젝트 작업 일지**다.
> 코드 변경 / 실험 설계 / 결과 / 다음 액션을 시간 순서로 패치한다.

## 개요

본 프로젝트는 Vortex GPGPU 시뮬레이터 (simx) 에 다음 두 기법을 결합한
적응형 스케줄러를 구현하는 것을 목표로 한다.

- **CAWA** (Coordinated Criticality-Aware Warp Acceleration, Lee & Wu, ISCA 2015)
  - gCAWS: 가장 critical한 warp을 우선 issue
  - CACP: critical warp의 cache 블록을 way reservation + SHiP-CB로 보호
- **iPAWS** (Instruction-issue Pattern-based Adaptive Warp Scheduling)
  - issue pattern (criticality 분포) 을 보고 매 구간 RR ↔ gCAWS 중 선택

## 전체 로드맵

1. **Phase 1**: gCAWS warp 스케줄러 ✅
2. **Phase 2**: CACP 캐시 관리 (way reservation + SHiP-CB) ✅
3. **Phase 3**: iPAWS 상태 기계 (gCAWS+CACP ↔ RR 적응) ✅
4. **Phase 3 후속**: 워크로드 규모 / iPAWS-CACP 분리 / CACP 튜닝 ← **현재 단계**
5. **Phase 4**: RTL 구현
6. **Phase 5**: FPGA 평가

---

## Phase 1: gCAWS 스케줄러 (simx 에뮬레이터)

| 파일 | 변경 내용 |
|------|----------|
| `sim/simx/emulator.h` | `WarpSchedulePolicy::gCAWS` enum, `warp_cpl_t` 구조체(`instr_count`/`stall_cycles`/`criticality`), 멤버 `critical_warp_`, `warp_cpl_`, 메서드 `select_gcaws_warp()`, `update_cpl_counters()` 선언. |
| `sim/simx/emulator.cpp` | `update_cpl_counters()`로 매 사이클 stall/instr 카운터 갱신, 그리고 `nInst*CPI_avg + nStall` 공식으로 criticality 계산. `select_gcaws_warp()`는 greedy 단계(현 critical warp 유지) → highest-criticality + oldest-ready tie-break 순으로 선택. 기본 정책을 `gCAWS`로 설정. |

## Phase 2: CACP (CAWA의 캐시 관리 기법)

CACP는 두 메커니즘으로 구성된다.

1. **Way reservation (way partitioning)**: critical warp 전용으로 cache way의
   일부(`DCACHE_NUM_WAYS/2`)를 예약. non-critical warp은 reserved way에서 절대 evict 불가.
2. **SHiP-CB + RRIP 기반 교체 정책**: PC signature → SHCT(1024 × 3-bit 카운터) →
   삽입 시 RRPV(0=near, 2=long, 3=distant) 결정. 또 hit 시 SHCT++/reused=true,
   evict 시 reused=false이면 SHCT--.
3. **CACP 가중치**: critical warp의 insertion은 SHCT 예측을 덮어쓰고 `RRPV=0`(near)로
   강제하여 cache에 더 오래 머무르도록 함.

| 파일 | 변경 내용 |
|------|----------|
| `sim/simx/types.h` / `types.cpp` | `LsuReq` / `MemReq`에 `wid`, `pc` 필드 추가. `LsuMemAdapter::tick()`이 두 필드를 LsuReq → MemReq로 복사. |
| `sim/simx/func_unit.cpp` | LSU에서 `lsu_req.wid = trace->wid`, `lsu_req.pc = trace->PC` 설정. |
| `sim/simx/mem_coalescer.cpp` | coalesce 시 in_req의 `wid`/`pc`를 out_req로 복사. |
| `sim/simx/cache_sim.h` | `CacheSim::Config`에 `cacp_enable`, `cacp_reserved_ways` 필드. `set_critical_warp(int wid)` API 추가. |
| `sim/simx/cache_sim.cpp` | `line_t`를 LRU 카운터 대신 `signature`/`rrpv`/`reused`로 교체. SHCT 클래스 구현(1024 entry, 3-bit). `set_t::tag_lookup`를 RRIP 기반 victim selection + way-partition로 재작성. `CacheBank`에 `shct_`, `critical_warp_id_` 멤버. 핵심 후크: hit 시 SHCT++/RRPV=0, miss/evict 시 SHCT--, fill 시 critical/SHCT 예측에 따라 RRPV 결정. |
| `sim/simx/cache_cluster.h` | `set_critical_warp(int wid)`를 내부 모든 `CacheSim`으로 전파. |
| `sim/simx/socket.{h,cpp}` | dcache용 `set_critical_warp(int wid)`. dcache config에 `cacp_enable=(DCACHE_NUM_WAYS>=2)`, `cacp_reserved_ways=DCACHE_NUM_WAYS/2` 전달. |
| `sim/simx/core.{h,cpp}` | `Core::set_critical_warp(int wid)`가 socket으로 위임. `core.cpp`에 `socket.h` 포함. |
| `sim/simx/emulator.cpp` | `select_gcaws_warp()`에서 `critical_warp_`가 변경될 때마다 `core_->set_critical_warp(...)` 호출. |

## Phase 3: iPAWS 상태 기계

iPAWS는 두 단계 상태 기계로 동작한다.

1. **Adapt phase (1024 cycles)**: 매 사이클 active warp들의 criticality
   분포를 샘플링. WOI(Warps of Interest) = `criticality < median × 0.5` 인 warp.
   샘플 WOI 비율의 평균을 누적.
2. **Decide**: 평균 WOI 비율 ≥ 0.4 → 분포가 skewed(concave) → **gCAWS+CACP**
   선택. 그렇지 않으면 uniform(convex) → **RR** 선택 (`critical_warp_=-1`로
   세팅하여 CACP 자동 비활성).
3. **Execute phase (16384 cycles)**: 선택된 정책 실행. 종료 후 다시 Adapt로 복귀.

| 파일 | 변경 내용 |
|------|----------|
| `sim/simx/emulator.h` | `WarpSchedulePolicy::iPAWS` enum, `iPAWSPhase` enum, `ipaws_state_t` 구조체. 메서드 `select_ipaws_warp()`, `ipaws_sample_and_step()`, `compute_woi_ratio()` 선언. |
| `sim/simx/emulator.cpp` | `compute_woi_ratio()` (active warp criticality → nth_element 기반 median → WOI 비율), `ipaws_sample_and_step()` (Adapt 샘플 수집/Decide 전환/Execute 카운트다운), `select_ipaws_warp()` (현재 phase에 따라 gCAWS/RR로 디스패치). 기본 정책을 `iPAWS`로 변경. RR phase 진입 시 `core_->set_critical_warp(-1)` 호출. |

---

## Build-time 옵션 (정책/CACP/iPAWS 튜닝)

소스를 건드리지 않고 빌드 시 `CONFIGS=...` 로 다음 매크로를 넘기면 정책과
캐시-사이드 동작을 골라 비교할 수 있다.

| 매크로 | 값 | 기본값 | 설명 |
|---|---|---|---|
| `VORTEX_SCHED` | 0/1/2/3/4 | 4 | Static / GTO / RR / gCAWS / iPAWS |
| `VORTEX_CACP_ENABLE` | 0/1 | 1 | dcache의 CACP(way reservation + RRPV bias) 전역 토글 |
| `VORTEX_CACP_RESERVED` | 0..A−1 | A/2 | CACP가 critical warp용으로 예약하는 way 개수 |
| `VORTEX_IPAWS_USE_CACP` | 0/1 | 1 | 0이면 iPAWS의 gCAWS 분기에서도 CACP를 끔 (iPAWS 의사결정 단독 평가용) |
| `VORTEX_IPAWS_ADAPT_CYCLES` | int | 1024 | iPAWS Adapt phase 길이 |
| `VORTEX_IPAWS_EXECUTE_CYCLES` | int | 16384 | iPAWS Execute phase 길이 |
| `VORTEX_IPAWS_WOI_RATIO` | float | 0.5 | median × 비율 미만이면 WOI |
| `VORTEX_IPAWS_CONCAVE_TH` | float | 0.4 | 평균 WOI 비율 ≥ 이 값이면 concave (gCAWS 선택) |

## 실험 인프라 (worklogs/)

```
worklogs/
├── scripts/
│   ├── bench_sweep.sh          # 메인 스윕: RR / GTO / gCAWS / iPAWS (4종)
│   └── exp_cacp_ablation.sh    # CACP 예약 way 0~4 스윕 (ablation 전용)
└── runs/                        # 모든 실행 결과 (timestamped)
    ├── run1_warps16_phase1.log
    ├── run1_warps16_summary.txt
    └── run2_warps32_v1/         # 첫 warps=32 baseline
```

- `worklogs/scripts/bench_sweep.sh "<bench> [args...]" ...` : 4개 정책으로
  한 번씩 빌드 후 주어진 벤치마크를 돌리고 summary 정리.
- `worklogs/scripts/exp_cacp_ablation.sh` : gCAWS 고정, CACP 예약 way 0~4 스윕.
  RTL에서는 CACP를 빼기로 했으므로 이 스크립트는 ablation 보고용으로만 사용.

기본 BASE 환경: `cores=1, warps=32, threads=32, DCACHE_NUM_WAYS=8`. 작은 input은
cache 압박이 없어 정책 차이가 묻히므로, sweep에서는 다음 input을 권장한다.

- `bfs` (graph4k.txt, 기본)
- `sgemm3 -n128` (3×128²×4B ≈ 192 KB)
- `spmv -i $(pwd)/tests/opencl/spmv/Dubcova3.mtx,...vec`

---

## 실험 결과 일지

### Run 1 — 초기 sweep (2026-05-11)

조건: `warps=16, threads=16, DCACHE_NUM_WAYS=8`, 기본 input.
정책: RR / GTO / gCAWS_noCACP / gCAWS_CACP / iPAWS (CACP on)

| Benchmark | RR | GTO | gCAWS_noCACP | gCAWS_CACP | iPAWS | 승자 |
|---|---|---|---|---|---|---|
| bfs        | 1.770 | 1.779 | **1.800** | 1.721 | 1.668 | gCAWS_noCACP |
| sgemm3 -n16 | 1.185 | **1.237** | 1.113 | 1.098 | 1.172 | GTO |
| vecadd -n64 | 0.857 | **0.924** | 0.875 | 0.870 | 0.854 | GTO |
| spmv 1138_bus | 1.725 | **1.766** | 1.717 | 1.639 | 1.637 | GTO |

발견사항:

1. **gCAWS scheduler 단독**은 bfs에서 +1.2%, 다른 워크로드에서는 ≤ GTO.
   CAWA가 가정한 criticality skew가 워크로드 의존적임을 재현.
2. **CACP가 모든 워크로드에서 손해 (−4 ~ −7.5% IPC)**.
   가설: (a) warp 수가 너무 적어 (16 warp × 4 shared way) 비-critical
   partition이 좁아져 conflict miss 폭증, (b) input이 dcache(16KB)보다 작아
   capacity miss 자체가 없어 SHiP-CB가 학습할 reuse 기회 부족.
3. **iPAWS 의사결정 자체는 일부 작동**: sgemm3에서 RR 분기 선택 → gCAWS_noCACP
   1.113 → iPAWS 1.172로 회복. 다만 BFS는 항상 concave로 잡혀 CACP 손해를 그대로 떠안음.

### Run 1 해석 (다른 Claude 리뷰 인용)

> "기법 자체보다 config가 문제일 가능성이 매우 큼." Vortex 기본 config의
> 1 core × 16 warps × 16 threads = 256 threads는 CAWA/iPAWS 논문이 가정한
> SM당 48 warp × 32 thread 환경의 **1/10 규모**. warp 수가 적으면 (1)
> criticality skew의 통계적 신호가 약해지고, (2) CACP의 way reservation 비율은
> 같아도 절대 way 수가 작아 두 partition 모두 buffering 부족.

### Spot 측정 (input 확대 효과, 2026-05-11)

빌드 1회씩만 돌려본 IPC (GTO, CACP off):

| Bench / Input | warps=16 | warps=32 + big input |
|---|---|---|
| sgemm3 (n=16 vs n=128) | 1.24 | **5.72** |
| spmv (1138_bus vs Dubcova3) | 1.77 | **3.20** |

→ 입력 규모만 키워도 IPC가 3~5배 향상. cache 압박이 비로소 발생.

### Run 2 — 재설계된 baseline (2026-05-11)

조건: `warps=32, threads=32, DCACHE_NUM_WAYS=8`
입력: `bfs (graph4k)`, `sgemm3 -n128`, `spmv Dubcova3.mtx`
iPAWS: criticality 기반 WOI 분류기 (구버전; closed-loop 문제 있음)

| Bench | RR | GTO | gCAWS_noCACP | gCAWS_CACP | iPAWS_noCACP | iPAWS_CACP |
|---|---|---|---|---|---|---|
| bfs | **1.358** | 1.345 | 1.300 | 1.269 | 1.309 | 1.309 |
| sgemm3 | **4.238** | 4.210 | 4.129 | 3.959 | 3.479 | 3.479 |
| spmv | 4.294 | 4.290 | **4.340** | 4.274 | 4.218 | 4.218 |

발견:
1. **gCAWS 단독 vs RR**: warps=32에선 BFS/sgemm3에서 RR이 우세. gCAWS는 spmv에서만 +1% IPC. Run 1의 gCAWS 우위가 사라짐 — warp 수 늘리면 RR의 parallelism이 충분.
2. **CACP 모든 워크로드에서 손해** (−1.5~−7.5% IPC). 8-way에서도 50% 예약은 비-critical partition 협소.
3. **iPAWS_noCACP ≡ iPAWS_CACP (소수점 6자리까지 동일)**: criticality-WOI 분류기는 closed-loop 문제로 항상 RR을 선택, 그래서 CACP-on/off 차이가 안 보임.

이 결과를 토대로 다음 결정:
- **CACP는 RTL 타겟에서 제외**.
- **iPAWS 분류기는 paper Algorithm 1 (Adapt 동안 GTO probe, mean/max<0.5)로 재구현**.

### Run 3 — 새 iPAWS 분류기 검증 (2026-05-11)

조건: Run 2와 동일. iPAWS만 새 분류기로 교체. CACP off 통일.
정책 4종: RR / GTO / gCAWS / **iPAWS**(=새 분류기)

| Bench | RR | GTO | gCAWS | **iPAWS** | iPAWS 결정 |
|---|---|---|---|---|---|
| bfs | 1.358 | 1.345 | 1.300 | **1.360** | RR 100% (16/16) — best match ✅ |
| sgemm3 | **4.238** | 4.210 | 4.129 | 4.207 | RR 99% (373/377), gCAWS 1% (4) — near-best ✅ |
| spmv | 4.294 | 4.290 | **4.340** | 4.290 | RR 100% (903/903) — gCAWS 기회 놓침 ❌ |

`IPAWS_STATS` 분석:
- **bfs**: decides=16, valid=5, skipped=11. mm 범위 [0.669, 0.840] — 5번의 valid 모두 convex. 11번은 woi_size<2 (barrier-stalled warp이 WOI에서 제외).
- **sgemm3**: decides=377, valid=341, skipped=36, mm_avg=0.784, mm_min=0.394. 일부 윈도우가 concave 진입 → 작은 gCAWS 비율 (1%).
- **spmv**: decides=903, valid=902, skipped=1, mm_avg=1.000, mm_min=0.794. **분포가 거의 완전 균등** → 항상 RR. 실제로 gCAWS가 spmv에서 이긴 이유는 분포 모양이 아니라 다른 mechanism (예: cache reuse 패턴).

요약: 분류기 정직성은 검증됨. BFS/sgemm3 best 정책 정확히 매치. spmv는 분류기가 잡을 수 있는 신호 자체가 없음.

#### Run 3에서 드러난 후속 이슈

`skipped=11` (BFS) 의 원인: 현재 구현은 `stalled_warps_.test(w)` true (= barrier 대기) 인 warp을 stall 카운트에서 제외함. 그 결과 barrier-heavy 윈도우는 woi_size<2 가 되어 분류 자체가 skip됨. 논문의 `iscore = inst + btime`에 맞게 barrier wait도 stall로 카운트하는 patch가 필요 (Run 4 검증).

---

## 다음 액션 (decided 2026-05-11)

CACP는 RTL 타겟에서 **제외**한다. 다음 두 가지 이유:
- 실험 결과 모든 워크로드에서 손해 (Run 2 표 참조).
- FPGA 면적 비용 (SHCT BRAM + 라인당 메타데이터 + wid/pc 버스 + way partition
  로직) 이 작은 SM 환경에서 정당화되지 않음.

따라서 RTL 타겟은 **gCAWS scheduler + iPAWS{gCAWS ↔ RR} 적응**.

완료:
- **(A)** ✅ iPAWS 분류기를 paper Algorithm 1로 재구현 (commit d1feaf27).
- **(B)** ✅ Run 3 sweep — 새 분류기 검증. BFS/sgemm3 best 매치, spmv 1% 손해.

진행 중:
- **(C)** Barrier wait를 iscore에 포함하는 1줄 patch — Run 4로 검증 예정.
- **(D)** CACP 관련 코드 정리 / LRU 캐시 복귀 (RTL prep). 별도 PR로 진행.

`scripts/exp_cacp_ablation.sh` 는 CACP 드롭 결정 근거로 ablation 보고할 때만 사용.
