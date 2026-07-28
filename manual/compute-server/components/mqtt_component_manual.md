# Compute Server MQTT Component Manual

## 1. Purpose

이 문서는 Compute Server의 MQTT 전송 계층과 TopView/Blur Sink의 구조, 실행 흐름,
동시성 계약, 자원 사용 및 운영 절차를 설명한다.

대상 파일:

- `IMqttTransport.h`
- `MqttTransport.cpp`
- `MqttFrameSink.h`
- `MqttTopViewSink.h`, `MqttTopViewSink.cpp`
- `MqttBlurSink.h`, `MqttBlurSink.cpp`

실제 통합 시에는 `Main_Project/compute-server/src/mqtt/MqttTransport.h`,
`AppConfig`, `Contract.h`, `AppContext`, `Pipeline`도 함께 참조해야 한다.

## 2. Responsibilities

### IMqttTransport

MQTT 연결과 발행 기능을 Sink에서 분리한 interface다.

주요 계약:

- `start()`는 client 초기화와 비동기 연결을 시작한다.
- `publish()`는 mosquitto 내부 queue에 메시지를 전달한다.
- `isReady()`는 client와 network loop가 준비됐는지 나타낸다.
- `isConnected()`는 broker 연결 상태를 나타낸다.
- 연결 listener는 mosquitto network thread에서 호출된다.
- `isReady()`와 `isConnected()`는 lock-free여야 한다.

### MqttTransport

하나의 mosquitto client를 소유하며 TopView와 Blur Sink가 공유한다.

담당 기능:

- TLS CA를 적용한 MQTT 연결
- 최초 연결 실패 시 background retry
- 연결 이후 mosquitto 자동 reconnect delay 설정
- channel alive LWT 등록
- 정상 연결 시 retained `"1"` 발행
- 정상 종료 시 retained `"0"` 발행
- client lifetime과 mosquitto global library lifetime 관리
- 연결 상태 listener 관리

### MqttFrameSink

TopView/Blur가 공유하는 bounded queue와 worker thread를 제공한다.

담당 기능:

- pipeline thread에서 frame 검증 및 queue 삽입
- queue가 가득 찼을 때 drop-oldest
- 연결된 경우에만 worker를 깨워 publish
- reusable payload buffer를 사용한 직렬화
- publish/drop counter와 rate-limited drop logging
- listener 해제, queue 비우기, worker join을 포함한 종료 처리

### MqttTopViewSink

TopView frame의 schema, timestamp, channel, class 및 유한 좌표를 검증한다.
유효한 frame을 `veda/ch/<channel>/topview` 계열 topic에 발행한다.

### MqttBlurSink

Blur frame의 schema, timestamp 및 channel을 검증한다. 개별 BlurTarget은 class, finite
coordinate, normalized range 및 rectangle 방향을 검사한다. 잘못된 target 하나 때문에
frame 전체를 폐기하지 않고 해당 target만 제외한다.

## 3. Architecture

```text
RTSP/ONVIF input
    |
    v
Pipeline
    |-----------------------------|
    v                             v
TopViewFrame                  BlurFrame
    |                             |
    v                             v
MqttTopViewSink              MqttBlurSink
  bounded queue                bounded queue
  worker thread                worker thread
    |                             |
    +---------- MqttTransport ----+
                   |
                   v
               TLS MQTT
                   |
                   v
                Broker
```

두 Sink는 connection을 공유하지만 queue와 worker는 공유하지 않는다. Blur 처리 지연이
TopView 발행까지 직접 막지 않도록 하기 위한 구조다. 최종 `mosquitto_publish()` 호출은
`clientMutex_`에서 직렬화된다.

## 4. Startup Sequence

`AppContext`의 정상 순서는 다음과 같다.

1. `MqttTransport` 생성
2. `MqttTopViewSink`, `MqttBlurSink` 생성
3. 각 Sink의 `start()` 호출
4. listener 등록
5. Sink worker 시작
6. `MqttTransport::start()` 호출
7. client 설정, LWT 설정, TLS 설정
8. 비동기 broker 연결과 mosquitto loop 시작
9. `onConnect()`에서 connected flag 갱신
10. retained alive `"1"` 발행
11. listener를 통해 대기 중인 Sink worker 깨움

Sink listener를 transport보다 먼저 등록해야 최초 연결 이벤트를 놓치지 않는다.

## 5. Frame Processing

### TopView

```text
send(frame)
-> schema version 검사
-> timestamp > 0 검사
-> frame.ch == configured channelId 검사
-> 각 object class 검사
-> 각 position finite 검사
-> staging frame 생성
-> bounded queue 삽입
-> worker 직렬화
-> publish
```

TopView는 object 하나가 잘못되면 frame 전체를 drop한다. downstream risk 판단에서 부분
frame을 정상 frame으로 오인하지 않도록 하는 정책이다.

### Blur

```text
send(frame)
-> schema version 검사
-> timestamp > 0 검사
-> frame.ch == configured channelId 검사
-> 각 target class/box 검사
-> 잘못된 target만 제외
-> 빈 blur list도 정상 frame으로 queue 삽입
-> worker 직렬화
-> publish
```

빈 BlurFrame은 이전 화면의 blur 영역을 지우는 의미가 있으므로 정상적으로 발행한다.

## 6. Channel Invariant

각 Compute Server process는 하나의 `config.channelId`만 담당한다. Sink topic은 생성자에서
이 ID로 한 번 계산된다.

```text
topic channel == frame.ch == config.channelId
```

수정 구현은 `frame.ch != channelId_`인 frame을 거부한다. 단순히 전체 채널 범위 안에
있는지만 검사하면 payload channel과 topic channel이 달라질 수 있으므로 범위 검사를
동등성 검사로 대체하면 안 된다.

## 7. Queue Policy

각 Sink queue는 `maxQueueSize_`로 제한된다.

- 최소 크기: 1
- full policy: oldest frame 제거
- 이유: 실시간 위치/blur 정보는 오래된 frame보다 최신 frame이 중요함
- disconnected 상태: 이미 준비된 transport라면 queue에 보관
- not-ready 상태: frame을 즉시 drop
- shutdown 상태: queue를 비우고 worker 종료

queue element 수는 제한되지만 frame 내부 object/blur 개수에는 이 컴포넌트 자체의 상한이
없다. upstream contract 상한과 Sink의 방어 상한을 함께 두는 것이 권장된다.

## 8. Threading Model

| Thread | Operations |
|---|---|
| Pipeline thread | `send()`, `prepare()`, queue insertion |
| TopView worker | TopView serialization and publish |
| Blur worker | Blur serialization and publish |
| Mosquitto network thread | connect/disconnect callbacks |
| Retry thread | initial connection retry |

### Lock order

주요 mutex:

- `queueMutex_`: 각 Sink queue와 stopping flag
- `clientMutex_`: mosquitto client pointer와 publish/destroy 상호 배제
- `listenerMutex_`: listener 목록과 listener callback 실행
- `retryMutex_`: retry condition variable
- global library mutex: mosquitto init/cleanup reference count

금지되는 구현:

```text
queueMutex_ 보유
-> isConnected()
-> transport 내부 mutex 획득
```

`isConnected()`는 queue condition predicate 안에서 호출되므로 atomic load여야 한다.

listener callback은 `listenerMutex_`를 보유한 network thread에서 호출되고 짧게
`queueMutex_`를 획득한다. callback 안에 network I/O, logging loop 또는 긴 계산을
추가하지 않는다.

## 9. Resource Lifetime

### Mosquitto library

`mosquitto_lib_init()`과 `mosquitto_lib_cleanup()`은 process global state를 다룬다.
여러 transport가 생기더라도 reference count가 0에서 1로 바뀔 때 init하고 1에서 0으로
바뀔 때 cleanup한다.

### Client

client는 local pointer 상태에서 callback, reconnect, LWT, TLS, connect 설정을 완료한 뒤
`clientMutex_` 아래에서 `client_`에 공개된다.

종료 시:

1. `clientMutex_` 아래에서 `client_`를 null로 교체
2. local pointer로 disconnect
3. loop stop
4. destroy
5. atomic state false
6. mosquitto library reference 반환

이 순서는 publish가 파괴 중인 client를 사용하는 것을 방지한다.

### Sink

파생 Sink 소멸자는 반드시 `shutdown()`을 먼저 호출한다. worker가 가상 함수와 파생 멤버를
사용하므로 기반 class 소멸까지 미루면 pure virtual call 또는 use-after-free 위험이 있다.

## 10. Dynamic Allocation

시작 시 발생 가능한 할당:

- config string 복사
- client ID와 alive topic 생성
- listener vector 확장
- `std::thread` state 생성
- mosquitto/OpenSSL 내부 allocation

frame당 발생 가능한 할당:

- `staging_`의 TopView object 또는 BlurTarget vector
- queue deque block 또는 element
- 최초 또는 증가된 payload serialization buffer
- `MqttTransport::publish()`의 topic string 복사
- debug/error log string 생성

`payloadBuf_`는 worker 소유라 `clear()` 이후 capacity를 재사용한다. 반면 `staging_`의
vector buffer는 queue로 move되어 다음 `prepare()`에 돌아오지 않으므로 일반적으로
frame마다 allocation이 발생한다.

## 11. Performance Characteristics

### Hot paths

- `MqttTopViewSink::isValidFrame`: O(object count)
- `MqttBlurSink::prepare`: O(blur count)
- `veda::encodeInto`: O(serialized payload size)
- queue push/pop: amortized O(1)
- `mosquitto_publish`: payload copy/queue 비용은 libmosquitto 정책에 의존

### Existing optimizations

- topic은 Sink 생성 시 한 번 계산
- payload serialization string capacity 재사용
- queue size 제한
- shared TLS/MQTT connection
- topic string allocation을 `clientMutex_` 밖으로 이동
- 정상 publish log의 문자열 조립을 debug level 검사 뒤 수행
- connected/ready 상태는 atomic lock-free read

### Branch behavior

정상 frame에서 잘 예측되는 조건:

- 올바른 schema와 timestamp
- 일치하는 channel
- 정상 class 및 finite coordinate
- queue가 full이 아님
- transport가 connected

오류 branch는 드물게 발생하는 fail-closed 경로다. 현재 조건 배치는 저비용 frame-level
검사를 먼저 하고 object loop를 나중에 수행하므로 합리적이다. `[[likely]]`나
`[[unlikely]]`는 실제 Raspberry Pi profile 없이 추가하지 않는다. 컴파일러의 PGO가
수동 hint보다 안전한 선택이다.

## 12. I/O Behavior

`send()`는 socket I/O를 직접 수행하지 않는다. queue 삽입 후 반환한다.

실제 MQTT I/O는:

- Sink worker가 `mosquitto_publish()`를 호출해 libmosquitto queue에 전달
- mosquitto network thread가 TLS socket에 기록

잠재 병목:

- 두 Sink가 공유하는 `clientMutex_`
- libmosquitto outgoing queue
- broker 또는 network backpressure
- QoS에 따른 inflight message
- 큰 payload serialization
- disconnect 동안 Sink queue 누적과 reconnect 후 burst

운영 metric 권장:

- Sink별 queue depth/high-water mark
- published/drop count 및 drop reason
- payload bytes histogram
- `publish()` latency와 mutex wait time
- reconnect count와 disconnected duration
- mosquitto publish return code
- process RSS와 allocation rate

## 13. Configuration

필수 또는 핵심 설정:

| Setting | Meaning | Validation |
|---|---|---|
| `mqttHost` | broker host/IP | 비어 있으면 실패 |
| `mqttPort` | TLS MQTT port | 1..65535 |
| `mqttCaFile` | CA bundle | 비어 있으면 실패 |
| `mqttClientId` | client ID | 비면 자동 생성 |
| `mqttKeepAliveSeconds` | keepalive | 양수 권장 |
| `mqttRetryIntervalMs` | initial retry | 양수 및 상한 권장 |
| `mqttReconnectDelaySec` | reconnect min | 양수 권장 |
| `mqttReconnectDelayMaxSec` | reconnect max | min 이상 권장 |
| `channelId` | process channel | deployment channel과 일치 |
| queue sizes | Sink별 backlog | 최소 1, memory budget 기반 |

TLS hostname/IP SAN 검증은 활성화되어 있으며 `tls_insecure=false`다.

## 14. Operational Checklist

시작 전:

- CA 파일 존재와 권한 확인
- broker certificate의 SAN과 설정 host 일치 확인
- channel ID와 camera 배치 일치 확인
- broker ACL이 해당 channel topic publish만 허용하는지 확인
- queue size와 최대 frame 크기로 최악 RSS 계산

실행 중:

- alive topic retained 값 확인
- reconnect와 drop count 감시
- queue full drop 증가 시 broker/network 병목 조사
- RSS가 시간에 따라 계속 증가하는지 확인
- payload size와 object count 이상치 감시

종료 시:

- retained `"0"` 발행 여부 확인
- worker join 지연 확인
- listener가 제거됐는지 확인
- mosquitto loop가 정상 종료되는지 확인

## 15. Extension Rules

새 Sink 추가 시:

1. `MqttFrameSink<T>`를 상속한다.
2. topic과 QoS를 생성 시 고정한다.
3. `prepare()`에서 schema, timestamp, exact channel 및 element를 검증한다.
4. element count 상한을 둔다.
5. 소멸자에서 `shutdown()`을 호출한다.
6. listener callback은 notify 이외의 긴 작업을 하지 않는다.
7. `isConnected()`를 mutex 기반으로 구현하지 않는다.
8. `noexcept` 함수 안에서 allocation 가능 연산을 수행하지 않는다.
9. 정상 frame, 다른 유효 채널, oversized frame, disconnect/reconnect, shutdown race를
   테스트한다.

