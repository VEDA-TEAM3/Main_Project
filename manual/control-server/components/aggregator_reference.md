# Aggregator 레퍼런스

> **대상 파일**
> - 집계 포트: `include/interfaces/IFrameAggregator.h`
> - 현재 구현: `src/aggregate/TimeWindowAggregatorV2.h`, `.cpp`
> - 버퍼 풀: `src/aggregate/FrameBufferPool.h`

Aggregator는 Control Server 파이프라인의 **입력 정렬 계층(input alignment layer)** 이다.
채널마다 제각각 도착하는 `TopViewFrame` 을 시간 윈도우 단위로 묶어, 다운스트림이
"같은 시점의 전 채널 상태"를 한 번에 볼 수 있게 만든다.

집계기는 좌표계도, 융합 규칙도, 위험 판정도 알지 못한다. 아는 것은 **시간**과 **채널 번호**
두 가지뿐이다. 월드 좌표 변환·교차 채널 융합·존 배정은 모두 콜백 이후 단계의 책임이다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-30 | 1.0.0 | Mangjun | Aggregator를 무할당 집계 포트로 재정의하고 버퍼 풀·콜백 수명 계약 문서화 |

---

## 1. 집계 포트 계약

```cpp
class IFrameAggregator {
public:
    using AggregatedFrames    = std::vector<veda::TopViewFrame>;
    using AggregationCallback = std::function<void(const AggregatedFrames&)>;

    virtual void setCallback(AggregationCallback callback) = 0;
    virtual void push(const veda::TopViewFrame& frame) = 0;
};
```

`IFrameAggregator` 가 정의하는 것은 **주입(`push`)과 완성 통지(`callback`)** 뿐이다.
윈도우 크기, 중복 처리 정책, 버퍼 재사용 방식은 구현체의 세부사항이다.

| 계약 | 내용 |
|------|------|
| 입력 타입 | `veda::TopViewFrame` (채널 로컬 좌표) |
| 호출 주체 | Receiver의 파이프라인 스레드 |
| 입력 수명 | `push()` 호출 동안만 유효 — 구현체가 필요한 만큼 복사 |
| 출력 수명 | **콜백 반환 시점까지만 유효한 borrowed reference** |
| 스레드 안전 | `push()`는 다중 스레드에서 호출 가능 |
| 콜백 실행 위치 | 구현체의 락 **밖** — 다운스트림이 다른 채널의 `push()`를 막지 않아야 함 |
| 등록 시점 | `setCallback()` 은 첫 `push()` 이전에 완료 |

### 1.1 ⚠️ 출력 수명 — 빌려주는 버퍼

`AggregationCallback` 은 값이 아니라 **`const` 참조**로 받는다.

> **콜백이 받은 참조를 저장하거나 콜백 밖으로 넘기면 안 된다.**

버퍼는 `FrameBufferPool` 에서 빌려 온 것이고, 콜백이 반환하는 즉시 반납되어 다음 윈도우에
덮어써진다. 보관이 필요하면 **콜백 안에서 복사**해야 한다.

이 계약이 윈도우마다 새 벡터를 만들지 않게 해 주는 근거다. 위반해도 컴파일 오류가 나지
않으므로, 새 소비자를 붙일 때 반드시 확인한다.

## 2. 집계 정책

**채널당 이번 윈도우의 최신 프레임 하나만 유지한다.**

`TopViewFrame` 은 델타가 아니라 **전체 상태 스냅샷**이다. 같은 채널의 프레임 N+1 은 N 을
완전히 대체하므로, 쌓아 두면 지연만 늘고 정보는 늘지 않는다.

| 상황 | 동작 |
|------|------|
| 같은 채널이 윈도우 내 여러 번 도착 | 최신 것으로 덮어씀 (이전 프레임 폐기) |
| 어떤 채널도 도착하지 않음 | 윈도우가 마감되지 않음 (아래 2.1) |
| 일부 채널만 도착 | 도착한 채널만 묶어서 전달 |
| 콜백 미등록 상태로 마감 | 해당 윈도우 데이터 폐기 + 에러 로그 |

### 2.1 ⚠️ 윈도우 마감은 `push()`가 구동한다

별도 타이머 스레드가 없다. 모든 채널이 조용해지면 마지막 묶음은 **다음 프레임이 올 때까지
전달되지 않는다.**

이것이 데이터 유실로 이어지지는 않는다. 채널 사망은 MQTT LWT 가 별도 경로
(`Controller::onChannelAlive`)로 전달하므로 정지 감지 자체는 막히지 않는다.
다만 "마지막 한 윈도우가 늦게 도착할 수 있다"는 점은 계약의 일부다.

## 3. 데이터 흐름

```text
MqttChannelReceiver (PipelineWorker 스레드)
        │  push(frame)                        채널당 초당 5회
        ▼
  ┌──────────────────────────────────────────────┐
  │ ① 검증   ch 범위 · objects 수 상한            │  위반 → 프레임 드롭
  │ ② 마감   윈도우 경과 시 슬롯 → 풀 버퍼 swap    │  mutex_ 보유 구간
  │ ③ 저장   slots_[ch] 에 덮어쓰기 (assign)      │
  └──────────────────────────────────────────────┘
        │  mutex_ 해제
        ▼
  callback(const AggregatedFrames&)              락 밖에서 호출
        │
        ▼
  Controller::processPipeline
        └─> transform ─> fuse ─> zone ─> risk ─> dispatch / sink
        │
        ▼
  FrameBufferPool::release(buffer)               반납 (해제 아님)
```

`mutex_` 보유 구간은 ①~③ 뿐이다. 콜백(=파이프라인 전체)을 락 안에서 부르면 융합·위험판정·
UART 전송이 도는 내내 다른 채널의 `push()`가 전부 블로킹된다.

## 4. 현재 구현 — `TimeWindowAggregatorV2`

### 4.1 생성자

```cpp
TimeWindowAggregatorV2(std::shared_ptr<IClock> clock, uint64_t windowSizeMs, int channelCount);
```

| 인자 | 제약 | 위반 시 |
|------|------|---------|
| `clock` | non-null | `std::invalid_argument` |
| `windowSizeMs` | 제약 없음 (0 이면 매 push 마다 마감) | — |
| `channelCount` | `[1, 256]` | `std::invalid_argument` |

생성자가 던지는 것은 프로젝트 규약이다. 조립 시점 오류는 `main` 이 잡아 프로세스를
종료시킨다. — 조용히 잘못된 값으로 도는 것보다 낫다.

### 4.2 자료구조

| 멤버 | 형태 | 목적 |
|------|------|------|
| `slots_` | `std::vector<TopViewFrame>` | 인덱스 = `channelId` (해싱 없이 직접 인덱싱) |
| `occupied_` | `std::vector<std::uint8_t>` | 슬롯 점유 여부 (O(1) 판정) |
| `activeChannels_` | `std::vector<ChannelId>` | 이번 윈도우에 채워진 채널만 — 마감 시 순회 대상 |
| `flushPool_` | `FrameBufferPool` | 콜백에 넘길 묶음 버퍼 |

> **`std::optional`을 쓰지 않는 이유**: `optional::reset()`은 `TopViewFrame`을 파괴하면서
> 내부 `objects` 버퍼까지 해제한다. 점유 여부는 `occupied_` 로 따로 보고, 슬롯 자체는
> 살려 두어야 버퍼가 재사용된다.

> **`activeChannels_` 를 따로 두는 이유**: 마감 시 `slots_` 전체를 훑지 않는다.
> `channelCount = 256` 설정에서 활성 채널이 12개라면 빈 슬롯 244개를 건너뛴다.

## 5. 메모리 풀 연동

### 5.1 두 종류의 버퍼

| 버퍼 | 소유자 | 재사용 방식 |
|------|--------|-------------|
| `slots_[ch].objects` | 집계기 (채널당 1개) | 마감 시 `swap`, 평시 `assign` — **절대 해제하지 않음** |
| 마감 묶음 | `FrameBufferPool` | `acquire()` / `release()` |

### 5.2 버퍼 회전

마감 시 `slot.objects` 와 풀 버퍼 원소의 `objects` 를 **교환**한다.

```cpp
dst.objects.swap(slot.objects);   // 풀 버퍼가 데이터를 가져감
slot.objects.clear();             // 슬롯은 풀 버퍼의 옛 버퍼를 넘겨받음 (capacity 유지)
```

버퍼가 슬롯 ↔ 풀 사이를 오갈 뿐 **어느 쪽도 해제되지 않는다.** 전체 상주량은
`channelCount × (1 + poolSize)` 개로 상한이 잡힌다.

### 5.3 `FrameBufferPool` API

```cpp
FrameBufferPool pool(poolSize, framesPerBuffer);   // 생성 시 poolSize 개 미리 확보
auto buf = pool.acquire();                          // 비어 있으면 새로 만듦 (fail-open)
// ... buf.resize(n); 채우기; 콜백 ...
pool.release(std::move(buf));                       // clear() 하지 않고 반납
```

| 항목 | 정책 | 이유 |
|------|------|------|
| `release()`가 `clear()` 하지 않음 | 원소를 살려 반납 | `clear()` 는 원소를 파괴해 `objects` 버퍼를 해제하므로 풀의 존재 이유가 사라짐 <br> 길이 조절은 호출자가 `resize()`로 |
| `acquire()`는 실패하지 않음 | 풀이 비면 새로 할당 (fail-open) | 실시간 경로에서 버퍼가 없어 프레임을 버림은 할당 1회보다 나쁜 결과 |
| 고갈 계수 | `exhaustedCount()` | 5초 주기 지표 로그에 `풀 고갈 N회` 로 노출 |
| 동기화 | 내부 `mutex` | 윈도우당 2회만 잠금(acquire/release) — lock-free 복잡도를 치를 이유가 없음 |

### 5.4 풀 크기 산정

기본 `kFlushBufferPoolSize = 4`

실배포에서 `push()` 는 `MqttChannelReceiver`의 단일 `PipelineWorker` 스레드에서만
호출되므로 동시에 살아 있는 마감 버퍼는 **항상 1개**다. 4는 멀티스레드 Receiver
(`NullReceiver` 등 테스트/모의 구현)를 위한 여유분이다.

**`exhaustedCount() > 0` 이면 `kFlushBufferPoolSize` 를 늘린다.**

## 6. 동시성

| 자원 | 보호 | 비고 |
|------|------|------|
| `slots_`, `occupied_`, `activeChannels_`, `windowStartTime_`, `callback_` | `mutex_` | |
| `metrics_` | `metricsMutex_` | 별개 mutex — 지표 갱신이 본 로직 경합에 영향 없음 |
| `FrameBufferPool::free_` | 풀 내부 mutex | 윈도우당 2회만 잠금 |

> **⚠️ 콜백 재진입**: 콜백은 락 밖에서 호출되므로, 두 스레드가 거의 동시에 윈도우를
> 마감시키면 다운스트림이 동시에 두 번 실행될 수 있다. `Controller`는 `processPipeline`이
> 단일 스레드로 돈다고 가정하므로, 멀티스레드 Receiver를 붙일 때 주의한다.

## 7. 관측성

5초 주기로 다음 지표를 `logSuccess` 로 출력한다.

```text
Aggregator - 최근 5000ms 지표 - push() N회, 윈도우 마감 M회, 평균 락 보유시간 X.XXus, 드롭 D건, 풀 고갈 E회
```

| 필드 | 정상값 | 벗어나면 |
|------|--------|----------|
| 평균 락 보유시간 | < 1 µs | 콜백이 락 안으로 새어 들어갔는지 확인 |
| 드롭 | 0 | 채널 ID 오설정 또는 객체 수 상한 초과 |
| 풀 고갈 | 0 | `kFlushBufferPoolSize` 증설 |

측정 대상은 **`mutex_` 보유 시간만**이다. 콜백은 락 밖이므로 제외된다 — 다운스트림이 느려도
이 수치는 오르지 않아야 정상이다.

## 8. 튜닝 상수

| 상수 | 위치 | 기본값 | 의미 |
|------|------|--------|------|
| `kMaxObjectsPerFrame` | `TimeWindowAggregatorV2.h` | 256 | 프레임당 객체 수 상한. **compute-server 파서 상한과 같아야 함** |
| `kFlushBufferPoolSize` | `TimeWindowAggregatorV2.h` | 4 | 마감 버퍼 풀 크기 |
| `kMetricsReportInterval` | `TimeWindowAggregatorV2.h` | 5000 ms | 지표 로그 주기 |
| `windowSizeMs` | `config.json` | 100 ms | 윈도우 크기 |

## 9. 테스트 하네스

집계기 테스트는 실제 시계나 브로커 없이 `IClock` 을 주입해 시간을 제어한다.

```cpp
class FakeClock final : public IClock {
public:
    veda::TimestampMs now() const override { return t_; }
    void advance(veda::TimestampMs ms) { t_ += ms; }
    veda::TimestampMs t_ = 1000;
};
```

```cpp
auto clock = std::make_shared<FakeClock>();
TimeWindowAggregatorV2 agg(clock, 100, 4);

std::size_t received = 0;
agg.setCallback([&](const std::vector<veda::TopViewFrame>& f) { received += f.size(); });

agg.push(makeFrame(0));
clock->advance(200);      // 윈도우 경과
agg.push(makeFrame(1));   // 이 push 가 이전 윈도우를 마감시킨다
```

하네스가 검증할 항목:

- 같은 채널 중복 push 시 최신 것만 남는지
- 윈도우 경과 전에는 콜백이 호출되지 않는지
- 범위 밖 `channelId` 와 상한 초과 `objects` 가 드롭되는지
- 생성자가 잘못된 `channelCount` / null `clock` 을 거부하는지
- 정상상태에서 힙 할당이 0인지 (전역 `operator new` 계수)

## 10. 확장 체크리스트

새 Aggregator 구현을 추가할 때 확인한다.

- [ ] `IFrameAggregator` 만 구현하고 `Controller` 를 수정하지 않았는가
- [ ] 콜백을 락 **밖**에서 호출하는가
- [ ] 콜백에 넘기는 버퍼의 수명 계약을 문서화했는가
- [ ] `push()` 핫패스에 힙 할당이 없는가 (전역 `operator new` 로 계수 검증)
- [ ] `channelId` 범위와 `objects` 수 상한을 검사하는가
- [ ] 생성자가 잘못된 인자를 던져서 거부하는가
- [ ] 시계를 `IClock` 으로 주입받아 테스트 가능한가
- [ ] 드롭·고갈 카운터를 지표로 노출하는가

## 11. 현재 구조의 개선 과제

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| 윈도우 마감 구동 | `push()` 에 의존 — 전 채널 침묵 시 마지막 묶음 지연 | 타이머 기반 마감 도입 검토 (스레드 1개 추가 비용과 견줄 것) |
| 콜백 재진입 | Receiver 구현에 의존 | `Controller` 쪽에서 파이프라인 진입 직렬화 |
| 객체 수 상한 | 세 곳에 각각 선언 (컴파일 검증 없음) | `shared/` 로 승격해 단일 정의화 검토 |
| 테스트 | `tests/` 미등록 | control-server를 테스트 범위에 포함할 때 이관 |
| `V1` 잔존 | `TimeWindowAggregator.h/.cpp` 가 빌드에서 제외된 채 남아 있음 | 비교 목적이 끝났으면 제거 |

인터페이스는 작게 유지한다. 윈도우 크기 조회, 통계 노출, 강제 플러시 같은 메서드를
`IFrameAggregator` 에 추가하지 않는다. — 그런 기능은 구현체 또는 별도 관측 인터페이스의 책임이다.