# MQTT 모듈 레퍼런스 (전송 계층 & 비동기 Sink)

> **대상 파일**
> - 인터페이스: `include/interfaces/IMqttTransport.h`
> - 전송 구현체: `src/mqtt/MqttTransport.h`, `.cpp`
> - Sink 공통 기반: `src/sink/MqttFrameSink.h`
> - TopView Sink: `src/sink/MqttTopViewSink.h`, `.cpp`
> - Blur Sink: `src/sink/MqttBlurSink.h`, `.cpp`

Compute Server가 만든 TopView/Blur frame을 **TLS MQTT로 외부에 발행하는 출력 계층**이다.

이 모듈은 두 책임으로 나뉜다.

- `MqttTransport`: MQTT 연결, TLS, 재접속, LWT, client 수명
- `MqttFrameSink<T>`: frame 검증, bounded queue, worker, 직렬화, drop 정책

`MqttTopViewSink`와 `MqttBlurSink`는 하나의 Transport를 공유하지만 queue와 worker는 각각
소유한다. Blur 발행 지연이 TopView 경로를 직접 막지 않으면서도 TLS connection과
mosquitto network thread는 하나만 사용하기 위한 구조다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-28 | 1.0.0 | DevSunbi | MQTT Transport/Sink 책임, 연결 수명주기, LWT, bounded queue, 검증 상한 및 동시성 계약 명세 |

---

## 1. 인터페이스 명세 (`IMqttTransport`)

```cpp
class IMqttTransport {
public:
    using ListenerId = std::uint64_t;
    static constexpr ListenerId kInvalidListener = 0;

    virtual ~IMqttTransport() = default;

    virtual ListenerId addConnectionListener(
        std::function<void(bool)> listener) = 0;
    virtual void removeConnectionListener(ListenerId id) noexcept = 0;

    virtual bool start() noexcept = 0;
    virtual void stop() noexcept = 0;

    virtual bool publish(
        std::string_view topic,
        std::string_view payload,
        int qos,
        bool retain = false) noexcept = 0;

    virtual bool isReady() const noexcept = 0;
    virtual bool isConnected() const noexcept = 0;
};
```

| 항목 | 계약 내용 |
|------|-----------|
| **역할 분담** | Sink는 무엇을 언제 보낼지 결정하고, Transport는 TLS/MQTT로 어떻게 보낼지 담당 |
| **연결 공유** | TopView/Blur Sink가 하나의 Transport와 mosquitto client를 공유 |
| **`publish()` 의미** | broker 전달 완료가 아니라 libmosquitto가 발행 요청을 받아들였음을 의미 |
| **Listener thread** | 연결 listener는 mosquitto network thread에서 호출되므로 오래 블로킹하면 안 됨 |
| **Listener 제거** | `removeConnectionListener()` 반환 뒤에는 해당 callback이 실행 중이거나 다시 호출되면 안 됨 |
| **상태 조회** | `isReady()`와 `isConnected()`는 mutex 없이 atomic load로 구현해야 함 |
| **오류 계약** | `publish()`는 예외를 외부로 던지지 않고 실패 시 `false` 반환 |

### 1.1 `ready`와 `connected`의 차이

```text
ready     = mosquitto client 생성 + network loop 시작 완료
connected = MQTT broker와 실제 연결 완료
```

`ready == true`, `connected == false` 상태가 정상적으로 존재한다. 예를 들어 broker가 일시
중단됐거나 재접속 backoff 중인 경우다.

Sink는 Transport가 아직 준비되지 않았으면 새 frame을 queue에 쌓지 않고 drop한다. 이미
준비된 이후 연결만 끊긴 경우에는 bounded queue에 최신 frame을 유지하며 재연결을 기다린다.

### 1.2 상태 조회가 lock-free여야 하는 이유

Sink worker는 `queueMutex_`를 보유한 상태에서 condition variable 술어를 평가한다.

```cpp
queueChanged_.wait(lock, [this] {
    return stopping_ ||
           (transport_->isConnected() && !queue_.empty());
});
```

연결 callback은 Transport의 listener lock을 보유한 상태에서 Sink의 `queueMutex_`를
획득한다. 따라서 `isConnected()`가 Transport 내부 mutex를 획득하면 다음 lock cycle이
생길 수 있다.

```text
Sink worker:
queueMutex_ -> transport mutex

Mosquitto network thread:
transport/listener mutex -> queueMutex_
```

`MqttTransport`는 `std::atomic_bool::load()`만 사용해 이 계약을 지킨다.

---

## 2. 구현체 분석: `MqttTransport`

### 2.1 설정과 고정 상태

생성자는 `AppConfig`에서 다음 값을 복사한다.

| 설정 | 용도 |
|------|------|
| `mqttHost` | broker host 또는 IP |
| `mqttPort` | TLS MQTT port |
| `mqttCaFile` | server certificate 검증용 CA |
| `mqttClientId` | MQTT client ID, 비면 자동 생성 |
| `mqttKeepAliveSeconds` | MQTT keepalive |
| `mqttRetryIntervalMs` | 최초 연결 준비 실패 시 재시도 간격 |
| `mqttReconnectDelaySec` | 연결 이후 reconnect 최소 delay |
| `mqttReconnectDelayMaxSec` | reconnect 최대 delay |
| `channelId` | retained alive topic 생성 |

client ID가 비어 있으면 steady clock tick을 사용해 다음 형태로 만든다.

```text
veda-compute-<ticks>
```

alive topic도 생성 시 한 번 계산한다.

```text
veda/ch/<channelId>/alive
```

### 2.2 Mosquitto 전역 라이브러리 수명

`mosquitto_lib_init()`과 `mosquitto_lib_cleanup()`은 process global state를 다룬다.
Transport 인스턴스마다 무조건 init/cleanup하면 한 인스턴스가 다른 인스턴스의 전역 상태를
먼저 정리할 수 있다.

```cpp
std::mutex g_libraryMutex;
std::size_t g_libraryRefCount = 0;
```

```text
0 -> 1 : mosquitto_lib_init()
1 -> 0 : mosquitto_lib_cleanup()
```

참조 횟수와 global init/cleanup은 같은 mutex로 보호한다.

### 2.3 시작과 최초 연결 재시도

```cpp
bool MqttTransport::start() noexcept {
    std::lock_guard<std::mutex> startLock(retryMutex_);

    if (stopping_) return false;
    if (ready_) return true;
    if (retryThread_.joinable()) return false;

    if (initializeClient()) return true;

    retryThread_ = std::thread(&MqttTransport::retryLoop, this);
    return false;
}
```

중복 `start()`는 새 client/thread를 만들지 않는다.

`initializeClient()`가 실패하면 별도 retry thread가 `mqttRetryIntervalMs`마다 다시 시도한다.
한 번 client와 network loop가 만들어진 뒤의 연결 끊김은 libmosquitto의 reconnect 기능이
담당한다.

```text
최초 client 준비 실패 -> retryLoop()
연결 성공 후 disconnect -> libmosquitto reconnect
```

> `start()`의 `false`는 process 전체의 치명적 시작 실패를 뜻하지 않는다. background
> retry를 시작했을 수 있으므로 `isReady()`와 로그를 함께 확인해야 한다.

### 2.4 Client 초기화 순서

초기화 중인 client를 `publish()`가 보지 못하도록 local pointer에서 모든 설정을 끝낸다.

```text
mosquitto library acquire
-> mosquitto_new
-> connect/disconnect callback
-> reconnect delay
-> LWT
-> MQTT 3.1.1
-> TLS CA
-> hostname/IP SAN 검증
-> connect_async
-> clientMutex_ 아래에서 client_ 공개
-> loop_start
-> ready = true
```

`loop_start()` 전에 `client_`를 공개하는 이유는 연결 callback이 빠르게 실행돼 alive
메시지를 발행할 때 동일 client를 찾을 수 있어야 하기 때문이다.

중간 단계가 실패하면 local client와 mosquitto library reference를 모두 정리한다.

### 2.5 TLS

```cpp
mosquitto_tls_set(
    client,
    caFile_.c_str(),
    nullptr,
    nullptr,
    nullptr,
    nullptr);

mosquitto_tls_insecure_set(client, false);
```

- CA를 사용해 broker certificate chain을 검증한다.
- `tls_insecure=false`이므로 hostname 또는 IP SAN도 검증한다.
- CA 경로가 비어 있으면 초기화를 거부한다.
- client certificate와 username/password 인증은 현재 Compute 구현 범위에 없다.

> TLS server 검증은 broker authorization을 대신하지 않는다. 운영 broker는 anonymous
> access를 끄고 Compute identity가 자기 channel topic만 publish하도록 ACL을 적용해야 한다.

### 2.6 LWT와 채널 생존 상태

payload:

```cpp
constexpr std::string_view kAlivePayload = "1";
constexpr std::string_view kDeadPayload = "0";
```

연결 전에 retained LWT `"0"`을 등록한다.

```cpp
mosquitto_will_set(
    client,
    aliveTopic_.c_str(),
    1,
    "0",
    veda::qos::kAlive,
    true);
```

연결 성공 callback에서는 retained `"1"`을 발행한다. 재연결 때도 다시 보내 broker에 남아
있던 `"0"`을 덮어쓴다.

정상 종료 시에는 broker가 keepalive 만료를 기다리지 않도록 직접 retained `"0"`을 보낸 뒤
client를 정리한다.

```text
비정상 종료/네트워크 단절 -> broker가 LWT "0"
정상 연결/재연결         -> client가 "1"
정상 종료                -> client가 "0"
```

Control Server는 이 topic으로 “빈 frame이 정상적으로 오는 상태”와 “채널이 죽어 frame이
오지 않는 상태”를 구분한다.

### 2.7 `publish()` 입력 검증

mosquitto 호출 전에 다음을 검사한다.

```cpp
if (topic.empty() ||
    topic.size() > 65535U ||
    qos < 0 || qos > 2 ||
    payload.size() > INT_MAX ||
    mosquitto_pub_topic_check2(topic.data(), topic.size())
        != MOSQ_ERR_SUCCESS) {
    return false;
}
```

| 검증 | 방어 목적 |
|------|-----------|
| 빈 topic | MQTT publish 불가 입력 차단 |
| 65,535 byte 상한 | MQTT UTF-8 topic length field 상한 |
| QoS `0..2` | 범위 밖 integer 차단 |
| payload `INT_MAX` | libmosquitto API의 `int payloadlen` 변환 overflow 차단 |
| `mosquitto_pub_topic_check2` | publish topic의 wildcard/형식 오류 차단 |

libmosquitto가 null-terminated topic을 요구하므로 `string_view`를 `std::string`으로
옮긴다. 이 allocation은 `clientMutex_` 밖에서 수행해 allocator 지연이 다른 Sink의
publish를 막지 않게 한다.

```cpp
std::string topicString;
try {
    topicString.assign(topic);
} catch (...) {
    return false;
}
```

allocation 실패도 `noexcept` 경계에서 process를 종료시키지 않고 `false`로 변환한다.

### 2.8 Client 수명과 publish 동기화

```cpp
std::lock_guard<std::mutex> lock(clientMutex_);
if (client_ == nullptr) return false;

mosquitto_publish(client_, ...);
```

`destroyClient()`는 같은 mutex 아래에서 pointer를 local로 옮기고 `client_`를 null로 만든다.

```cpp
mosquitto* client = nullptr;
{
    std::lock_guard<std::mutex> lock(clientMutex_);
    client = client_;
    client_ = nullptr;
}

mosquitto_disconnect(client);
mosquitto_loop_stop(client, true);
mosquitto_destroy(client);
```

따라서 publish와 client pointer 회수는 상호 배제된다. 실제 disconnect/loop stop/destroy는
mutex 밖에서 수행해 긴 종료 작업 동안 다른 thread가 lock에 묶이지 않게 한다.

### 2.9 Listener 수명

listener는 ID와 callback을 vector에 저장한다.

```cpp
struct Listener {
    ListenerId id;
    std::function<void(bool)> callback;
};
```

`notifyListeners()`는 listener mutex를 보유한 상태로 callback을 호출한다. callback 복사 후
lock 밖에서 호출하면 그 사이 Sink가 파괴돼 죽은 `this`를 호출할 수 있기 때문이다.

반대로 callback은 조건변수 wakeup 정도만 수행해야 한다. callback에서 network I/O나 긴
계산을 하면 listener 등록/제거와 mosquitto network thread를 함께 지연시킨다.

### 2.10 종료

```text
stopping_ CAS
-> retry condition wakeup
-> retry thread join
-> connected면 retained dead "0"
-> client pointer 회수
-> disconnect
-> network loop stop
-> client destroy
-> mosquitto library reference 반환
-> listener false 통지
```

`stop()`은 `stopping_` atomic CAS로 멱등성을 보장한다. 소멸자는 `stop()`을 호출한다.

---

## 3. Sink 공통 기반: `MqttFrameSink<T>`

### 3.1 Thread 모델

| 실행 주체 | 작업 |
|-----------|------|
| Pipeline thread | `send()`, frame 검증, staging 작성, queue 삽입 |
| Sink worker | queue pop, JSON 직렬화, `publish()` |
| Mosquitto network thread | 연결 listener 호출 |

TopView와 Blur는 각각 queue/worker를 하나씩 가진다.

### 3.2 시작

`start()`는 atomic `started_` CAS로 중복 호출을 무시한다.

```text
started_ false -> true
-> connection listener 등록
-> worker thread 생성
```

thread 생성이 실패하면 listener를 제거하고 `started_`를 false로 복구한 뒤 예외를 다시
던진다. listener만 남고 worker가 없는 반쪽 초기화를 방지한다.

이미 `shutdown()`된 Sink는 다시 시작하지 않는다.

### 3.3 `send()` 예외 경계

```cpp
void send(const T& frame) noexcept override {
    try {
        if (!prepare(frame, staging_)) {
            recordDrop("invalid frame");
            return;
        }
        // ready 확인, queue 삽입
    } catch (...) {
        recordDrop(...);
    }
}
```

`prepare()`는 vector `assign/reserve/push_back`으로 allocation할 수 있으므로 `noexcept`가
아니다. allocation 실패는 `send()`의 catch에서 frame drop으로 바뀐다.

이 예외 경계가 없다면 `std::bad_alloc`이 `noexcept`를 넘어 `std::terminate()`를 호출해
Compute Server 전체가 종료된다.

### 3.4 Bounded queue와 drop-oldest

```cpp
if (queue_.size() >= maxQueueSize_) {
    queue_.pop_front();
    recordDrop("queue full; oldest frame removed");
}
queue_.push_back(std::move(staging_));
```

- queue 최소 크기는 1
- full이면 가장 오래된 frame 제거
- 실시간 좌표는 오래된 값보다 최신 값이 우선
- queue 연산과 `stopping_`은 `queueMutex_`로 보호

frame 개수는 제한되지만 총 byte를 직접 계산하는 `maxQueuedBytes`는 아직 없다. 대신 개별
frame element 수와 직렬화 payload에 hard limit를 적용한다.

### 3.5 Worker 대기와 연결 상태

```cpp
queueChanged_.wait(lock, [this] {
    return stopping_ ||
           (transport_->isConnected() && !queue_.empty());
});
```

연결이 끊긴 동안 worker는 queue를 소비하지 않는다. 재연결 callback이 `queueChanged_`를
깨우면 최신 frame부터 다시 발행한다.

listener callback은 `queueMutex_`를 한 번 획득했다 놓은 뒤 notify한다. 술어 확인과
wait 진입 사이에 알림이 들어오는 lost wakeup을 막기 위한 순서다.

### 3.6 직렬화 payload 상한

worker는 reusable string에 직접 직렬화한다.

```cpp
veda::encodeInto(frame, payloadBuf_);

if (payloadBuf_.size() > 1024U * 1024U) {
    recordDrop("serialized payload too large");
    return;
}
```

payload 최대값은 **1 MiB**다. 상한 초과 frame은 mosquitto에 전달하지 않는다.

`payloadBuf_`는 worker thread만 사용하며 `encodeInto()`가 `clear()` 후 capacity를
재사용하므로 warm-up 이후 반복 allocation을 줄인다.

### 3.7 Drop 집계

drop counter는 atomic이며 첫 건과 100건마다 로그를 남긴다.

```text
1, 100, 200, 300, ...
```

frame마다 같은 오류를 출력해 synchronous log I/O 병목을 만드는 것을 방지한다.

`recordDrop()` 내부의 문자열 조립도 allocation할 수 있으므로 try/catch로 감싼다. 메모리
부족 상황에서도 drop accounting 자체는 유지한다.

### 3.8 종료와 파생 class 수명

파생 Sink 소멸자는 반드시 `shutdown()`을 먼저 호출한다.

```text
listener 제거
-> stopping_ = true
-> queue 비우기
-> worker wakeup
-> worker join
```

worker가 `describe()` 같은 virtual method와 파생 class 상태를 사용하므로 파생 멤버가
파괴된 뒤 worker가 남아 있으면 pure virtual call 또는 use-after-free가 발생할 수 있다.

listener 제거를 queue lock보다 먼저 수행해 다음 역순 교착도 막는다.

```text
shutdown thread: queueMutex_ -> listenerMutex_
network thread : listenerMutex_ -> queueMutex_
```

---

## 4. TopView Sink

### 4.1 Topic과 QoS

```text
topic  = veda/ch/<channelId>/topview
QoS    = veda::qos::kTopView
retain = false
```

topic은 생성 시 한 번 계산해 멤버에 저장한다.

### 4.2 Frame 검증

검사 순서:

```text
schema version
-> timestamp > 0
-> frame.ch == config.channelId
-> object count <= 256
-> 모든 object class가 Human 또는 Vehicle
-> 모든 local position이 finite
```

TopView는 하나의 object라도 잘못되면 frame 전체를 drop한다. Control Server의 risk 계산이
부분적으로 손상된 frame을 정상 frame으로 오인하지 않도록 하는 정책이다.

### 4.3 Exact channel invariant

Sink topic은 `config.channelId`로 고정되므로 payload channel도 반드시 같아야 한다.

```text
topic channel == payload frame.ch == config.channelId
```

단순히 `0 <= frame.ch < channelCount`만 검사하면 채널 2 process가 채널 1 payload를
`veda/ch/2/topview`에 발행할 수 있다. 수정 구현은 exact equality를 강제한다.

### 4.4 Empty frame

object가 0개인 frame도 정상이다. 이는 “해당 시각에 위험 객체가 없음”을 의미한다.
frame 자체를 보내지 않는 것과 다르다. 채널 연결 여부는 별도 alive topic으로 전달한다.

---

## 5. Blur Sink

### 5.1 Topic과 QoS

```text
topic  = veda/ch/<channelId>/blur
QoS    = veda::qos::kBlur
retain = false
```

### 5.2 Frame 검증

frame-level 검사:

```text
schema version
-> timestamp > 0
-> frame.ch == config.channelId
-> BlurTarget count <= 256
```

target-level 검사:

```text
class == Head 또는 LicensePlate
-> l/t/r/b 모두 finite
-> 모든 좌표가 [0,1]
-> l <= r, t <= b
```

### 5.3 부분 필터링

잘못된 BlurTarget 하나 때문에 같은 frame의 정상 얼굴/번호판까지 버리지 않는다.

```cpp
for (const auto& blur : in.blurs) {
    if (isValidBlurTarget(blur)) {
        out.blurs.push_back(blur);
    } else {
        recordDrop("invalid blur target skipped");
    }
}
```

빈 BlurFrame도 정상적으로 발행한다. Qt 화면에서 이전 frame의 blur rectangle을 지우려면
“현재 blur 대상 없음” 상태가 필요하기 때문이다.

---

## 6. 메모리와 동적 할당

### 6.1 할당 지점

| 경로 | 할당 가능성 | 재사용 여부 |
|------|-------------|-------------|
| Transport 생성 | config/client/topic string | 생성 시 1회 |
| Listener 등록 | vector/function | 시작 시 |
| Sink staging vector | frame element 수에 따라 | 현재 queue로 move되어 반환되지 않음 |
| `std::deque` queue | block/element | queue 상태에 따라 |
| `payloadBuf_` | 직렬화 크기 증가 시 | worker가 capacity 재사용 |
| publish topic string | 매 publish | 현재 반복 생성 |
| libmosquitto/OpenSSL | 내부 queue/TLS | library 정책 |

### 6.2 Staging capacity가 재사용되지 않는 이유

```text
staging_
-> move into queue
-> move into worker frame
-> worker frame이 다음 queue element로 교체
```

buffer가 producer의 `staging_`으로 되돌아오는 경로가 없으므로 `clear()`만으로 frame당
allocation을 없앨 수 없다.

완전한 재사용이 필요하면 다음 구조가 필요하다.

```text
Sink별 fixed Frame Pool
-> free slot 획득
-> slot frame 작성
-> ready queue에 slot index
-> worker publish
-> free pool로 slot 반환
```

공용 pool은 TopView/Blur 간 allocator lock 경합과 memory budget 침범을 만들 수 있으므로
Sink별 pool이 적합하다.

### 6.3 현재 hard limit

| 항목 | 상한 |
|------|------|
| TopView objects/frame | 256 |
| Blur targets/frame | 256 |
| Serialized payload | 1 MiB |
| MQTT topic | 65,535 bytes |
| MQTT payload API | `INT_MAX` |
| Sink queue frame count | `AppConfig`의 Sink별 queue size |

향후 queue 총 byte 상한과 libmosquitto outgoing queue metric을 추가할 수 있다.

---

## 7. I/O와 성능 특성

### 7.1 비동기 경계

`send()`는 socket I/O를 하지 않는다.

```text
Pipeline thread
-> bounded queue 삽입
-> 즉시 반환

Sink worker
-> JSON 직렬화
-> mosquitto_publish()

Mosquitto network thread
-> TLS socket I/O
```

### 7.2 잠재 병목

- TopView/Blur가 공유하는 `clientMutex_`
- 큰 frame 직렬화
- libmosquitto outgoing queue
- QoS inflight 제한
- broker/network backpressure
- disconnect 후 queue 누적과 reconnect burst
- 동기 log 출력

### 7.3 적용된 최적화

- TLS/MQTT connection 공유
- Sink별 worker와 bounded queue
- cached topic
- reusable serialization buffer
- topic allocation을 `clientMutex_` 밖으로 이동
- debug level 확인 후 성공 로그 문자열 조립
- atomic connection state
- drop log rate limit

### 7.4 분기 순서

저비용 frame-level 검사를 먼저 수행한다.

```text
schema/timestamp/channel/count
-> element loop
-> queue 상태
-> serialization
-> publish
```

정상 경로는 대부분 검사를 통과하고 오류 branch는 조기에 반환한다. `[[likely]]`와
`[[unlikely]]`는 실제 Raspberry Pi profile 없이 추가하지 않는다. branch hint가 필요하면
실제 workload 기반 PGO를 우선한다.

### 7.5 운영 지표

권장 metric:

- Sink별 queue depth/high-water mark
- published/drop count와 drop reason
- payload byte histogram
- publish latency와 `clientMutex_` wait
- reconnect count와 disconnected duration
- process RSS
- allocations/frame
- libmosquitto publish return code

---

## 8. 오류와 복구 동작

| 상황 | 동작 |
|------|------|
| 잘못된 frame | Sink에서 drop |
| element 상한 초과 | frame 전체 drop |
| 잘못된 BlurTarget | 해당 target만 제외 |
| payload 1 MiB 초과 | publish 전 drop |
| topic/QoS 오류 | Transport가 `false` 반환 |
| topic allocation 실패 | `false` 반환 |
| Transport 미준비 | 새 frame drop |
| Broker 연결 끊김 | bounded queue 유지, libmosquitto reconnect |
| Queue full | oldest frame drop |
| 최초 client 초기화 실패 | retry thread |
| 비정상 process 종료 | broker LWT `"0"` |
| 정상 종료 | client가 retained `"0"` 후 정리 |

---

## 9. 테스트 기준

현재 필수 회귀 시나리오:

| Test | Expected |
|------|----------|
| 연결 전 queue 후 연결 | worker가 깨어나 발행 |
| Queue 초과 | drop-oldest |
| Sink 파괴 후 연결 callback | listener 제거, UAF 없음 |
| 정상/비정상 BlurTarget 혼합 | 정상 target만 발행 |
| 다른 유효 channel | frame drop |
| TopView object 256개 | 발행 |
| TopView object 257개 | drop |
| 중복 Sink `start()` | listener/thread 추가 없음 |
| publish와 `stop()` 경합 | UAF/deadlock 없음 |
| TLS hostname 불일치 | 연결 거부 |
| Broker disconnect/reconnect | alive `"0"`/`"1"` 상태 정상화 |

권장 추가 테스트:

- BlurTarget 256/257 boundary
- serialized payload 1 MiB 직전/직후
- invalid topic/QoS
- allocation failure injection
- concurrent Transport `start()`
- TSAN publish/stop/listener stress
- 장시간 RSS와 queue high-water

---

## 10. Edge-Worker 원칙

- **채널 단일성**: Compute Server process는 자기 `channelId` 하나만 발행한다.
- **Topic/payload 일치**: topic channel과 payload `frame.ch`는 반드시 동일하다.
- **연결 공유, queue 격리**: Transport는 공유하고 TopView/Blur queue는 분리한다.
- **실시간 우선**: queue full이면 오래된 frame을 버리고 최신 frame을 유지한다.
- **상태와 데이터 분리**: 빈 frame은 정상 데이터이며 채널 생사는 alive topic으로 전달한다.
- **제한된 자원**: frame element, payload 및 queue에 상한을 둔다.
- **Fail closed**: 잘못된 schema/channel/좌표/topic/QoS는 broker에 전달하지 않는다.
- **Graceful shutdown**: Sink worker를 먼저 끝내고 retained dead 상태와 client 정리를 완료한다.

---

## 11. 남은 설계 부채

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| Frame당 staging allocation | queue로 move되어 반복 가능 | Sink별 Frame Pool + fixed ring |
| Topic 반복 복사 | `string_view`를 매번 string으로 변환 | owned topic 또는 Topic Handle |
| Queue byte budget | frame 개수만 제한 | `maxQueuedBytes` 추가 |
| Broker client 인증 | CA 기반 server 검증만 코드에서 확인 | mTLS 또는 channel별 credential |
| Broker ACL | 코드 밖 운영 설정 | channel별 publish allowlist |
| Runtime 관측성 | count/log 중심 | queue/RSS/latency metric |
| Allocation failure test | 정적 분석 중심 | failure-injection test |
| 동시성 검증 | lifecycle 회귀 테스트 | TSAN stress |

> Memory Pool이나 PMR arena는 “동적 할당을 줄이기 위해 무조건 도입”하지 않는다.
> 현재 frame rate와 allocation profile을 먼저 측정하고, 적용 시에는 전역 공용 pool보다
> **Sink별 fixed Frame Pool**을 사용해 장애와 경합을 격리한다.
