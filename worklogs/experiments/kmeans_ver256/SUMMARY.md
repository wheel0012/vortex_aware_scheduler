# kmeans @ WG=256 — coalescer 개선 전후 비교

**Workload**: kmeans `-f100 -p1000` (working set ~400KB, L1=16KB → ~25× thrash)
**WG size**: 256 (= 8 warps per work-group, NUM_WARPS=32 의 1/4)
**Branch**: `feat/gcaws` (warp-wide coalescer 적용 후)

## Sim configure

| Param | Value | Note |
|---|---|---|
| NUM_CORES | 1 | |
| NUM_WARPS | 32 | per core |
| NUM_THREADS | 32 | per warp (SIMT lane) |
| L1 dcache (`DCACHE_SIZE`) | 16 KB | 4-way, 2 banks |
| `DCACHE_NUM_REQS` | 2 | LSU lanes per cycle into dcache |
| `DCACHE_NUM_BANKS` | 2 | |
| `DCACHE_NUM_WAYS` | 4 | |
| L1 icache (`ICACHE_SIZE`) | 16 KB | |
| L2 cache (`L2_CACHE_SIZE`) | 1024 KB | 8-way, enabled |
| Coalescer | `output_size=DCACHE_NUM_REQS=2` | warp-wide (after fix); paper-era Fermi-like |
| MSHR depth | upstream default | sim's in-flight queue (not real MSHR) |
| Memory model | Ramulator + DRAM (~108c latency) | |
| Build flags | `-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0` | CACP off, scheduler-only experiment |
| Scheduler | `VORTEX_SCHED ∈ {1=GTO, 2=RR, 3=gCAWS, 4=iPAWS}` | per-build via CONFIGS |
| perf class | 1=CORE (pipeline) + 2=MEM (cache) | separate runs, results below |

### Run command
```bash
cd build && CONFIGS="-DPERF_ENABLE -DVORTEX_CACP_ENABLE=0 -DVORTEX_IPAWS_USE_CACP=0 -DVORTEX_SCHED=<N>" \
  ./ci/blackbox.sh --driver=simx --app=kmeans --cores=1 --warps=32 --threads=32 \
  --l2cache --perf={1,2} --args="-f100 -p1000"
```

## 1. 변경 내역

### Coalescer (`sim/simx/mem_coalescer.cpp`)
- **BEFORE (upstream Vortex slot-limited)**: thread `i`와 `j`가 같은 cache line이라도 다른 output slot (`i / output_ratio != j / output_ratio`)이면 별개 transaction.
- **AFTER (warp-wide, paper Fermi-like)**: 32 thread 전체에서 같은 line address 인 thread들을 slot 제약 없이 묶어서 하나의 cache transaction으로 처리.

### Coalescer metric (`runtime/stub/utils.cpp`)
- **upstream 원본**: `(req-miss)/req`, 라벨 "hit ratio" — 단위 혼합 + uint64 underflow → `-2147483648%` 출력 버그.
- **1차 fix (clamp)**: `max(0, req-miss)/req`, 라벨 "coalesce ratio" — underflow는 잡았지만 분모 의미 모호.
- **2차 fix (현재)**: `misses / cycles`, 라벨 **"split rate"** — 의미 명확. **낮을수록 좋음** (input이 1 cycle 안에 fully coalesce 됐다는 뜻).

## 2. perf=2 (CLASS_MEM — cache stats)

| Policy | IPC | cycles | dcache_reads | L1_hit | dc_read_miss | bank_stall | mshr_stall | coal_split |
|---|---|---|---|---|---|---|---|---|
| **BEFORE (slot-limited)** | | | | | | | | |
| RR | 6.630 | 660,066 | 97,051 | 35% | 62,212 | 17% | 14% | (broken: 0%) |
| GTO | 6.623 | 660,792 | 97,049 | 37% | 60,637 | 17% | 13% | (broken: 0%) |
| gCAWS | 6.557 | 667,456 | 97,017 | 35% | 62,490 | 16% | 14% | (broken: 0%) |
| iPAWS | 6.606 | 662,515 | 97,334 | 35% | 63,081 | 17% | 14% | (broken: 0%) |
| **AFTER (warp-wide)** | | | | | | | | |
| RR | 6.719 | 651,277 | **72,762** | 27% | 52,964 | **11%** | 15% | 23% |
| GTO | **6.800** | 643,499 | **72,762** | 24% | 54,928 | **11%** | 13% | 24% |
| gCAWS | 6.702 | 652,972 | **73,084** | 25% | 54,782 | **11%** | 15% | 23% |
| iPAWS | 6.686 | 654,544 | **73,082** | 26% | 53,612 | **11%** | 15% | 23% |

### Speedup_RR (IPC_policy / IPC_RR)

| Policy | BEFORE | AFTER | Δ |
|---|---|---|---|
| RR | 1.000 | 1.000 | — |
| GTO | 0.999 | **1.012** (+1.2%) | scheduler 우위 확립 |
| gCAWS | 0.989 | 0.997 | RR 격차 좁힘 |
| iPAWS | 0.996 | 0.995 | 거의 동일 |

### Coalescer 효과 (절대값 변화)
- **dcache_reads: 97K → 73K (−25%)** ★ 같은 line이 묶여서 cache 요청 수 자체 감소
- **bank stall rate: 17% → 11% (−6pp)** ★ 묶인 요청은 같은 bank에 한 번
- **IPC: +1.3 ~ +2.7%** (cycle 감소 효과)
- 정확성 보존: kmeans clustering output BEFORE/AFTER **identical** ✓

## 3. perf=1 (CLASS_CORE — pipeline stats)

| Policy | IPC | sched_idle | ibuf_stall | scrb_stall (lsu / fpu) | opds_stall |
|---|---|---|---|---|---|
| RR | 6.719 | 78% | 42% | 81% (lsu=97%, fpu=2%) | 2% |
| GTO | 6.800 | 78% | **47%** | 79% (lsu=97%, fpu=2%) | 2% |
| gCAWS | 6.702 | 78% | 42% | **84%** (lsu=97%, fpu=2%) | 2% |
| iPAWS | 6.686 | 78% | 41% | 80% (lsu=97%, fpu=2%) | 2% |

**관찰**:
- **scoreboard LSU stall = 97%** 모든 정책 동일 — 진짜 bottleneck은 **load latency 대기**. cache hit 차이 (24~27%)가 그대로 stall 시간에 누적.
- scheduler_idle 78% — 매 cycle 중 28%는 issue할 ready warp이 없음.
- **GTO 의 ibuf_stall 47%** (다른 정책 41~42%) — greedy stick 으로 instruction fetch 패턴이 한 warp에 집중 → ibuffer 회전 빨라짐.
- **gCAWS 의 scrb 84%** (다른 정책 79~81%) — critical warp 우선 처리하면서 LSU stall 더 누적. 우리 환경에서 gCAWS의 churn이 오히려 scrb 압력 증가시킴.

## 4. **핵심 — Scheduler 효과를 보려면 무엇이 필요한가**

### 본 실험으로 확인된 것
1. **Coalescer 개선만으로는 scheduler 차이 작음** (1~2%). dcache_reads 25% 감소했어도 4 정책 IPC 차이는 1.2%p 이내.
2. **GTO가 RR보다 +1.2%** 우위로 분명해짐 — paper의 GTO 우위 약하게 재현. **그러나 gCAWS는 여전히 RR 수준**, iPAWS도 RR과 동률.

### 본 환경(1-core, NUM_WARPS=32, L1=16KB single bank)의 fundamental limit
| 필요 조건 | Paper Fermi GTX480 | 우리 Vortex | 차이 |
|---|---|---|---|
| 동시 active warp pool | 720 (15 SMs × 48) | 32 | 22× |
| L1 bank 수 | 32 banks | 2 banks | 16× |
| 동시 active warp / WG | up to 32 | up to 8 (WG=256) | 4× |
| Warp criticality variance | branch divergent 워크로드 풍부 | **kmeans는 균등 (variance ≈ 0)** | — |

### Scheduler 효과 발현 조건 (체크리스트)
- [x] L1 thrashing 영역 진입 (working set 3~25× L1) — `-f100 -p1000` 만족
- [x] coalescer가 paper 수준 — warp-wide fix 적용
- [ ] **워크로드의 warp-criticality variance 존재** ← **kmeans는 부재 (모든 thread 동일 연산)**
- [ ] **active warp pool ≥ 16~32 warps** ← WG=256 에서 8 warps 정도. 부족.
- [ ] **bank 수 ≥ 4** ← 우리 2 banks. bank conflict가 scheduler 무관 deterministic.
- [ ] **DRAM 안 saturated** — 만족 (mem latency 108c)

### 결론 — paper의 3.13× kmeans 효과를 본 환경에서 보기 위한 필요 조건
1. **워크로드 다양화**: bfs / b+tree / nw 같은 **inherent warp imbalance 가 있는 워크로드** 추가. kmeans는 본질적으로 critical warp 개념 성립 안 함.
2. **아키텍처 강화**: `DCACHE_NUM_BANKS=4` 이상 + NUM_WARPS 늘리기 (paper처럼 1 SM 자체를 paper와 동등하게 맞춤).
3. **coalescer + bank 동시 개선**: 본 turn coalescer만 고쳤지만, bank가 2개라 critical-warp acceleration 효과가 묶인다.
4. **gCAWS stick (greedy time slice)**: 본 turn 미적용. 적용하면 paper §3.2 의 "larger time slice for critical warp" 효과 발현 가능.

**현재 단계 한 줄 결론**:
> warp-wide coalescer 적용 → cache traffic −25%, IPC +1~3% 향상, **GTO scheduler 우위 미세 재현** (+1.2%). gCAWS/iPAWS의 paper-level 효과(2.6×, 3.13×)는 워크로드 자체의 warp-criticality variance 부재 + bank/warp pool 부족으로 발현 안 됨. 다음 step은 워크로드 다양화 + bank 수 증가 + gCAWS stick 적용.

## 5. 산출물

- `RR/`, `GTO/`, `gCAWS/`, `iPAWS/` — 각 정책 build.log + kmeans.perf1.log + kmeans.perf2.log
- BEFORE 데이터: `/tmp/before_ver256/{pol}.log` (slot-limited, 비교용 보관)
- 관련 commit:
  - `feat/gcaws` (base): coalescer 수정 전 sweep 결과
  - `feat/warp-wide-coalescer`: warp-wide coalescer 적용
