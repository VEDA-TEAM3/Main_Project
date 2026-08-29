# Aggregator 성능 지표 (Before / After)

| 항목 | 값 |
| --- | --- |
| 대상 | `TimeWindowAggregatorV2` |
| 측정 일자 | 2026-07-29 |
| 측정 방식 | **실측** (추정 아님) — 전역 `operator new` 계측 + `steady_clock` 샘플링 |
| 측정 환경 | x86_64 / WSL2 / g++ `-O2` / 단일 스레드 |
| Before | 리팩터 이전 V2 로직을 동일 파일에 재현 |
| After | 프로덕션 `TimeWindowAggregatorV2.cpp` 를 그대로 링크 |

> **측정 환경 주의**: 라즈베리파이 4(ARM64)가 아니라 x86 개발 장비 수치다.
> 절대값은 Pi 에서 3~5배 커지지만, **할당 횟수는 하드웨어와 무관하게 동일**하다.
> Pi 실측치는 `performance/control-server.md` 에 별도 갱신 필요.

---

## 측정 조건

| 파라미터 | 값 | 근거 |
| --- | --- | --- |
| 채널 수 | 12 | Pi 1대 최대 (CCTV 3대 × 4채널) |
| 프레임당 객체 수 | 20 | 현장 전형값 |
| 윈도우 크기 | 100 ms | `config.json` 기본값 |
| 측정 윈도우 | 2,000회 (= 200초 분량) | |
| 총 `push()` 호출 | 24,000회 | |
| warmup | 200 윈도우 (측정 제외) | 풀/버퍼 capacity 안정화 |

---

## 우선순위 2 — 할당 오버헤드 (핵심 성과)

| 지표 | Before | After | 변화 |
| --- | --- | --- | --- |
| 총 힙 할당 (24,000 push) | **26,002회** | **1회** | **−99.996%** |
| push 1회당 | 1.08회 | 0.00회 | — |
| **윈도우 1회당** | **13.0회** | **0.0회** | **−13.0** |
| 초당 (10Hz 기준) | 130회/s | 0회/s | — |

### Before 의 13회 내역

| 발생원 | 횟수/윈도우 |
| --- | --- |
| `optional::reset()` 후 다음 push 의 `objects` 재할당 | 12 (채널당 1) |
| `std::vector<TopViewFrame> flushedFrames` 외곽 벡터 | 1 |

### After 가 0인 이유

- 슬롯 `objects` 버퍼를 `swap` 으로 회전 — 해제 자체가 없다
- 마감 묶음은 `FrameBufferPool` 에서 대여/반납
- `activeChannels_` 는 생성 시 `reserve(channelCount)` — `push_back` 재할당 없음

잔여 1회는 warmup 직후 첫 윈도우의 잔여분이며 이후 정상상태에서 0으로 유지된다.

### 메모리 상주량

| | Before | After |
| --- | --- | --- |
| 상주 버퍼 수 | 가변 (할당/해제 반복) | `channelCount × (1 + poolSize)` 로 **상한 고정** |
| 12채널 기준 | — | 60개 버퍼 |
| 단편화 위험 | 초당 130회 할당/해제로 누적 | 없음 (해제가 일어나지 않음) |

---

## 우선순위 1 — 지연

| 지표 | Before | After | 변화 |
| --- | --- | --- | --- |
| `push()` p50 | 0.06 µs | 0.10 µs | **+0.04 µs (악화)** |
| `push()` p99 | 0.46 µs | **0.22 µs** | **−52%** |
| `push()` 최악 | 38.99 µs | 36.74 µs | −6% |

### p50 이 악화된 이유 (숨기지 않음)

정상 push 경로에 부기(bookkeeping)가 늘었다.

- `occupied_[ch]` 조회 + `activeChannels_.push_back()` — 별도 캐시 라인 접근
- `objects.assign()` + 스칼라 3개 대입 (이전에는 `optional` 대입 1회)

약 40 ns 이며, **의도적으로 치른 비용**이다. 그 대가로:

- **p99 가 절반**이 되었다. 이전 p99 0.46 µs 의 꼬리는 대부분 마감 윈도우의 13회 할당이었다.
  실시간 시스템에서 의미 있는 값은 중앙값이 아니라 꼬리다.
- **마감 윈도우가 더 이상 튀지 않는다.** 부담이 모든 push 에 균등하게 퍼졌다 — 지터 감소.

### 마이크로벤치가 보여주지 못하는 이득

단일 스레드 측정이라 다음 두 가지는 수치에 나타나지 않으며, 실환경에서 더 크다.

1. **할당자 락 경합 제거.** 집계기는 mosquitto 네트워크 스레드와 같은 힙을 쓴다.
   초당 130회의 `malloc`/`free` 가 사라지면서 수신 경로로 새던 지연이 없어진다.
2. **장기 가동 시 힙 단편화 제거.** 수개월 무중단에서 RSS 증가 요인 하나가 사라졌다.

### 부수 개선

- `steady_clock::now()` 호출을 push 당 **3회 → 2회**로 축소
  (`buildMetricsReportIfDue` 가 이미 측정된 `lockEnd` 를 인자로 받음)
- 마감 시 순회가 `channelCount` 전체 스캔 → `activeChannels_` 크기로 축소.
  12채널에서는 차이가 없지만 **`channelCount = 256`** 설정에서는 빈 슬롯 244개를
  훑지 않는다 (O(channelCount) → O(활성 채널))

---

## 재현

```bash
g++ -std=c++20 -O2 -I control-server/include -I control-server/src -I shared agg_bench.cpp control-server/src/aggregate/TimeWindowAggregatorV2.cpp -o aggbench -lpthread && ./aggbench
```

원본 출력:

```
설정: 12채널 x 객체 20개, 윈도우 100ms, 2000윈도우 (push 24000회)

[Before] 할당 26002회 (= 1.08회/push, 13.0회/윈도우)
[Before] push() p50 0.06us  p99 0.46us  최악 38.99us

[After ] 할당 1회 (= 0.00회/push, 0.0회/윈도우)
[After ] push() p50 0.10us  p99 0.22us  최악 36.74us
```

---

## 런타임 지표

리팩터 후 5초 주기 로그에 드롭·풀 고갈이 추가되었다.

```
Aggregator - 최근 5000ms 지표 - push() N회, 윈도우 마감 M회, 평균 락 보유시간 X.XXus, 드롭 D건, 풀 고갈 E회
```

| 필드 | 정상값 | 벗어나면 |
| --- | --- | --- |
| 평균 락 보유시간 | < 1 µs | 콜백이 락 안으로 새어 들어갔는지 확인 |
| 드롭 | 0 | 채널 ID 오설정 또는 객체 수 상한 초과 (보안 감사 A1/A5) |
| 풀 고갈 | 0 | `kFlushBufferPoolSize` 증설 필요 |
