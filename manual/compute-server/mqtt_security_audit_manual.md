# Compute Server MQTT Security Audit Manual

## 1. Scope

이 문서는 Compute Server의 MQTT Transport와 TopView/Blur Sink를 반복 감사하기 위한
절차와 현재 감사 결과를 제공한다.

검사 범위:

- 보안 취약점과 trust boundary
- OOM 및 자원 고갈
- stack overflow
- 성능 최적화
- I/O 병목
- 분기 예측
- 동적 할당
- 동시성, 종료 및 client lifetime

이 결과는 MQTT broker, 운영 방화벽, 실제 인증서, OS hardening 또는 전체 repository의
안전성을 보증하지 않는다.

## 2. Threat Model

### Untrusted or partially trusted inputs

- RTSP/ONVIF metadata에서 파생된 frame 내용
- object/blur count와 좌표
- camera 및 deployment channel 구성
- broker 응답, disconnect 및 reconnect timing
- network backpressure
- 잘못된 내부 component가 전달하는 topic, QoS 또는 frame

### Protected assets

- Compute Server process availability
- channel별 TopView/Blur 데이터 무결성
- 개인정보 blur 데이터의 정확한 귀속
- MQTT credential과 TLS channel
- Raspberry Pi의 제한된 RAM, CPU 및 thread 자원

### Security goals

1. 잘못된 frame이 다른 channel topic에 발행되지 않는다.
2. 입력 크기와 연결 장애가 무제한 memory/CPU 사용으로 이어지지 않는다.
3. malformed input이나 allocation 실패가 process termination으로 확대되지 않는다.
4. publish, disconnect, shutdown 경합이 UAF, deadlock 또는 double free를 만들지 않는다.
5. MQTT server identity가 TLS로 검증된다.

## 3. Audit Summary

| Area | Status | Highest risk |
|---|---|---|
| Channel integrity | Patched | Low residual |
| TLS transport | Good baseline | Broker ACL/auth outside scope |
| Queue bounds | Partially bounded | Frame element/byte bounds missing |
| OOM safety | Needs improvement | `noexcept` allocation terminates |
| Stack safety | Acceptable | No recursion/VLA, max measured frame 2064B |
| Concurrency | Good baseline | Repeated start and callback discipline need tests |
| I/O backpressure | Partially controlled | No byte-based queue budget/metrics |
| Branch efficiency | Reasonable | Profile data absent |
| Dynamic allocation | Known hot allocations | Staging vector and topic copy |

### Applied hardening status

GitHub tracking issue:

- `VEDA-TEAM3/Main_Project#54`
- https://github.com/VEDA-TEAM3/Main_Project/issues/54

`Main_Proejct_edited`에 다음 P0/P1 항목을 적용했다.

| Finding | Applied change | Status |
|---|---|---|
| MQTT-SEC-01 | `prepare()`의 `noexcept` 제거, `send()` catch 안에서 실행 | Applied |
| MQTT-SEC-01/04 | TopView object 및 BlurTarget 최대 256개 | Applied |
| MQTT-SEC-02 | topic 문자열 할당 예외를 `false`로 변환 | Applied |
| MQTT-SEC-03 | empty/length/wildcard topic 및 QoS `0..2` 검사 | Applied |
| MQTT-SEC-04 | serialized payload 최대 1 MiB 검사 | Applied |
| MQTT-SEC-05 | Sink/Transport의 중복 `start()` 방어 | Applied |
| OOM logging | `recordDrop()` 문자열 할당 실패가 terminate되지 않도록 방어 | Applied |
| Frame Pool | Sink별 고정 pool/ring 구조 | Not applied; structural follow-up |
| Queue byte budget | queue 총 소유 byte 상한 | Not applied; follow-up |
| Topic Handle | 매 publish topic 복사 제거 | Not applied; follow-up |
| Broker ACL/mTLS | 운영 broker 설정 | Not verified |

검증 결과:

- 일반 Compute Server build: PASS
- 경고 강화 + ASan/UBSan 계측 build: PASS
- Sink lifecycle/security regression: PASS 15 / FAIL 0
- 확인한 경계: 다른 유효 채널, object 256/257, 중복 Sink start
- 실제 broker I/O, allocator failure injection 및 TSAN: 아직 수행하지 않음

## 4. Security Findings

### MQTT-SEC-01: `prepare() noexcept` allocation termination

- **Severity:** Medium
- **CWE:** CWE-248, CWE-400
- **Affected:** Both original and modified implementations
- **Locations:**
  - `MqttFrameSink::prepare(...) noexcept`
  - `MqttTopViewSink::prepare`: vector `assign`
  - `MqttBlurSink::prepare`: vector `reserve` and `push_back`
- **Root cause:** allocation-capable vector operations execute inside a `noexcept` function.
- **Trigger:** large frame or general memory pressure causes `std::bad_alloc`.
- **Impact:** exception cannot reach the catch in `send()`; C++ calls `std::terminate()`.
- **Exploit likelihood:** Medium. Upstream now has some object count hardening, but memory pressure and
  future sources still make the condition credible on constrained hardware.
- **Recommended fix:** remove `noexcept` from the virtual method and overrides, or catch allocation
  failures inside each override. Add element count limits before allocation.
- **Applicability:** High. The caller already has a catch boundary.

### MQTT-SEC-02: `publish() noexcept` topic allocation termination

- **Severity:** Low to Medium
- **CWE:** CWE-248, CWE-400
- **Affected:** Both; modified code allocates even when `client_ == nullptr`
- **Location:** `MqttTransport::publish`
- **Root cause:** `std::string(topic)` may allocate in a `noexcept` function.
- **Trigger:** oversized topic or low memory.
- **Impact:** process termination.
- **Exploit likelihood:** Low in the current production path because topics are internal cached strings.
- **Recommended fix:** validate length, catch allocation failure, or expose an internal API accepting an
  already owned null-terminated topic.
- **Applicability:** High.

### MQTT-SEC-03: Missing topic and QoS validation

- **Severity:** Low
- **CWE:** CWE-20
- **Affected:** Both
- **Location:** `MqttTransport::publish`
- **Root cause:** only payload `INT_MAX` is checked.
- **Trigger:** empty/oversized topic, embedded NUL, wildcard publish topic, invalid UTF-8, QoS outside
  `0..2`.
- **Impact:** publish rejection, diagnostic ambiguity, excess allocation or topic confusion.
- **Exploit likelihood:** Low for current fixed call sites; higher if the interface is reused with
  external topic input.
- **Recommended fix:** use libmosquitto topic validation or equivalent strict validation and explicit
  QoS range checks.
- **Applicability:** High.

### MQTT-SEC-04: Queue count bound without byte budget

- **Severity:** Medium
- **CWE:** CWE-400, CWE-770
- **Affected:** Both
- **Location:** `MqttFrameSink::send`
- **Root cause:** `maxQueueSize_` limits frames, not total elements or bytes.
- **Trigger:** each queued frame contains a large object/blur vector.
- **Impact:** RSS spike or OOM despite a bounded frame count.
- **Exploit likelihood:** Medium if upstream count limits are bypassed or configured too high.
- **Recommended fix:**
  - contract-wide maximum elements per frame
  - estimated serialized byte limit
  - queue `maxQueuedBytes`
  - drop metric separating count and byte budget
- **Applicability:** Medium to High. Byte accounting requires careful ownership updates.

### MQTT-SEC-05: Repeated `start()` is not demonstrably idempotent

- **Severity:** Low
- **CWE:** CWE-362
- **Affected:** Both
- **Location:** `MqttTransport::start`, `MqttFrameSink::start`
- **Root cause:** no explicit started-state guard before creating client/listener/thread.
- **Trigger:** lifecycle misuse invokes `start()` twice.
- **Impact:** multiple clients/listeners, assignment to a joinable `std::thread`, termination or leaks.
- **Exploit likelihood:** Low; current `AppContext` calls start once.
- **Recommended fix:** add atomic lifecycle state and return existing state or reject repeated start.
- **Applicability:** High.

### MQTT-SEC-06: Broker authorization cannot be established from client code

- **Severity:** Environment-dependent, potentially High
- **CWE:** CWE-862, CWE-306
- **Affected:** Deployment
- **Location:** broker configuration outside reviewed files
- **Root cause:** TLS server verification protects confidentiality and server identity but does not prove
  per-channel publish authorization.
- **Trigger:** compromised client certificate/network credential or permissive anonymous broker.
- **Impact:** cross-channel publish and data forgery.
- **Exploit likelihood:** Cannot be determined without broker evidence.
- **Recommended fix:** broker ACL restricting each Compute identity to its own channel topics, disable
  anonymous access, prefer mTLS or unique credentials.
- **Applicability:** Depends on broker capabilities.

## 4.1 Finding-specific Improvement Designs

이 절은 각 발견사항을 실제 구현으로 옮길 때 선택할 수 있는 대안과 트레이드오프를
정리한다. 단순히 “상한을 추가한다”에서 끝내지 않고 memory pool, arena, API 변경 및 운영
통제를 함께 비교한다.

### MQTT-SEC-01 Improvements: `prepare() noexcept` and frame allocation

#### Option A: Remove `noexcept`

```cpp
virtual bool prepare(const T& in, T& out) = 0;
```

`send()`에 이미 `try/catch`가 있으므로 vector allocation 실패를 frame drop으로 변환할 수 있다.

- 장점: 변경량이 작고 기존 catch boundary 재사용
- 단점: interface와 모든 override를 함께 수정해야 함
- 적용 가능성: **Very High**
- 우선순위: **P0**

#### Option B: Catch inside `prepare()`

```cpp
bool prepare(const T& in, T& out) noexcept {
    try {
        // assign/reserve/push_back
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
```

- 장점: public interface 유지
- 단점: 구현체마다 catch가 반복되고 상세 실패 사유 전달이 어려움
- 적용 가능성: **High**
- 우선순위: interface 변경이 어려울 때 P0

#### Option C: Per-Sink fixed Frame Pool

```text
free slot 획득
-> slot vector에 frame 작성
-> ready queue에는 slot index만 저장
-> worker publish
-> clear 후 free pool로 반환
```

pool slot 수는 일반적으로 `maxQueueSize + staging 1 + worker 1`로 계산한다. 각 slot의
`objects` 또는 `blurs` vector를 시작 시 최대 element 수만큼 `reserve()`한다.

- 장점: frame당 vector allocation을 거의 제거하고 pool 고갈을 명시적 drop으로 전환
- 단점: free/ready queue, slot 반환 및 double-release 방어 필요
- 적용 가능성: **High**
- 우선순위: **P1**, 현재 구조에 가장 적합한 구조 개선

#### Option D: Per-slot PMR arena

각 ring slot이 독립적인 `std::pmr::monotonic_buffer_resource`를 소유한다. worker가 발행을
끝낸 후에만 `release()`한다.

```cpp
std::pmr::monotonic_buffer_resource arena{
    storage.data(),
    storage.size(),
    std::pmr::null_memory_resource()
};
```

`null_memory_resource()`를 upstream으로 사용하면 arena 부족 시 heap fallback을 금지할 수
있다.

- 장점: runtime heap allocation 0과 hard memory budget 가능
- 단점: `Contract.h`의 vector를 PMR-compatible type으로 변경하고 serializer도 수정해야 함
- 적용 가능성: **Medium**
- 우선순위: heap 금지가 필수 요구사항일 때 P2

#### Option E: Fixed-capacity container

`std::array<T, Max> + size` 또는 검증된 `static_vector`를 사용한다.

- 장점: element storage의 heap allocation을 완전히 제거
- 단점: Contract, encode/decode 및 downstream API 변경 범위가 큼
- 적용 가능성: **Medium**
- 우선순위: protocol 최대 element 수가 확정됐을 때 P2

#### Recommended combination

1. 즉시 Option A와 element count 상한 적용
2. 이후 Option C의 Sink별 Frame Pool 및 고정 ring queue 적용
3. 완전 무할당 요구가 있을 때만 Option D 또는 E 적용

검증 기준:

- allocator failure injection에서 process가 종료되지 않고 drop counter 증가
- pool 고갈에서 block 또는 heap fallback 없이 정의된 drop 정책 수행
- 최대 element와 최대+1 boundary
- 반복 부하에서 allocations/frame과 RSS가 안정적

### MQTT-SEC-02 Improvements: Topic allocation in `publish()`

#### Option A: Allocation exception handling

```cpp
try {
    const std::string topicString(topic);
    // publish
} catch (const std::bad_alloc&) {
    return false;
}
```

- 장점: interface 변경 없음
- 단점: 매 publish topic 복사는 유지
- 적용 가능성: **Very High**
- 우선순위: **P0**

#### Option B: Owned topic reference

Sink가 이미 소유한 `std::string topic_`을 직접 전달한다.

```cpp
virtual bool publish(const std::string& topic,
                     std::string_view payload,
                     int qos,
                     bool retain = false) noexcept = 0;
```

- 장점: 매 frame topic allocation 제거
- 단점: interface 변경과 caller lifetime 계약 필요
- 적용 가능성: **High**
- 우선순위: **P1**

#### Option C: Registered Topic Handle

Transport 시작 시 topic을 한 번 등록하고 hot path에서는 작은 ID만 전달한다.

```cpp
TopicId id = transport.registerTopic("veda/ch/2/topview");
transport.publish(id, payload, qos);
```

- 장점: 검증, 소유권 및 allocation을 초기화 시점에 한 번만 처리
- 단점: registry와 invalid/stale handle 관리 필요
- 적용 가능성: **Medium**
- 우선순위: topic 종류와 publish 빈도가 증가할 때 P2

#### Memory pool applicability

topic은 몇 개의 고정 문자열이므로 memory pool이나 arena를 적용할 실익이 작다. 생성 시
소유한 string reference 또는 Topic Handle이 더 단순하고 빠르다.

#### Recommended combination

Option A로 process termination을 먼저 막고 Option B로 반복 allocation을 제거한다.

검증 기준:

- disconnected 상태에서도 oversized topic이 terminate를 유발하지 않음
- 정상 cached topic 발행에서 application allocation 0
- publish와 stop 경합에서 topic reference와 client lifetime 유지

### MQTT-SEC-03 Improvements: Topic and QoS validation

#### Option A: Validate every transport entry

```cpp
if (topic.empty() || topic.size() > 65535 || qos < 0 || qos > 2)
    return false;
```

embedded NUL, publish wildcard `+/#` 및 UTF-8 정책도 검사한다.

- 장점: 모든 caller에 동일한 fail-closed 정책
- 단점: 매 publish 반복 검사
- 적용 가능성: **Very High**
- 우선순위: **P0/P1**

#### Option B: Validate immutable topic once

Topic Handle이나 validated topic object를 생성할 때만 검사하고 hot path에서는 handle만
사용한다.

- 장점: 반복 분기와 문자열 scan 제거
- 단점: 동적 topic을 허용하는 API에는 별도 경로 필요
- 적용 가능성: **High**, 현재 production topic은 고정
- 우선순위: **P1**

#### Option C: Strong QoS type

```cpp
enum class MqttQos : std::uint8_t {
    AtMostOnce,
    AtLeastOnce,
    ExactlyOnce
};
```

- 장점: invalid integer QoS를 compile time에 차단
- 단점: Contract 상수와 interface 변경 필요
- 적용 가능성: **High**
- 우선순위: P1/P2

#### Recommended combination

고정 topic은 생성 시 검증해 handle로 저장하고 QoS는 enum으로 제한한다. public entry
point에는 방어적 runtime validation도 유지한다.

검증 기준:

- empty, embedded NUL, wildcard, oversized 및 invalid UTF-8 topic
- QoS `-1`, `0`, `2`, `3`
- invalid input이 mosquitto 호출 전에 거부됨

### MQTT-SEC-04 Improvements: Queue memory exhaustion

#### Option A: Element and payload hard limits

```cpp
if (in.objects.size() > kMaxTopViewObjects)
    return false;
```

serialized payload에도 별도 최대 byte를 둔다.

- 장점: 큰 복사와 직렬화 전에 거부
- 단점: 실제 queue 총 memory를 직접 관리하지는 못함
- 적용 가능성: **Very High**
- 우선순위: **P0**

#### Option B: Queue byte accounting

```text
queuedBytes + frameOwnedBytes > maxQueuedBytes
-> oldest frame 제거 또는 새 frame drop
```

- 장점: frame 수가 작아도 개별 frame이 큰 경우 방어
- 단점: push, pop, drop-oldest, shutdown 모든 경로에서 counter 일관성 필요
- 적용 가능성: **High**
- 우선순위: **P1**

#### Option C: Fixed Frame Pool and ring queue

Sink별로 slot을 격리하고 queue에는 generation을 포함한 slot index를 저장한다.

- 장점: 생성 시 memory upper bound 확정, frame당 allocation 제거, pool 고갈을 drop으로 처리
- 단점: stale handle, double release 및 shutdown slot 회수 검증 필요
- 적용 가능성: **High**
- 우선순위: **P1**, 권장 구조

#### Option D: Shared synchronized pool

TopView와 Blur가 하나의 synchronized pool을 공유하는 방식은 권장하지 않는다.

- allocator lock 경합
- 한 Sink의 burst가 다른 Sink memory budget 침범
- 장애 격리 저하

pool이 필요하면 Sink별로 둔다.

#### Option E: Arena

arena는 pool slot 내부 구현으로 사용할 때 적합하다. 하나의 global arena는 frame lifetime이
서로 겹쳐 일괄 reset할 수 없으므로 부적합하다.

#### Recommended combination

1. element와 serialized byte 상한
2. Sink별 fixed Frame Pool
3. 고정 ready/free ring
4. pool 외 libmosquitto queue까지 포함한 byte/RSS metric

검증 기준:

- queue full 상태에서 최악 크기 frame 반복 시 RSS upper bound 유지
- drop-oldest에서 slot이 정확히 한 번 반환됨
- pool exhaustion 후 정상 처리로 회복
- ASan/TSAN에서 UAF, double free 및 race 없음

### MQTT-SEC-05 Improvements: Lifecycle misuse

#### Option A: Atomic state machine

```cpp
enum class State {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped
};
std::atomic<State> state_;
```

CAS로 유효한 상태 전이만 허용한다.

- 장점: 동시/반복 start와 stop 정책 명확화
- 단점: 초기화 실패 후 retry와 restart 정책 정의 필요
- 적용 가능성: **High**
- 우선순위: **P1**

#### Option B: `std::once_flag`

- 장점: 구현 단순
- 단점: 초기화 실패 후 재시도와 stop 후 restart를 표현하지 못함
- 적용 가능성: **Low to Medium**
- 우선순위: process lifetime에 정확히 한 번만 시작할 때만 사용

#### Option C: Restrict ownership to `AppContext`

start/stop 호출 권한을 lifecycle owner에만 두고 외부 API surface를 줄인다.

- 장점: 오용 가능성 감소
- 단점: runtime 방어를 대체하지 못함
- 적용 가능성: **Medium**
- 우선순위: Option A의 보조 통제

#### Recommended combination

Atomic state machine을 적용하고 repeated start는 idempotent success 또는 명시적 false 중
하나로 contract를 확정한다. Sink와 Transport가 같은 lifecycle 규칙을 사용해야 한다.

검증 기준:

- 두 thread의 동시 start
- start 실패 중 stop
- retry thread 실행 중 stop
- stop 두 번
- stopped instance 재시작 정책

### MQTT-SEC-06 Improvements: Broker authorization

#### Option A: Per-channel broker ACL

```text
compute-channel-2:
  allow publish veda/ch/2/topview
  allow publish veda/ch/2/blur
  allow publish veda/ch/2/alive
  deny  publish veda/ch/+/#
```

- 장점: application 검증 우회 시에도 broker가 cross-channel publish 차단
- 단점: channel별 identity와 배포 자동화 필요
- 적용 가능성: broker ACL 지원 시 **High**
- 우선순위: **P0 운영 보안**

#### Option B: mTLS client identity

- 장점: 강한 client identity와 ACL 연계
- 단점: certificate 발급, 보호, rotation 및 폐기 체계 필요
- 적용 가능성: **Medium to High**
- 우선순위: production P1

#### Option C: Per-service username/password

- 장점: 일반적인 broker에서 쉽게 적용
- 단점: secret file 보호와 rotation 필요
- 적용 가능성: **High**
- 우선순위: mTLS가 어려운 환경의 최소 기준

#### Option D: Network segmentation

Compute/broker VLAN과 firewall allowlist를 적용한다.

- 장점: credential 탈취와 network 공격 범위 축소
- 단점: application-level ACL을 대체하지 못함
- 적용 가능성: 환경 의존
- 우선순위: defense in depth

#### Recommended combination

TLS server 검증에 channel별 unique identity와 broker ACL을 결합한다. 가능하면 mTLS를
사용하고 network segmentation을 보조 방어로 둔다.

검증 기준:

- channel 2 identity의 channel 1 publish 거부
- wildcard publish/subscribe 거부
- anonymous connection 거부
- expired/revoked certificate 거부
- credential rotation 중 availability 유지

## 5. OOM Audit

### Allocation inventory

| Path | Allocation | Bound | Failure behavior |
|---|---|---|---|
| Transport construction | config/client/topic strings | config-dependent | constructor throws |
| Listener registration | vector/function storage | number of Sinks | exception propagates |
| Sink start | thread state | fixed thread count | exception propagates |
| TopView prepare | object vector | input count | terminate due to `noexcept` |
| Blur prepare | blur vector | input count | terminate due to `noexcept` |
| Queue insertion | deque/frame storage | frame count only | caught by `send()` |
| Serialization | payload string | encoded size | caught by `publishFrame()` |
| Topic conversion | `std::string(topic)` | no explicit bound | terminate due to `noexcept` |
| Mosquitto publish | library message copy | library/broker policy | error code or library allocation behavior |
| Logging | temporary strings | event content | may terminate in `noexcept` paths |

### Worst-case memory model

각 Sink의 대략적 upper bound를 계산할 때 단순 queue length만 사용하면 안 된다.

```text
Sink memory
~= maxQueueFrames * maxFrameOwnedBytes
 + stagingFrameBytes
 + workerFrameBytes
 + serializationCapacity
 + deque overhead
 + libmosquitto outgoing queue
```

두 Sink와 TLS/OpenSSL buffer, RTSP frame 및 pipeline vector가 동시에 존재한다. 운영 memory
budget은 이 전체를 기준으로 정한다.

### OOM verification procedure

1. object/blur count를 0, 정상 최대, 최대+1로 생성한다.
2. broker 연결을 막아 queue를 full로 유지한다.
3. queue size별 peak RSS를 측정한다.
4. process memory limit 또는 failure-injection allocator로 allocation 실패를 유도한다.
5. 기대 결과는 process 종료가 아니라 frame drop과 counter 증가다.
6. ASan은 OOM policy가 다를 수 있으므로 production allocator 시험을 별도로 수행한다.

### Required fixes

- `prepare noexcept` 문제를 P0로 수정
- element/serialized byte limit을 P1로 추가
- queue byte budget과 libmosquitto outgoing limit을 P1/P2로 조정
- RSS high-water 및 allocation failure metric 추가

## 6. Stack Overflow Audit

### Static structure

- 검토 대상 코드에 recursion 없음
- 가변 길이 stack array 없음
- input count에 비례하는 stack allocation 없음
- frame element는 `std::vector` heap에 저장
- worker/retry loop는 반복문이며 call depth가 input에 따라 증가하지 않음

### Measured stack usage

GCC 14의 `-fstack-usage`와 ASan/UBSan instrumented build에서 주요 결과:

| Function | Reported static stack |
|---|---:|
| `MqttTransport::removeConnectionListener` | 2064 B |
| `MqttTransport::initializeClient` | 1728 B |
| `MqttTransport::onConnect` | 1120 B |
| `MqttTransport::retryLoop` | 944 B |
| `MqttFrameSink::publishFrame` | 944 B |
| `MqttFrameSink::workerLoop` | 720-736 B |
| `MqttTransport::publish` | 512 B |
| `MqttTopViewSink::prepare` | 656 B |
| `MqttBlurSink::prepare` | 336 B |

Instrumentation과 compiler version에 따라 값은 달라진다. 최대 static frame은 약 2 KiB이며
input에 따라 call depth가 증가하지 않으므로 현재 범위에서 stack overflow 위험은 낮다.

### Stack policy

- thread stack을 과도하게 축소하지 않는다.
- callback에서 큰 local array를 추가하지 않는다.
- future parser/serializer에 recursion을 도입하지 않는다.
- release build에서도 `-fstack-usage` threshold를 CI에서 확인한다.
- 권장 warning threshold는 대상 환경 측정 후 정하되 우선 8 KiB/function부터 시작한다.

## 7. Performance Audit

### CPU complexity

| Operation | Complexity |
|---|---|
| Frame validation | O(elements) |
| Blur filtering | O(elements) |
| Serialization | O(payload bytes) |
| Queue push/pop | Amortized O(1) |
| Listener notification | O(listener count) |

### Positive findings

- connection/TLS session 공유
- bounded frame queue와 drop-oldest
- payload buffer capacity 재사용
- exact channel 검사를 object loop 전에 수행
- topic allocation을 client lock 밖으로 이동
- status read는 atomic
- debug message formatting을 log level 뒤로 지연

### Optimization candidates

1. `publish()`의 topic 복사 제거
   - Sink topic은 이미 `std::string`으로 lifetime이 안정적이다.
   - libmosquitto 호출 시 null-terminated pointer를 안전하게 전달하는 internal overload를
     고려한다.
   - API lifetime 계약이 복잡해지므로 benchmark로 이득을 확인한 후 적용한다.
2. staging buffer recycling
   - 현재 vector는 queue로 move되어 매 frame allocation될 수 있다.
   - buffer pool은 allocation을 줄이지만 lock과 lifetime 복잡도를 높인다.
   - 5 fps x 2 Sink 수준에서는 우선순위가 낮다.
3. reserve policy
   - 공격자가 만든 순간적 대형 frame 때문에 capacity가 장기간 유지되는 경우 shrink 정책이
     필요할 수 있다.
   - 매 frame `shrink_to_fit()`은 금지한다.
4. PGO
   - branch hint보다 실제 Raspberry Pi workload 기반 PGO를 우선한다.

### Measurement plan

- `perf stat`: cycles, instructions, branches, branch-misses, cache-misses
- `perf record`: serialization, allocation, mutex contention hot spot
- allocation profiler: allocations/frame, bytes/frame
- benchmark matrix: 0/16/64/256 elements, connected/disconnected, QoS별
- latency percentiles: send-to-publish p50/p95/p99

실제 broker와 production-like payload가 없는 상태에서는 최적화 효과를 확정하지 않는다.

## 8. I/O Bottleneck Audit

### Boundaries

`send()`는 socket I/O를 하지 않는다. worker가 libmosquitto queue에 publish하고 별도 network
thread가 TLS socket I/O를 수행한다.

### Bottleneck candidates

- 두 Sink의 `clientMutex_` 직렬화
- broker/network backpressure
- libmosquitto outgoing message queue
- QoS inflight 제한과 acknowledgment 지연
- reconnect 후 queued frame burst
- TLS record encryption
- large payload copy
- synchronous logging sink

### Detection

- publish call duration이 늘지만 network throughput이 낮음: libmosquitto/lock 병목
- queue high-water와 drop이 증가: downstream I/O 병목
- CPU와 branch miss가 낮지만 wall time이 증가: blocking/lock/network 병목
- reconnect 직후 burst와 RSS 증가: outgoing queue 적체
- log volume과 disk wait 동반 증가: logging I/O 병목

### Controls

- broker/client별 message and inflight limit
- queue frame 및 byte budget
- drop-oldest 유지
- oversized payload drop
- reconnect jitter/backoff
- rate-limited logging
- transport return code metric

## 9. Branch Prediction Audit

현재 branch 순서는 정상 workload에 적합하다.

1. frame-level schema/timestamp/channel 검사
2. element loop
3. rare invalid element branch
4. queue full/stopping branch
5. connected 상태에서 publish

권장:

- 검증 비용이 작은 조건을 먼저 유지한다.
- invalid input을 조기에 반환한다.
- `[[likely]]`/`[[unlikely]]`는 profile 없이 추가하지 않는다.
- error logging을 hot loop에서 rate limit한다.
- class별 `switch` 또는 helper가 compiler에 inlining되는지 release profile로 확인한다.
- PGO로 실제 branch frequency를 반영한다.

## 10. Dynamic Allocation Audit

### Per-frame allocations to watch

- TopView `out.objects.assign`
- Blur `out.blurs.reserve/push_back`
- deque block growth
- payload buffer growth
- topic copy
- log formatting
- libmosquitto message copy

### Allocation reduction priority

| Candidate | Benefit | Complexity | Priority |
|---|---|---|---|
| Element count limit | OOM prevention | Low | P0/P1 |
| Remove `noexcept` allocation termination | Availability | Low | P0 |
| Topic validation and safe copy | Availability | Low | P1 |
| Payload capacity reuse | Already applied | Low | Keep |
| Topic copy elimination | Small recurring saving | Medium | P2 |
| Frame buffer pool | Allocation reduction | High | P3 after benchmark |
| Custom allocator/PMR | Potential control | High | P3 |

## 11. Build and Tool Results

Modified files were overlaid only in `/tmp/Main_Project_patch_review`; source artifacts were not changed.

Commands:

```bash
cmake -S /tmp/Main_Project_patch_review/compute-server \
  -B /tmp/Main_Project_patch_review/compute-server/build-audit \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -fstack-usage -fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

cmake --build /tmp/Main_Project_patch_review/compute-server/build-audit \
  --target compute-server -j2
```

Results:

- Configure: PASS
- Compile: PASS
- Link: PASS
- ASan/UBSan instrumented binary generation: PASS
- Target-file compiler warnings: none observed
- Runtime sanitizer workload: not executed because it requires deployment configuration and MQTT/RTSP
  endpoints
- Existing `sink-lifecycle`: could not compile because its test fixture still references removed
  `AppConfig::channelCount`
- Out-of-scope warnings:
  - `RtspClientV2`: `ssize_t` to `int` conversion
  - `RtspClientV2`: deprecated OpenSSL `MD5()`

## 12. Audit Test Matrix

| ID | Test | Expected |
|---|---|---|
| SEC-CH-01 | configured channel frame | publish |
| SEC-CH-02 | different but otherwise valid channel | drop |
| SEC-CH-03 | negative/max channel | drop |
| OOM-01 | max elements | bounded success |
| OOM-02 | max+1 elements | drop before allocation |
| OOM-03 | allocator failure in prepare | no process termination |
| OOM-04 | disconnected full queues | RSS within byte budget |
| STACK-01 | maximum element count | stack usage remains constant |
| PERF-01 | 0/16/64/256 element benchmark | linear CPU, bounded allocation |
| IO-01 | broker throttling | queue bound and drop-oldest maintained |
| IO-02 | reconnect burst | no unbounded outgoing queue |
| RACE-01 | publish vs stop loop | no UAF/deadlock |
| RACE-02 | listener remove vs callback | callback absent after remove returns |
| LIFE-01 | repeated start | safely rejected/idempotent |
| INPUT-01 | invalid topic/QoS | rejected before mosquitto |
| TLS-01 | wrong CA/hostname | connection rejected |
| ACL-01 | publish to other channel | broker denies |

## 13. Remediation Order

### P0

1. Remove or safely handle allocation in `noexcept prepare()`.
2. Repair `sink_lifecycle` fixture and add cross-channel tests.
3. Define object/blur element limits.

### P1

1. Add topic/QoS validation and allocation handling.
2. Add queue byte budget and payload size limit.
3. Make `start()` lifecycle explicitly idempotent or reject repeats.
4. Add publish/drop/reconnect/RSS metrics.

### P2

1. Stress test publish/disconnect/shutdown with TSAN.
2. Benchmark topic copy and frame buffer allocation.
3. Verify broker authentication and per-channel ACL evidence.
4. Add CI stack-usage threshold and sanitizer jobs.

## 14. Audit Checklist

At every security review:

- [ ] Exact channel equality is enforced.
- [ ] Frame element count and serialized bytes are bounded.
- [ ] Queue count and queue bytes are bounded.
- [ ] No allocation-capable operation can escape a `noexcept` boundary.
- [ ] Topic and QoS are validated.
- [ ] TLS peer verification is enabled.
- [ ] Broker anonymous access is disabled.
- [ ] Per-channel ACL is tested.
- [ ] `isConnected()` and `isReady()` remain lock-free.
- [ ] Listener removal still guarantees no in-flight callback.
- [ ] Publish and destroy remain mutually exclusive.
- [ ] Repeated lifecycle calls are safe.
- [ ] No recursion or input-sized stack allocation was introduced.
- [ ] Stack usage is below the project threshold.
- [ ] Allocation/frame and RSS high-water are measured.
- [ ] Queue high-water, drop reason and publish latency are observable.
- [ ] ASan/UBSan and TSAN regression tests pass.
