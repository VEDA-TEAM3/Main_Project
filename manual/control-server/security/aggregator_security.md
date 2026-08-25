# Aggregator 계층 보안·성능 권고 (Security & Performance Advisory)

> **대상 모듈**
> - `include/interfaces/IFrameAggregator.h`
> - `src/aggregate/TimeWindowAggregatorV2.h`, `.cpp`
> - `src/aggregate/FrameBufferPool.h`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-30 | 1.0.0 | Mangjun | Aggregator 계층 OOM·경계 검사·스택 안전성 및 무할당 리팩터 감사 명세 |

---

## 1. 위협 모델 (Threat Model)

Aggregator는 control-server에서 **원격 입력이 처음으로 메모리에 상주하는 계층**이다.
compute-server의 파서와 달리 이쪽은 신뢰 경계를 넘어온 데이터를 **보관**한다 — 윈도우가
닫힐 때까지 전 채널의 프레임이 슬롯에 남아 있다.

| 출처 | 통제 주체 | 위협 |
|------|-----------|------|
| **`frame.objects`** (MQTT 디코드 결과) | **원격 compute-server(신뢰 불가)** | 무제한 객체 수 → 슬롯 상주량 폭증 → OOMKill |
| **`frame.ch`** | 원격 compute-server(신뢰 불가) | 범위 밖 인덱스 → 힙 오염 |
| **`channelCount`** | 운영자 `config.json` / DI | 음수 → `size_t` 캐스팅 폭주 → 거대 할당 |
| **`clock`** | 조립 코드 (DI) | null → 락 보유 중 세그폴트 |

| 위협 범주 | 시나리오 | 성립 여부 |
|-----------|----------|-----------|
| **OOM — 객체 수 무제한** | 대량 객체 프레임으로 슬롯 상주량 폭증 | ✅ 차단 (상한 256) |
| **OOM — 거대 할당** | 음수 `channelCount` 로 엑사바이트 요구 | ✅ 조립 시점 throw |
| **OOM — 힙 단편화** | 초당 130회 할당/해제 누적 | ✅ **핫패스 할당 0** |
| **힙 오염 (OOB write)** | 범위 밖 `channelId` 로 슬롯 밖 쓰기 | ✅ 구조적 불가 — 인덱싱 전에 검사 |
| **스택 오버플로** | 재귀·대형 지역 배열 | ✅ 구조적 불가 |
| **널 역참조** | `clock` null | ✅ 조립 시점 throw |
| **DoS — 락 점유** | 느린 다운스트림으로 전 채널 차단 | ✅ 콜백이 락 밖 |
| **UAF — 버퍼 수명** | 콜백이 빌린 버퍼를 보관 | ⚠️ **계약으로만 방어** (A1) |
| **로그 도배 / 로그 인젝션** | 드롭 로그로 디스크·CPU 소모 | ⚠️ 정수만 삽입, 단 rate-limit 없음 (A3) |

### 실패 방향 분석

이 계층의 실패는 **"프레임이 전달되지 않는다"** 로 나타난다.

| 방향 | 결과 | 평가 |
|------|------|------|
| **과소 수용** (프레임 드롭) | 해당 채널이 이번 윈도우에서 빠짐 | 위험 판정이 한 윈도우(100ms) 늦어짐 |
| **과대 수용** (무제한 보관) | 컨테이너 OOMKill → **전 채널 정지** | ⚠️ **훨씬 나쁨** |

> **⚠️ 그래서 방어는 "의심스러우면 버린다" 방향으로 설계했다.** 프레임 하나를 버리면
> 100ms 뒤 다음 프레임이 온다. 그러나 OOMKill 은 프로세스 전체를 멈추고, 같은 프레임이
> 재시도되면 크래시 루프가 된다. 한 채널의 100ms 손실과 전 시스템 정지는 비교 대상이 아니다.

---

## 2. 경계 검사 (Bounds & Input Validation)

### 2.1 ✅ 프레임당 객체 수 상한 — OOM 차단

```cpp
static constexpr std::size_t kMaxObjectsPerFrame = 256;

if (frame.objects.size() > kMaxObjectsPerFrame) {
    logError(kIface, "ch=... 객체 N개가 상한 초과 - 프레임 드롭");
    ++metrics_.droppedCount;
    return;
}
```

리팩터 전에는 이 검사가 **없었다.** compute-server가 256으로 자르지만 그것은 **상대편의
선의**일 뿐이다. 경계 검사는 자기 쪽에서 해야 한다 — 고장난 퍼블리셔, 다른 벤더 구현,
또는 악의적 발신자가 붙는 순간 그 가정은 무너진다.

**피해 규모**: 객체 1개 ≈ 40B 기준, 채널당 100만 객체 = 40MB. 12채널이면 480MB가 슬롯에
상주하고 마감 시 풀 버퍼로 옮겨지며 순간 2배가 된다. 컨테이너 제한(256M)을 즉시 넘긴다.

> **자르지 않고 통째로 버리는 이유**: 앞 256개만 취하면 "일부만 반영된 프레임"이 되어
> 융합·위험 판정이 **조용히 틀린 답**을 낸다. 드롭은 지표에 드러나지만 부분 수용은 드러나지
> 않는다. 명시적 실패가 조용한 오작동보다 낫다.

> **⚠️ 상수 동기화**: `kMaxObjectsPerFrame` 은 compute-server의 `OnvifParser.cpp`,
> `ContainmentSanitizer.cpp` 와 **같은 값이어야 한다.** 서로 다른 TU(그리고 다른 서버)의
> 상수라 불일치해도 컴파일 오류가 나지 않는다.

### 2.2 ✅ `channelId` 범위 — 힙 오염 구조적 불가

```cpp
if (frame.ch < 0 || frame.ch >= channelCount_) {
    logError(kIface, "channelId ... 범위 밖 - 프레임 드롭");
    ++metrics_.droppedCount;
    return;
}
```

리팩터 전에도 있었고 그대로 유지했다. **`slots_[ch]` 인덱싱 전에 수행되므로 범위 밖 쓰기가
성립할 코드 경로가 없다.** 하한(`< 0`)을 함께 보는 것이 중요하다 — `ChannelId` 는 부호 있는
타입이라 음수가 들어올 수 있고, `size_t` 로 캐스팅되면 거대한 인덱스가 된다.

리팩터에서 **드롭 카운터를 추가**했다. 이전에는 에러 로그만 남고 집계되지 않아, 로그 레벨을
올린 운영 환경에서는 드롭이 일어나는지조차 알 수 없었다.

### 2.3 ✅ `channelCount` 범위 — 거대 할당 차단

```cpp
latestByChannel_(static_cast<std::size_t>(channelCount))   // 리팩터 전: channelCount = -1
```

`-1` 이 `std::size_t` 로 캐스팅되면 `18446744073709551615` 가 된다. 생성자가 곧바로 수십
엑사바이트를 요구하고 `std::length_error` 또는 `std::bad_alloc` 이 난다.

`AppConfig::load` 가 `[1, 256]` 으로 보정하므로 정상 기동 경로에서는 도달하지 않지만,
**DI로 직접 조립하는 경로(테스트·다른 조립부)에는 방어가 없었다.**

```cpp
if (channelCount_ < 1 || channelCount_ > kMaxChannelCount) {
    throw std::invalid_argument("aggregator channelCount out of range [1, 256]");
}
```

`kMaxChannelCount = 256` 은 `AppConfig` 의 동일 상수와 맞춘 값이다 (하드웨어 채널 ID가
`uint8_t` 범위).

### 2.4 ✅ `clock` null — 락 보유 중 세그폴트 차단

`clock_->now()` 는 **`mutex_` 를 쥔 상태에서** 호출된다. null이면 첫 `push()` 에서 세그폴트가
나며, 락을 쥔 채 죽으므로 다른 스레드도 함께 멈춘다.

```cpp
if (!clock_) { throw std::invalid_argument("aggregator requires a non-null clock"); }
```

---

## 3. 메모리 & 스택 안전성

### 3.1 ✅ 핫패스 힙 할당 0 — 단편화 차단

리팩터 전에는 마감 시 `std::move` 후 `optional::reset()` 을 호출했다. `reset()` 이
`TopViewFrame` 을 파괴하면서 내부 `objects` 버퍼까지 **해제**하므로, 다음 윈도우의 첫
push가 반드시 재할당했다. 여기에 윈도우마다 `std::vector<TopViewFrame>` 이 새로 만들어졌다.

**실측: 24,000 push 동안 26,002회 할당 (윈도우당 13회 = 채널 12 + 외곽 벡터 1).**

| 발생원 | 횟수/윈도우 |
|--------|-------------|
| `optional::reset()` 후 다음 push의 `objects` 재할당 | 12 (채널당 1) |
| 마감 묶음 벡터 | 1 |

즉각적인 OOM은 아니지만 두 가지 문제가 있다.

- **힙 단편화**: 크기가 미세하게 다른 블록이 초당 130회 할당/해제되며 glibc malloc의 bin을
  오염시킨다. RSS가 실사용량보다 서서히 커져, 256M 제한 컨테이너에서는 수 주 뒤 OOMKill이
  날 수 있다.
- **할당자 락 경합**: mosquitto 네트워크 스레드와 같은 힙을 쓰므로, 집계기의 할당이
  **수신 경로의 지연으로 새어 나간다.**

**조치**: 슬롯 버퍼는 `swap` 으로 회전시켜 해제 자체를 없애고, 마감 묶음은
`FrameBufferPool` 에서 대여·반납한다.

```cpp
dst.objects.swap(slot.objects);   // 해제 없음 — 버퍼가 슬롯 ↔ 풀 사이를 오갈 뿐
slot.objects.clear();             // capacity 유지
```

**실측 결과: 24,000 push 동안 할당 1회** (warmup 잔여, 이후 정상상태 0).

| | Before | After |
|---|---|---|
| 상주 버퍼 수 | 가변 (할당/해제 반복) | `channelCount × (1 + poolSize)` 로 **상한 고정** |
| 12채널 기준 | — | 60개 |
| 단편화 위험 | 초당 130회 | 없음 (해제가 일어나지 않음) |

> **⚠️ 회귀 주의**: `slots_` 를 `std::optional` 로 되돌리거나 `FrameBufferPool::release()` 에
> `clear()` 를 추가하면 이 성질이 조용히 사라진다. 둘 다 "정리를 안 한 것처럼 보이는" 코드라
> 리뷰에서 되돌려지기 쉽다.

### 3.2 ✅ 상주량 상한 — OOM 계산 가능

리팩터 후 집계기가 붙잡을 수 있는 최악 메모리는 계산 가능한 값이다.

```text
channelCount × (1 + kFlushBufferPoolSize) × kMaxObjectsPerFrame × sizeof(TopViewObject)
= 12 × 5 × 256 × 40B ≈ 614 KB
```

256M 컨테이너 제한 대비 **0.24%**. 상한이 없던 리팩터 전에는 이 값이 무한대였다.

### 3.3 ✅ 스택 오버플로 — 구조적 면역

- **재귀 없음.** `push()` → `fillFlushBufferLocked()` → 반환. 콜백은 다운스트림으로 나가며
  집계기로 되돌아오지 않는다 (파이프라인이 단방향).
- **대형 지역 배열 없음.** VLA·`alloca`·큰 `std::array` 를 쓰지 않는다. 최대 스택 객체는
  `std::ostringstream`(지표 보고, 5초에 1회)이며 힙 기반이다.
- 채널 순회는 `activeChannels_`(힙, 생성 시 `reserve`) 기반 평면 루프다.

> **⚠️ 계약 위반 시**: 콜백 구현이 다시 `push()` 를 부르면 `mutex_` 는 이미 해제된 뒤라
> 데드락은 나지 않지만 **무한 재귀**가 가능하다. 인터페이스 계약상 금지이며, 현재 유일한
> 소비자 `Controller::processPipeline` 은 위반하지 않는다.

---

## 4. 성능 (CPU / 동시성)

### 4.1 ✅ 락 보유 시간 — 다운스트림과 분리

콜백은 **`mutex_` 밖**에서 호출된다. 락 안에서 부르면 융합·위험 판정·UART 전송이 도는 내내
다른 채널의 `push()` 가 전부 블로킹된다 — 느린 다운스트림 하나가 전 채널을 멈추는
DoS 경로가 된다.

지표 로그의 `평균 락 보유시간` 이 이 성질을 감시한다. 콜백이 락 안으로 새어 들어가면
이 수치가 즉시 올라간다.

### 4.2 ✅ 마감 순회 — O(활성 채널)

`activeChannels_` 만 순회하므로 `channelCount = 256` 설정에서 활성 채널이 12개라면 빈 슬롯
244개를 훑지 않는다. 리팩터 전에는 `latestByChannel_` 전체를 스캔했다 (O(channelCount)).

### 4.3 ✅ 지연 실측

| 지표 | Before | After | 변화 |
|------|--------|-------|------|
| `push()` p50 | 0.06 µs | 0.10 µs | +0.04 µs |
| `push()` p99 | 0.46 µs | **0.22 µs** | **−52%** |
| 총 힙 할당 | 26,002회 | **1회** | −99.996% |

p50이 소폭 악화된 것은 `occupied_` 조회와 `activeChannels_.push_back()` 이 정상 경로에
추가되었기 때문이다. **의도적으로 치른 비용**이다 — 이전 p99의 꼬리는 대부분 마감 윈도우의
13회 할당이었고, 실시간 시스템에서 의미 있는 값은 중앙값이 아니라 꼬리다.

> 상세 수치와 재현 절차는 성능 문서를 참고한다. 본 문서는 보안 관점(단편화·경합)만 다룬다.

### 4.4 ⚠️ 스레드 안전 — 콜백 재진입 창

`push()` 자체는 스레드 안전하다. 그러나 두 스레드가 거의 동시에 윈도우를 마감시키면
콜백이 **동시에 두 번** 실행될 수 있다. `Controller.h` 는 "processPipeline은 단일 스레드에서만
도므로 락이 필요 없음"이라고 명시하므로, 이 가정이 깨지면 `observations_` 등 멤버 버퍼가
경합한다.

실배포(`MqttChannelReceiver`)는 `push()` 가 단일 `PipelineWorker` 스레드에서만 호출되어
도달하지 않는다. 멀티스레드 `NullReceiver` 에서는 가능하다. **리팩터 전후 동일**하며
버퍼 자체의 경합은 없다(풀에서 분리되어 나옴).

---

## 5. 감사 요약

| 영역 | 판정 | 근거 |
|------|------|------|
| **OOM (객체 수)** | ✅ **All Clear** | `kMaxObjectsPerFrame = 256` 상한, 초과 시 프레임 전체 드롭 <br> 상주량이 614 KB로 계산 가능한 상한을 가짐 |
| **OOM (거대 할당)** | ✅ **All Clear** | 음수·과대 `channelCount` 를 조립 시점 throw → `main` 종료 |
| **OOM (단편화)** | ✅ **All Clear** | 핫패스 할당 0 (전역 `operator new` 계수로 실측 검증) <br> 버퍼 해제가 일어나지 않아 단편화 원인 자체가 소멸 |
| **경계 검사** | ✅ **All Clear** | `channelId` 를 인덱싱 **전에** 검사 → OOB write가 성립할 경로 없음 |
| **널 역참조** | ✅ **All Clear** | `clock` null을 생성자에서 거부 |
| **스택 오버플로** | ✅ **All Clear** (구조적 면역) | 재귀 없음, 지역 저장소 O(1), VLA/`alloca` 없음 |
| **DoS (락 점유)** | ✅ **All Clear** | 콜백이 락 밖 → 느린 다운스트림이 다른 채널을 막지 않음 |
| **버퍼 수명 (UAF)** | ⚠️ **계약 의존** | 콜백이 참조를 보관하면 UAF. 컴파일러가 잡지 못함 (A1) |

### 지적 사항 목록

| # | 항목 | 등급 | 영향 |
|---|------|------|------|
| **A1** | 콜백에 넘기는 버퍼가 **borrowed reference** — 보관 시 UAF | ⚠️ **Warning** | 풀에서 빌린 버퍼라 콜백 반환 즉시 반납되고 다음 윈도우에 덮어써진다. 컴파일 오류가 나지 않으므로 새 소비자를 붙일 때 반드시 확인. 현재 유일한 소비자 `processPipeline` 은 읽기만 하므로 안전 |
| **A2** | 콜백 **재진입 가능** (멀티스레드 Receiver) | ⚠️ **Warning** | `processPipeline` 의 단일 스레드 가정이 Receiver 구현에 의존. 실배포는 단일 `PipelineWorker` 라 미해당. 근본 조치는 `Controller` 쪽 직렬화이며 aggregator 범위 밖 |
| **A3** | 드롭 로그에 **rate-limit 이 없다** | ℹ️ Info | 지속적으로 잘못된 프레임이 들어오면 프레임당 1회 에러 로그가 쌓인다. 삽입 값은 정수(`ch`, `size`)뿐이라 **로그 인젝션 경로는 없고**, CSV 이스케이프는 `Logger` 가 담당. 필요 시 `HomographyTransform` 의 `shouldLogFailure()` 패턴 적용 |
| **A4** | `kMaxObjectsPerFrame` 이 **세 곳에 각각 선언** | ℹ️ Info | compute-server 2곳 + control-server 1곳. 서로 다른 TU라 불일치가 컴파일 오류로 드러나지 않는다. `shared/` 승격 검토 |
| **A5** | 윈도우 마감이 `push()` 구동 — 전 채널 침묵 시 마지막 묶음 지연 | ℹ️ Info | 채널 사망은 LWT가 별도 경로로 알리므로 정지 감지는 막히지 않는다. 데이터 유실이 아닌 지연 |

### 범위 밖 — 상류 권고

| # | 항목 | 등급 | 내용 |
|---|------|------|------|
| **U1** | **디코드 단계 페이로드 크기 무제한** | ⚠️ **Warning** | `MqttChannelReceiver::pipelineLoop` 의 `veda::decode<TopViewFrame>(payload)` 에 크기 상한이 없다. 수신 큐는 **개수** 상한(`kMaxQueuedMessages`)만 있고 **페이로드 1건의 크기** 상한이 없다. 2.1의 객체 수 검사는 디코드 **이후**에 동작하므로 **피크 메모리를 막지 못한다.** 100MB JSON 하나면 nlohmann DOM이 그 몇 배를 잡는다. <br> **권고**: MQTT 콜백에서 큐에 넣기 전 `payload` 바이트 길이 상한을 건다. `kMaxObjectsPerFrame × 객체당 최대 JSON 길이` ≈ 수십 KB면 충분하다. receive 계층 변경이라 본 감사 범위에 포함하지 않았다 |

**총평**: 리팩터 전 Aggregator는 **control-server에서 가장 노출된 OOM 표면**이었다.
원격 입력을 보관하면서 객체 수 상한이 없었고, 초당 130회의 할당/해제로 장기 단편화를
누적했으며, 음수 `channelCount` 에 대한 방어도 없었다.

리팩터 후 상주량은 **614 KB로 계산 가능한 상한**을 갖고, 핫패스 할당은 **실측 0**이며,
잘못된 조립 인자는 프로세스 기동 자체를 막는다. 남은 위험은 두 종류다 — **타입 시스템이
강제하지 못하는 계약**(A1 버퍼 수명, A2 재진입)과 **상류 계층의 미조치**(U1 페이로드 크기).

A1·A2는 코드가 아니라 리뷰가 지켜야 하는 항목이므로, 새 소비자나 새 Receiver를 붙일 때
[Aggregator 레퍼런스](../components/aggregator_reference.md) 1.1절과 6장을 함께 확인한다.

**U1은 별도 조치가 필요하다.** Aggregator의 방어만으로는 피크 메모리를 막을 수 없다.
