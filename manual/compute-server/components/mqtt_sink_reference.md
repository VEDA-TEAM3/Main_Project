# MQTT Sink 레퍼런스

> **대상 파일**
> - Sink 공통 기반: `src/sink/MqttFrameSink.h`
> - TopView Sink: `src/sink/MqttTopViewSink.h`, `.cpp`
> - Blur Sink: `src/sink/MqttBlurSink.h`, `.cpp`

Compute Server가 만든 TopView/Blur frame을 검증하고 bounded queue를 통해 비동기로 발행하는
출력 계층이다. MQTT 연결, TLS, 재접속과 client 수명은
[MQTT Transport 레퍼런스](./mqtt_transport_reference.md)에서 다룬다.

`MqttTopViewSink`와 `MqttBlurSink`는 하나의 Transport를 공유하지만 queue와 worker는 각각
소유한다. Blur 발행 지연이 TopView 경로를 직접 막지 않도록 발행 경로를 격리한다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-28 | 1.0.0 | DevSunbi | 비동기 Sink, bounded queue, frame 검증, 직렬화, drop 정책 및 동시성 계약 명세 |

---

## 1. Sink 공통 기반: `MqttFrameSink<T>`

### 1.1 Thread 모델

| 실행 주체 | 작업 |
|-----------|------|
| Pipeline thread | `send()`, frame 검증, staging 작성, queue 삽입 |
| Sink worker | queue pop, JSON 직렬화, `publish()` |
| Mosquitto network thread | 연결 listener 호출 |

TopView와 Blur는 각각 queue/worker를 하나씩 가진다.

### 1.2 시작

`start()`는 atomic `started_` CAS로 중복 호출을 무시한다.

```text
started_ false -> true
-> connection listener 등록
-> worker thread 생성
```

thread 생성이 실패하면 listener를 제거하고 `started_`를 false로 복구한 뒤 예외를 다시
던진다. listener만 남고 worker가 없는 반쪽 초기화를 방지한다.

이미 `shutdown()`된 Sink는 다시 시작하지 않는다.

### 1.3 `send()` 예외 경계

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

### 1.4 Bounded queue와 drop-oldest

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

### 1.5 Worker 대기와 연결 상태

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

### 1.6 직렬화 payload 상한

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

### 1.7 Drop 집계

drop counter는 atomic이며 첫 건과 100건마다 로그를 남긴다.

```text
1, 100, 200, 300, ...
```

frame마다 같은 오류를 출력해 synchronous log I/O 병목을 만드는 것을 방지한다.

`recordDrop()` 내부의 문자열 조립도 allocation할 수 있으므로 try/catch로 감싼다. 메모리
부족 상황에서도 drop accounting 자체는 유지한다.

### 1.8 종료와 파생 class 수명

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

## 2. TopView Sink

### 2.1 Topic과 QoS

```text
topic  = veda/ch/<channelId>/topview
QoS    = veda::qos::kTopView
retain = false
```

topic은 생성 시 한 번 계산해 멤버에 저장한다.

### 2.2 Frame 검증

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

### 2.3 Exact channel invariant

Sink topic은 `config.channelId`로 고정되므로 payload channel도 반드시 같아야 한다.

```text
topic channel == payload frame.ch == config.channelId
```

단순히 `0 <= frame.ch < channelCount`만 검사하면 채널 2 process가 채널 1 payload를
`veda/ch/2/topview`에 발행할 수 있다. 수정 구현은 exact equality를 강제한다.

### 2.4 Empty frame

object가 0개인 frame도 정상이다. 이는 “해당 시각에 위험 객체가 없음”을 의미한다.
frame 자체를 보내지 않는 것과 다르다. 채널 연결 여부는 별도 alive topic으로 전달한다.

---

## 3. Blur Sink

### 3.1 Topic과 QoS

```text
topic  = veda/ch/<channelId>/blur
QoS    = veda::qos::kBlur
retain = false
```

### 3.2 Frame 검증

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

### 3.3 부분 필터링

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

## 4. 메모리와 동적 할당

### 4.1 할당 지점

| 경로 | 할당 가능성 | 재사용 여부 |
|------|-------------|-------------|
| Listener 등록 | vector/function | 시작 시 |
| Sink staging vector | frame element 수에 따라 | 현재 queue로 move되어 반환되지 않음 |
| `std::deque` queue | block/element | queue 상태에 따라 |
| `payloadBuf_` | 직렬화 크기 증가 시 | worker가 capacity 재사용 |

### 4.2 Staging capacity가 재사용되지 않는 이유

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

### 4.3 현재 hard limit

| 항목 | 상한 |
|------|------|
| TopView objects/frame | 256 |
| Blur targets/frame | 256 |
| Serialized payload | 1 MiB |
| Sink queue frame count | `AppConfig`의 Sink별 queue size |

향후 queue 총 byte 상한을 추가할 수 있다.

---

## 5. I/O와 성능 특성

### 5.1 비동기 경계

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

### 5.2 잠재 병목

- TopView/Blur가 공유하는 `clientMutex_`
- 큰 frame 직렬화
- disconnect 후 queue 누적과 reconnect burst
- 동기 log 출력

### 5.3 적용된 최적화

- TLS/MQTT connection 공유
- Sink별 worker와 bounded queue
- cached topic
- reusable serialization buffer
- debug level 확인 후 성공 로그 문자열 조립
- atomic connection state
- drop log rate limit

### 5.4 분기 순서

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

### 5.5 운영 지표

권장 metric:

- Sink별 queue depth/high-water mark
- published/drop count와 drop reason
- payload byte histogram
- process RSS
- allocations/frame

---

## 6. 오류와 복구 동작

| 상황 | 동작 |
|------|------|
| 잘못된 frame | Sink에서 drop |
| element 상한 초과 | frame 전체 drop |
| 잘못된 BlurTarget | 해당 target만 제외 |
| payload 1 MiB 초과 | publish 전 drop |
| Transport 미준비 | 새 frame drop |
| Broker 연결 끊김 | bounded queue에 최신 frame 유지 |
| Queue full | oldest frame drop |

---

## 7. 테스트 기준

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

권장 추가 테스트:

- BlurTarget 256/257 boundary
- serialized payload 1 MiB 직전/직후
- allocation failure injection
- TSAN queue/listener/shutdown stress
- 장시간 RSS와 queue high-water

---

## 8. Edge-Worker 원칙

- **채널 단일성**: Compute Server process는 자기 `channelId` 하나만 발행한다.
- **Topic/payload 일치**: topic channel과 payload `frame.ch`는 반드시 동일하다.
- **연결 공유, queue 격리**: Transport는 공유하고 TopView/Blur queue는 분리한다.
- **실시간 우선**: queue full이면 오래된 frame을 버리고 최신 frame을 유지한다.
- **상태와 데이터 분리**: 빈 frame은 정상 데이터이며 채널 생사는 alive topic으로 전달한다.
- **제한된 자원**: frame element, payload 및 queue에 상한을 둔다.
- **Fail closed**: 잘못된 schema/channel/좌표는 broker에 전달하지 않는다.
- **Graceful shutdown**: 파생 class 상태가 파괴되기 전에 Sink worker를 종료한다.

---

## 9. 남은 설계 부채

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| Frame당 staging allocation | queue로 move되어 반복 가능 | Sink별 Frame Pool + fixed ring |
| Queue byte budget | frame 개수만 제한 | `maxQueuedBytes` 추가 |
| Runtime 관측성 | count/log 중심 | queue/RSS/latency metric |
| Allocation failure test | 정적 분석 중심 | failure-injection test |
| 동시성 검증 | lifecycle 회귀 테스트 | TSAN stress |

> Memory Pool이나 PMR arena는 “동적 할당을 줄이기 위해 무조건 도입”하지 않는다.
> 현재 frame rate와 allocation profile을 먼저 측정하고, 적용 시에는 전역 공용 pool보다
> **Sink별 fixed Frame Pool**을 사용해 장애와 경합을 격리한다.
