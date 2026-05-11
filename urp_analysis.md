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

- `worklogs/bench_sweep.sh "<bench> [args...]" ...` : 6개 정책 (RR / GTO /
  gCAWS_noCACP / gCAWS_CACP / **iPAWS_noCACP** / iPAWS_CACP) 으로 한 번씩 빌드 후
  주어진 벤치마크들을 돌리고 결과를 summary로 정리. 워크로드별 입력 인자는 인자로 전달.
- `worklogs/exp_E_cacp_reserved.sh` : gCAWS 고정, CACP 예약 way 0/1/2/3/4 스윕.
- `worklogs/exp_F_ipaws_threshold.sh` : iPAWS 고정, concave threshold 0.30~0.75 스윕.

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

### Run 2 — 재설계된 baseline (진행 중)

조건: `warps=32, threads=32, DCACHE_NUM_WAYS=8`
입력: `bfs (graph4k)`, `sgemm3 -n128`, `spmv Dubcova3.mtx`
정책 6종: RR / GTO / gCAWS_noCACP / gCAWS_CACP / **iPAWS_noCACP** / iPAWS_CACP

> iPAWS_noCACP는 본 프로젝트가 새로 추가한 `VORTEX_IPAWS_USE_CACP=0` 빌드 모드.
> gCAWS 분기에서도 critical_warp 정보를 dcache로 push하지 않아 (Emulator의
> `suppress_critical_push_` 플래그가 select_gcaws_warp의 propagate 호출을 차단)
> iPAWS의 분류기 자체의 효과만 평가한다.

결과는 sweep 완료 후 본 섹션에 채울 예정.

---

## 다음 액션 (P0/P1/P2)

- **(P0)** 입력 규모 + warp 수 확대 — Run 2에서 검증 중. 끝나면 결과를 위에 패치.
- **(P1)** iPAWS = gCAWS_noCACP ↔ RR 분리 평가 — Run 2의 `iPAWS_noCACP` 항목.
  BFS에서 1.800 근처 (≈ gCAWS_noCACP) 가 나와야 분류기가 제대로 작동했다는 신호.
- **(P2)** CACP 살릴 길 찾기:
  - reservation way 수 스윕 (`exp_E_cacp_reserved.sh`).
  - 가설: warp/way 비율이 충분히 커지면 (Run 2 환경) 50% reservation도
    더 이상 conflict miss를 폭증시키지 않을 수 있음.
- **(P3)** iPAWS concave threshold 튜닝 (`exp_F_ipaws_threshold.sh`).
  BFS가 RR로 분류되도록 임계를 올리는 게 도움이 되는지 검증.
