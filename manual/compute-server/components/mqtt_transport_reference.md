# MQTT Transport 레퍼런스

> **대상 파일**
> - 인터페이스: `include/interfaces/IMqttTransport.h`
> - 전송 구현체: `src/mqtt/MqttTransport.h`, `.cpp`

Compute Server의 MQTT 연결, TLS, 재접속, LWT 및 Mosquitto client 수명을 담당하는
전송 계층이다. Frame 검증과 비동기 발행 queue는
[MQTT Sink 레퍼런스](./mqtt_sink_reference.md)에서 다룬다.

`MqttTopViewSink`와 `MqttBlurSink`는 하나의 Transport와 Mosquitto network thread를 공유한다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-28 | 1.0.0 | DevSunbi | MQTT 연결 수명주기, TLS, LWT, publish 및 동시성 계약 명세 |

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

## 3. 오류와 복구 동작

| 상황 | 동작 |
|------|------|
| 잘못된 topic/QoS | `publish()`가 `false` 반환 |
| topic allocation 실패 | 예외를 전파하지 않고 `false` 반환 |
| 최초 client 초기화 실패 | retry thread에서 재시도 |
| Broker 연결 끊김 | libmosquitto reconnect 수행 |
| 비정상 process 종료 | broker가 LWT `"0"` 발행 |
| 정상 종료 | retained `"0"` 발행 후 client 정리 |

---

## 4. 테스트 기준

필수 회귀 시나리오:

| Test | Expected |
|------|----------|
| 잘못된 topic/QoS | publish 거부 |
| publish와 `stop()` 경합 | UAF/deadlock 없음 |
| 중복 `start()` | client/retry thread 추가 없음 |
| TLS hostname 불일치 | 연결 거부 |
| Broker disconnect/reconnect | alive `"0"`/`"1"` 상태 정상화 |
| Listener 제거와 callback 경합 | 제거 후 callback 재호출 없음 |

권장 추가 테스트는 allocation failure injection, concurrent `start()`, TSAN
publish/stop/listener stress 및 장시간 재접속 시험이다.

---
