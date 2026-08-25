# Source 모듈 레퍼런스 (비즈니스 로직 & 핸드오프)

> **대상 파일**
> - 인터페이스: `include/interfaces/IMetadataSource.h`
> - 구현체: `src/source/RtspOnvifSourceV2.h`, `.cpp`

**push ↔ pull 임피던스 불일치를 해소하는 어댑터 계층**이다. 하위 네트워크 계층(`RtspClientV2`)은 카메라가 **밀어보내는(push)** 콜백 방식인데, `main` 루프는 `source.next(out)`을 **당겨쓰는(pull)** 블로킹 호출로 소비한다. 이 계층이 그 사이에 **고정 크기 SPSC 링버퍼**를 놓아 두 세계를 잇는다.

동시에 **재연결·백오프 정책의 소유자**이기도 하다. 하위 클라이언트는 "한 번의 연결 시도"만 알고, "끊기면 언제 다시 붙을지"는 전적으로 이 계층이 정한다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | IMetadataSource 인터페이스 및 RtspOnvifSourceV2 구현체 분석 (SPSC 링버퍼 기반 파이프라인 핸드오프 설계 문서화) |

---

## 1. 인터페이스 명세 (Interface Contract)

```cpp
class IMetadataSource {
public:
    virtual ~IMetadataSource() = default;

    /// @brief 다음 Metadata 패킷을 가져옴 (패킷이 올 때까지 블로킹해도 됨)
    /// @return 정상 수신 시 true, 스트림 종료 시 false
    virtual bool next(domain::RawPacket& out) = 0;

    /// @brief 블로킹 중인 next()를 깨워 스트림을 끝냄
    virtual void stop() noexcept = 0;
};
```

| 항목 | 계약 내용 |
|------|-----------|
| **블로킹 허용** | `next()`는 패킷이 도착할 때까지 블로킹해도 된다 |
| **종료 신호** | `false` 반환 = 스트림 종료 <br> **`false`를 받은 뒤 다시 `next()`를 부르는 것은 허용되지 않음** |
| **`stop()`의 존재 이유** | `next()`는 무한정 기다리므로, 이게 없으면 종료 시그널을 받아도 `main` 루프를 빠져나올 방법이 없다. 호출 이후 `next()`는 **남은 버퍼를 모두 내보낸 뒤** `false`를 반환 |
| **스레드 안전성** | `stop()`은 `next()`를 부르는 스레드가 **아닌 다른 스레드**(시그널 처리 스레드)에서 호출된다. → 구현체는 `next()`와의 동시 호출에 안전해야 함 |
| **예외 금지** | `stop()`은 `noexcept` — 예외를 던지지 않음 |

---

## 2. 구현체 분석: `RtspOnvifSourceV2`

### 2.1 워커 스레드 수명주기

**생성자에서 즉시 시작, 소멸자에서 정지 후 join**

```cpp
RtspOnvifSourceV2::RtspOnvifSourceV2(const AppConfig& config)
    : ringCapacity_(config.sourceRingCapacity),
      metricsReportInterval_(config.metricsReportIntervalMs),
      backoffInitialSec_(config.rtspReconnectBackoffInitialSec),
      backoffMaxSec_(config.rtspReconnectBackoffMaxSec),
      config_(config) {
    ring_.resize(ringCapacity_);                                  // 링 슬롯 미리 확보
    worker_ = std::thread(&RtspOnvifSourceV2::workerLoop, this);  // 즉시 가동
}

RtspOnvifSourceV2::~RtspOnvifSourceV2() {
    stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}
```

> **주의**: 생성자가 곧바로 워커를 띄우므로, `AppContext`가 Pipeline을 다 조립하기 **전에** 이미 패킷이 링에 쌓이기 시작할 수 있다. drop-oldest 정책 덕분에 이는 문제가 되지 않는다. (오래된 프레임이 밀려날 뿐, 메모리가 늘지 않음)

### 2.2 SPSC 링버퍼 설계 (drop-oldest)

`std::queue<RawPacket>`(deque, 청크 단위 할당/해제)을 버리고 **고정 크기 `std::vector<domain::RawPacket>`** 를 쓴다. 이 벡터는 **저장소이자 동시에 `RawPacket` 버퍼 풀** 역할을 겸한다.

```cpp
std::vector<domain::RawPacket> ring_;
std::size_t head_  = 0;   ///< 다음에 next()로 꺼낼 위치
std::size_t count_ = 0;   ///< 현재 채워진 개수 (ringCapacity_ 이하)
```

**Producer (RtspClientV2 콜백 스레드)**:

```cpp
std::lock_guard<std::mutex> lk(mtx_);

std::size_t writeIdx = 0;
if (count_ == ringCapacity_) {
    // 링이 가득 참: 가장 오래된 프레임 자리를 그대로 재사용 -> drop-oldest
    writeIdx = head_;
    head_ = (head_ + 1) % ringCapacity_;
    ++metrics_.droppedCount;
} else {
    writeIdx = (head_ + count_) % ringCapacity_;
    ++count_;
}

domain::RawPacket& slot = ring_[writeIdx];
slot.channelId = config_.channelId;
slot.bytes.assign(payload.begin(), payload.end());  // 슬롯의 기존 capacity 재사용
slot.recvTime  = std::chrono::system_clock::now();

cv_.notify_one();
```

| 설계 선택 | 이유 |
|-----------|------|
| **고정 크기 링** | 큐가 무제한으로 자라지 않음 (메모리 상한 보장) <br> 청크 단위 할당/해제 제거 |
| **drop-oldest** | Pipeline이 못 따라가면 **가장 오래된** 프레임을 버린다. <br> 실시간 좌표는 **최신이 항상 더 유용**하므로, 지연이 무한정 누적되는 것보다 낫다. |
| **슬롯에 직접 `assign`** | 콜백마다 새 `RawPacket`을 만들지 않고 슬롯에 직접 쓴다 → 슬롯 `vector`의 capacity를 재사용 → **콜백당 힙 할당 0** |
| **`recvTime` 기록** | 네트워크 도착 시각 <br> 이후 `next()` 시점과의 차이로 **큐 지연**을 계측 |

### 2.3 Zero-copy 핸드오프 — `std::swap` (핵심 최적화)

`next()`가 링에서 패킷을 꺼낼 때 **복사하지 않고 버퍼 소유권을 교환**한다.

```cpp
bool RtspOnvifSourceV2::next(domain::RawPacket& out) {
    std::string report;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return count_ > 0 || stopping_.load(); });

        if (count_ == 0) {
            return false;          // stop() + 잔여 없음 -> 스트림 종료
        }

        domain::RawPacket& slot = ring_[head_];

        out.channelId = slot.channelId;
        std::swap(out.bytes, slot.bytes);       // ★ O(1) 포인터 스왑 (복사 아님)
        out.recvTime  = slot.recvTime;

        head_ = (head_ + 1) % ringCapacity_;
        --count_;

        const auto latency = std::chrono::system_clock::now() - out.recvTime;
        metrics_.totalQueueLatency += std::chrono::duration_cast<std::chrono::nanoseconds>(latency);
        ++metrics_.consumedCount;

        report = buildMetricsReportIfDue();
    }

    if (!report.empty()) {
        logSuccess(kIface, report);   // 로그는 락 밖에서
    }

    return true;
}
```

**`std::swap`이 주는 이중 이득**:

1. **per-frame `memcpy` 제거** — 수 KB 복사가 O(1) 포인터 교환으로 대체된다.
2. **양쪽 capacity 보존** — `out`이 들고 있던(이미 소비 완료된) 버퍼가 **슬롯으로 넘어가** 다음 write의 capacity로 재사용된다. `main` 루프가 `domain::RawPacket raw;` 하나를 재사용하므로, warmup 이후 양쪽 모두 할당이 없다.

> **스레드 안전성**: 슬롯과 파이프라인 모두 `mtx_` 안에서만 `bytes`를 만지므로 스왑은 안전하다.

### 2.4 왜 lock-free가 아니라 mutex + CV인가

producer 1개(RTSP 콜백 스레드) ↔ consumer 1개(`next()` 호출 스레드)의 **전형적 SPSC** 구조지만, 의도적으로 lock-free 큐를 쓰지 않는다.

- **컨슈머를 재우기 위해서**: `cv_.wait`으로 잠들었다가 알림에만 깨어난다. lock-free + 바쁜 대기(spin)는 라즈베리파이의 제한된 CPU를 낭비한다.
- 실측 처리율이 5fps 수준이라 락 경합이 사실상 없고, 지표에도 영향이 없다.

### 2.5 재연결 & 백오프 정책 (이 계층의 소유)

```cpp
void RtspOnvifSourceV2::workerLoop() {
    int backoffSec = backoffInitialSec_;

    while (!stopping_) {
        // client 보다 먼저 선언 -> client 가 먼저 파괴되므로 콜백이 참조하는 동안 항상 살아있음
        bool sessionProductive = false;
        RtspClientV2 client(config_);                 // 세션마다 지역 인스턴스 (RAII)

        client.onPayloadReceived = [this, &sessionProductive](std::string_view payload) {
            sessionProductive = true;                  // 실제 payload 를 받은 세션만 '생산적'
            ... 링버퍼에 기록 (§2.2) ...
        };

        { std::lock_guard<std::mutex> lk(clientMutex_);
          if (stopping_) {
            break;
          }
          activeClient_ = &client;
        } // stop() 이 취소할 수 있도록 등록

        if (client.connect() && client.setup()) {
            client.play();
            if (client.playSucceeded()) {
                client.run();   // PLAY 200 아니면 run() 진입 안 함
            }
        }

        { std::lock_guard<std::mutex> lk(clientMutex_);
          activeClient_ = nullptr; }                    // 파괴 '전에' 반드시 해제 (UAF 방지)

        if (stopping_) {
            break;
        }

        if (sessionProductive) {
            backoffSec = backoffInitialSec_;   // 생산적 세션만 리셋
        }

        ... 로그 + cv_.wait_for(backoffSec) ...
        backoffSec = backoffSec > backoffMaxSec_ - backoffSec
                         ? backoffMaxSec_
                         : backoffSec * 2;    // overflow 없이 지수 증가 (포화)
    }
}
```

**① 백오프는 "생산적" 세션에만 리셋한다** — 이것이 정책의 핵심이다. 예전에는 매 시도마다 1초로 리셋되어, `rtspPlayUri`가 잘못된 설정 오류 상황에서 **카메라를 1초 간격으로 두드리는 재접속 폭풍**이 발생했다. 이제는 *실제로 payload가 도착한* 세션만 백오프를 초기화하므로, connect/setup은 되는데 PLAY가 계속 실패하는 경우에도 지수 백오프(1s → 2s → … → 최대 30s)가 정상적으로 커진다.

**② 세션마다 지역 `RtspClientV2` 인스턴스** — 루프를 돌 때마다 RAII로 소켓·keepalive 스레드가 정리되므로 fd/스레드 누수가 없다.

**③ 선언 순서가 중요하다** — `sessionProductive`를 `client`보다 **먼저** 선언한다. 지역 변수는 역순으로 파괴되므로 `client`가 먼저 사라지고, 콜백이 캡처한 참조가 죽은 변수를 가리키는 일이 없다.

### 2.6 취소 경로와 UAF 방지

```cpp
void RtspOnvifSourceV2::stop() noexcept {
    stopping_.store(true);

    {
        std::lock_guard<std::mutex> lk(clientMutex_);
        if (activeClient_ != nullptr) {
            activeClient_->cancel();   // 진행 중인 세션 즉시 취소
        }
    }

    std::lock_guard<std::mutex> lock(mtx_);
    cv_.notify_all();   // lost wakeup 없이 next() 와 백오프 대기를 깨움
}
```

- **왜 플래그만으로는 부족한가**: 스트림이 건강한 동안 `recv()`는 계속 성공해 타임아웃이 나지 않는다. `stopping_`만 세우면 `run()`이 반환하지 않아 소멸자의 `worker_.join()`이 **무한 대기**한다. (= SIGINT/SIGTERM에 응답하지 못하고 MQTT 종료 신호도 못 보냄) <br> `cancel()`이 소켓에 `shutdown(SHUT_RDWR)`을 걸어 블로킹 `recv()`를 즉시 푼다.
- **UAF 방지 계약**: `activeClient_`는 `clientMutex_`로 보호되며, 워커는 클라이언트가 파괴되기 **전에 반드시 `nullptr`로 지운다.** 이 순서가 깨지면 `stop()`이 이미 죽은 객체의 `cancel()`을 부른다.
- **멱등**: 여러 번 호출해도 안전하다.

### 2.7 종료 시맨틱 — 잔여 버퍼 배출

`next()`의 술어는 `count_ > 0 || stopping_`이다. 따라서 `stop()` 이후에도:

1. 링에 남은 프레임이 있으면 → 계속 `true`를 반환하며 **전부 배출**
2. 링이 비면 → `count_ == 0`이므로 `false` 반환 → `main` 루프 종료

이는 인터페이스 계약("호출 이후 `next()`는 남은 버퍼를 모두 내보낸 뒤 `false`를 반환")을 정확히 구현한 것이다.

### 2.8 성능 지표

```cpp
struct Metrics {
    std::uint64_t producedCount;   ///< 콜백에서 생산된 프레임 수
    std::uint64_t consumedCount;   ///< next()로 소비된 프레임 수
    std::uint64_t droppedCount;    ///< 링이 가득 차서 버려진 수 (drop-oldest)
    std::uint64_t totalBytes;      ///< 소비된 payload 총 바이트
    std::chrono::nanoseconds totalQueueLatency;  ///< recvTime ~ next() 인출 시간 합
    std::chrono::steady_clock::time_point windowStart;
} metrics_;
```

`buildMetricsReportIfDue()`는 **`mtx_`를 잡은 상태에서 호출되어 문자열만 만들어 반환**하고, 실제 로그 출력(`logSuccess`)은 **호출자가 락을 푼 뒤** 수행한다. → 락 유지 시간 최소화

**`droppedCount`가 지속적으로 증가한다면** Pipeline이 카메라 프레임 레이트를 못 따라가고 있다는 신호다. (`sourceRingCapacity` 조정 또는 하류 최적화 검토)

> `ringCapacity_`가 런타임 값이 되면서 인덱스 계산의 `%`가 컴파일타임 비트마스크로 접히지 않게 되었으나, 콜백/`next()`당 각 1회씩이고 실측 처리율이 5fps 수준이라 측정 지표에 변화가 없었다.

---

## 3. AppContext / main 통합

### 3.1 조립 (`AppContext`)

```cpp
// AppContext.cpp — 생성자 첫 줄
AppContext::AppContext(const AppConfig& config) {
    source_ = std::make_shared<RtspOnvifSourceV2>(config);   // 가장 먼저 생성 (워커 즉시 가동)
    ...
}

// AppContext.h — 접근자
IMetadataSource& source() { return *source_; }
```

- **인터페이스로만 노출**: `AppContext`는 `std::shared_ptr<IMetadataSource>`로 보관하고 `IMetadataSource&`만 반환한다. `main`은 구현체 타입(`RtspOnvifSourceV2`)을 전혀 모른다. → 다른 Source로 교체해도 `main`은 그대로다. (DI 원칙)
- **선언 순서와 파괴 순서**: `AppContext`의 멤버 선언은 `source_` → `transport_` → `pipeline_` 순이며, 파괴는 그 **역순**(`pipeline_` → `transport_` → `source_`)이다. Source가 마지막에 파괴되므로, 파이프라인이 정리되는 동안에도 Source 객체는 유효하다.

### 3.2 소비 (`main.cpp`)

```cpp
// 시그널 스레드에 source 참조를 넘김 -> SIGINT/SIGTERM 시 stop() 호출
std::thread signalThread(waitForShutdownSignal, shutdownMask, std::ref(context->source()));

// 메인 루프: 하나의 RawPacket 을 재사용 (std::swap 이 capacity 를 보존)
domain::RawPacket raw;
while (context->source().next(raw)) {
    ++packetCount;
    try {
        context->pipeline().onPacket(raw);
    } catch (const std::exception& error) {
        logError(kIface, std::string("패킷 처리 중 예외 - 이 패킷을 건너뜁니다: ") + error.what());
    }
}
```

- **`raw`를 루프 밖에 선언**하는 것이 zero-copy 설계의 전제다. 매 반복 새로 만들면 스왑으로 돌려받은 capacity가 매번 버려진다.
- **`stop()`은 시그널 스레드에서** 호출된다 — 인터페이스가 요구한 "다른 스레드에서의 안전한 호출"이 실제로 이렇게 쓰인다. 시그널 핸들러가 아니라 `sigwait` 전용 스레드를 쓰는 이유는, 핸들러 안에서는 async-signal-safe 함수만 부를 수 있는데 `stop()`은 mutex/condition_variable을 만지기 때문이다.
- **스트림이 시그널 없이 먼저 끝난 경우**를 대비해, 루프 종료 후 `pthread_kill(signalThread, SIGTERM)`으로 감시 스레드를 회수한다.

---

## 4. Edge-Worker 원칙

- **채널 단일성**: 링 슬롯에 `slot.channelId = config_.channelId`를 박을 뿐, `channelCount`나 다른 카메라의 존재를 전혀 모른다. 전역 상태 없이 자기 채널 하나만 처리하는 엣지 워커다.
- **원본 바이트만 운반**: 이 계층이 나르는 것은 카메라 **원본 메타데이터 바이트**이며(디코딩된 영상이 아님), 좌표 해석·월드 변환은 전부 하류의 몫이다.
- **graceful shutdown 보장**: `stop()` → `cancel()` → `shutdown(SHUT_RDWR)` 경로가 있어야 SIGINT/SIGTERM에 즉시 응답하고 **MQTT 종료 신호(dead)** 를 정상 발행할 수 있다. 이게 없으면 systemd가 SIGKILL로 강제 종료해, control-server가 채널의 생사를 구분하지 못한다.