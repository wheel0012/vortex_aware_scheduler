# URP: Vortex GPGPU 에 CAWA + iPAWS 적응형 스케줄러 적용

> Vortex 본체 README 와 분리된 URP 프로젝트 작업 일지.

## 무엇을 만들었는가

simx 에뮬레이터 위에서 **상황에 따라 RR 과 gCAWS 사이를 자동 전환하는 warp 스케줄러** 를 구현했다.

기반 논문 두 편:

- **CAWA** (Lee & Wu, ISCA 2015) — `gCAWS`: 가장 critical 한 warp (즉, 가장 뒤처졌고 stall 많이 한 warp) 을 우선 issue. Criticality 공식: `nInst * CPI_avg + nStall`.
- **iPAWS** (Lee et al., 2016) — 워크로드의 **instruction-issue pattern** 을 짧게 probe 해서 분포가 `concave` 면 gCAWS, `convex` 면 RR 을 선택. 짧은 Adapt phase → Decide → 긴 Execute phase.

최종 코드는 두 정책을 묶어 **gCAWS ↔ RR 적응형** 으로 동작한다.

---

## 논문과 Vortex 환경의 차이, 그에 따른 결정

논문은 GPGPU-sim 환경 + GTX480 모델 기준이고, 우리는 Vortex simx + FPGA 타겟. 그래서 그대로 옮기면 안 되는 부분들이 있었고, 그때마다 "왜 어긋나는지" 와 "어떻게 우회했는지" 를 기록해둔다.

### 1. CACP (CAWA 의 cache 관리) 는 빼기로 결정

CACP 는 critical warp 의 dcache way 일부를 예약 + SHiP-CB 기반 RRIP 교체 정책.

**문제**: 모든 워크로드에서 IPC 손해 (−1.5% ~ −7.5%). 8-way dcache 에서 50% way 예약 하면 non-critical partition 이 너무 협소해져 conflict miss 폭증. SHCT 학습할 reuse 기회도 부족.

**결정**: RTL 타겟에서 제외. 이유는 두 가지:
- 실험적으로 손해.
- FPGA 면적 (SHCT BRAM + 라인당 메타데이터 + wid/pc 버스 + way partition 로직) 이 작은 SM 환경에서 정당화 안 됨.

코드는 `VORTEX_CACP_ENABLE` 빌드 토글로 남아있지만 기본 OFF.

### 2. iPAWS WOI 필터 — paper 가정이 Vortex 와 구조적으로 어긋남

iPAWS 논문 §3.2 (Figure 8) 의 WOI (Warps-of-Interest) 필터: BLK 같은 케이스에서 "issue 기회를 한 번도 못 받은 newer warp" 을 분류 metric 에서 제외하는 메커니즘. 핵심 가정 — **newer warp 일수록 instruction-issue stall count 가 monotonic 하게 증가** (Figure 8(b)).

**문제**: 우리 환경에선 이 가정이 깨진다.

| 가정 (iPAWS paper) | 실제 (Vortex simx) |
|---|---|
| **Birth-order GTO** — warp 생성 순서로 oldest 정의. Stall 해도 우선순위 유지. | **Recency-based GTO** — `ready_timestamps_` 가 작을수록 oldest. Warp 이 stall 하면 timestamp 가 0 으로 리셋되고, 다시 ready 될 때 현재 cycle 로 stamp 됨. 즉 **stall 자주 하는 warp 가 newest 로 강등됨**. |
| **시간차 warp dispatch** — warp 들이 점진적으로 도착 | **동시 wspawn** — `active_warps_.count()==1` 일 때 한 cycle 안에 32 warps 가 한꺼번에 activation. Birth-order 정보 자체가 없음. |

BFS 로 측정해보면 warp 0 의 stall count 가 다른 warp 들보다 오히려 크다 (BFS frontier 진입점이라 일찍 stall 시작 → 영구적으로 newest 로 강등). Figure 8(b) 와 정반대. 그러면 `w* = argmax(stall)` 이 warp 0 이 되고 `WOI = {w : w_id ≤ w*}` 가 `{0}` 한 개로 줄어 분류 자체가 invalid.

**결정**: 코드는 **paper-strict 그대로 두되**, 이 필터가 본 환경에선 사실상 발동 안 한다는 점을 명시. Future work — birth-order GTO + 시간차 CTA dispatch 환경 (GPGPU-sim 등) 에서 재평가.


### 3. iPAWS Adapt 트리거 — periodic 대신 kernel boundary

논문은 "first warp 이 끝날 때까지" Adapt 를 돌리고, 이후 Execute. 우리는 처음에 단순화해서 **periodic 1024 Adapt + 16384 Execute** 사이클로 구현했다.

**문제**: 같은 워크로드 안에서 분류 결과가 거의 항상 동일하게 나옴 (BFS valid decide 5번 모두 RR). 주기적 재평가의 reactivity 가 살아나지 않고 Adapt phase 오버헤드만 더해짐.

**결정**: **wspawn (kernel launch) 마다 Adapt 한 번**, 그 뒤 다음 wspawn 까지 Execute 무한 지속. Execute phase 의 cycle 카운터 자체를 제거 — periodic 재트리거가 의도치 않게 되살아나지 않게.

논문의 "first warp finish" 마커는 Vortex 에 정확히 매핑되지 않지만, wspawn 은 동일한 의미 (= kernel launch boundary) 를 가진다. ADAPT_CYCLES 는 1024 → **4096** 으로 증가 (분포 신호 안정화 위해).

### 4. iPAWS btime — barrier-only 로 분리

논문 §3.3: iscore = inst + btime, 여기서 `btime` 은 **barrier wait time** 만 의미. Scoreboard stall 은 포함 안 함.

**우리 첫 구현 문제**: `adapt_stall[w]++` 를 "active && != chosen" 모두에 대해 ticked. Scoreboard stall + barrier stall 둘 다 카운트.

**결정**: Helper `is_barrier_stalled(wid)` 추가 (warp 이 `barriers_[i]` bitmask 중 어느 하나에라도 포함되면 true). Adapt 의 stall 누적은 barrier-stalled 인 warp 에만 적용.

빌드 토글 `VORTEX_IPAWS_BARRIER_ONLY_BTIME=1` 기본. 0 으로 두면 이전(완화) 동작.

### 5. Recover phase — 본 환경에서 사실상 무용지물, 기본 비활성

논문 §3.4: Convex (RR) 결정 후 RR 진입 전에 **Recover phase** 가 있다. Adapt 동안 GTO probe 로 warp 들의 instr_count 가 skewed (older warp 이 많이 issue, newer 적게). RR 의 inter-warp locality 효과를 보려면 먼저 균형 잡아야 함. Recover scheduler = `argmin(instr_count)` 우선.

**논문 strict 종료 조건**: "newest warp 이 oldest warp 만큼 issue 할 때까지" — 즉 `min(instr_count) >= recover_target` (recover_target = Recover 시작 시점의 max).

코드는 paper-faithful 하게 (a) strict 조건 + (b) relative 완화 조건 + (c) cycle cap 으로 구현했지만, 측정 결과 이 환경에서 사실상 작동 안 함. 데이터 근거 아래.

#### Step 1 — Recover entry 분포 측정

`IPAWS_DBG_RECOVER_ENTRY` 로 Convex 결정 직후 (Recover 진입 시점) per-warp `instr_count` 분포 덤프. 각 워크로드 2 개 decide (boot kernel + user kernel) 기록.

| Workload | Entry | cycle | max (wid) | min (wid) | mean | stddev | range | range/mean | range/max |
|---|---|---|---|---|---|---|---|---|---|
| BFS | 1 (boot) | 4843 | 39 (w0) | 30 (w1) | 30.3 | 1.6 | 9 | 0.297 | 0.231 |
| BFS | 2 (user) | 149578 | **445 (w0)** | 188 (w17) | 197.7 | 44.4 | **257** | **1.300** | **0.578** |
| sgemm3 | 1 (boot) | 4843 | 38 (w0) | 29 (w1) | 29.3 | 1.6 | 9 | 0.307 | 0.237 |
| sgemm3 | 2 (user) | 152369 | **441 (w0)** | 191 (w1) | 199.8 | 43.3 | **250** | **1.252** | **0.567** |
| spmv | 1 (boot) | 4817 | 38 (w0) | 29 (w1) | 29.3 | 1.6 | 9 | 0.307 | 0.237 |
| spmv | 2 (user) | 148596 | **422 (w0)** | 188 (w1) | 195.3 | 40.7 | **234** | **1.198** | **0.555** |

**관찰**:

- **Paper 의 skew 가정은 instr_count 측면에선 유지됨**: max 가 항상 w0 (oldest). Recover 의 least-issued-first 정책 자체는 잘못된 방향이 아님.
- **하지만 range 가 mean 보다 큼 (range/mean ≈ 1.2~1.3)** — skew 가 매우 가파름. Recover 가 따라잡아야 할 양이 큼.
- **range/max ≈ 0.55~0.58** — 거의 동일하게 모든 워크로드에서.

#### Step 2 — 종료 조건 옵션 비교

BFS user entry (range 257, max 445, mean 198) 기준:

| 옵션 | 조건 | 결과 |
|---|---|---|
| 절대 임계 4 | `range ≤ 4` | 도달 불가 → cycle cap (8192) 으로 종료 |
| A. relative 0.1 | `range ≤ max × 0.1 = 44` | 도달 불가 → cycle cap |
| A'. relative 0.3 | `range ≤ max × 0.3 = 134` | 도달 불가 → cycle cap |
| A''. relative 0.6 | `range ≤ max × 0.6 = 267` | 즉시 종료 (range 257 < 267) — **Recover 무용지물** |
| B. 절대+상대 OR | `range ≤ max(4, mean × 0.1) = 20` | 도달 불가 → cycle cap |
| C. paper-strict (a) | `min ≥ recover_target = 445` | 8192 cycle 안에 도달 불가 → cycle cap |

#### 왜 strict 조건이 도달 못 하는가

이론적으로 Recover scheduler 는 매 cycle min-warp 한 개 픽 → 1 cycle 당 min 이 1 씩 증가 → range 257 닫는데 ~257 cycle 이면 충분해야 함.

**실제로 안 되는 이유**: min-warp 도 memory miss / scoreboard stall 자주 발생. Recover 가 픽 하려 해도 ready 가 아니라 그냥 idle. 즉 Vortex 의 dcache miss penalty (160+ cycle memory latency) 가 paper 환경보다 (상대적으로) 커서 newest 가 진도 못 나감.

이건 architectural difference — paper 환경 (GPGPU-sim + GTX480 + 큰 L1/L2) 보다 우리 dcache (single-bank, 8-way) 가 contention 심함. WOI 필터 미적용과 같은 맥락.

#### 결정: 기본 비활성

- 빌드 토글 `VORTEX_IPAWS_USE_RECOVER=0` 기본.
- 코드는 그대로 유지 (USE_RECOVER=1 로 빌드 가능, 위 ablation 재현 가능).
- 측정 결과 Recover-on/off 의 IPC 차이는 0.1% 미만 (Recover 가 cycle cap 으로만 동작해 실질적 정책 변화 없음).
- WOI 필터 미적용과 동일 narrative: paper 의도 자체는 옳으나 Vortex architecture 와 호환되지 않음.

**Future work**: 더 큰 dcache + 짧은 memory latency (예: shared memory 활용 워크로드) 환경에서 Recover 효과 재평가.

---

## 코드 맵

| 컴포넌트 | 위치 |
|---|---|
| gCAWS scheduler | `Emulator::select_gcaws_warp()` — criticality argmax + oldest-ready tie-break |
| Criticality 갱신 | `Emulator::update_cpl_counters()` — `nInst * CPI_avg + nStall` |
| iPAWS 상태 기계 | `select_ipaws_warp()` / `ipaws_sample_and_step()` |
| iPAWS Adapt | GTO probe + per-warp inst/btime 카운터 |
| iPAWS Decide | Algorithm 1: `iscore_sum < |WOI| × iscore_max / 2` |
| WOI stall-rank 필터 | `issue_stall_count[]` 누적, w*=argmax, WOI={w:w_id≤w*} (Vortex 에선 비활성) |
| Recover phase | `select_recover_warp()` (argmin instr_count) + 3 종료 조건 (기본 비활성) |
| Kernel-boundary 트리거 | `step()` 의 wspawn 디스패치 직후 phase=Adapt 강제 |
| Barrier 식별 helper | `is_barrier_stalled(wid)` — `barriers_[]` bitmask 검사 |
| CACP (off) | `cache_sim.{h,cpp}` SHCT + RRIP + way partition |

전부 `sim/simx/emulator.{h,cpp}` + `sim/simx/cache_sim.{h,cpp}` 안에.

## Build-time 옵션

| 매크로 | 기본 | 설명 |
|---|---|---|
| `VORTEX_SCHED` | 4 | 0=Static / 1=GTO / 2=RR / 3=gCAWS / 4=iPAWS |
| `VORTEX_CACP_ENABLE` | 1 | dcache CACP 전역 토글 (RTL 타겟에선 OFF 권장) |
| `VORTEX_CACP_RESERVED` | A/2 | CACP 가 critical warp 용으로 예약하는 way 개수 |
| `VORTEX_IPAWS_USE_CACP` | 1 | 0 이면 iPAWS 의 gCAWS 분기에서도 CACP off |
| `VORTEX_IPAWS_ADAPT_CYCLES` | 4096 | Adapt phase 길이 (cycle) |
| `VORTEX_IPAWS_BARRIER_ONLY_BTIME` | 1 | btime 을 barrier wait 만 카운트 (논문 strict) |
| `VORTEX_IPAWS_USE_RECOVER` | 0 | Convex 결정 후 Recover phase 실행 (본 환경에선 무용지물 → 비활성) |
| `VORTEX_IPAWS_RECOVER_THRESHOLD` | 4 | (Recover on 일 때) 종료 조건 — max−min ≤ 임계 |
| `VORTEX_IPAWS_RECOVER_MAX_CYCLES` | 8192 | (Recover on 일 때) cycle cap |

Adapt 트리거: wspawn (kernel launch) 마다 Adapt 강제 시작 → ADAPT_CYCLES 동안 GTO probe → Decide → Execute 무한 지속 (다음 wspawn 까지). Periodic re-trigger 없음.

## 실험 인프라

```
worklogs/
├── scripts/
│   ├── bench_sweep.sh          # RR / GTO / gCAWS / iPAWS 4 정책 스윕
│   └── exp_cacp_ablation.sh    # CACP way 0~4 ablation (보고용)
└── runs/                        # 모든 timestamped 결과
```

기본 BASE config: `cores=1, warps=32, threads=32, DCACHE_NUM_WAYS=8`.

권장 워크로드 / input:
- `bfs` (graph4k)
- `sgemm3 -n128` (3 × 128² × 4B ≈ 192 KB)
- `spmv` Dubcova3 sparse matrix
- `vecadd`

작은 input + warps=16 으로는 cache 압박이 없어 정책 차이가 묻힘. warps=32 + big input 으로 가야 IPC 신호가 잡힌다.

## 다음 액션

- 최종 sweep 결과 정리 + 발표 자료 작성.
- RR-friendly 워크로드 추가 (stencil/gaussian/lbm 후보) — RR 적응 능력을 다양한 케이스로 검증.
- RTL 타겟 준비 — gCAWS scheduler + iPAWS{gCAWS ↔ RR}. CACP 관련 코드는 빌드 토글로 비활성, LRU 캐시로 복귀.
