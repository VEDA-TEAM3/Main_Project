# Sink 레퍼런스

> **대상 파일**
> - 출력 포트: `include/interfaces/ISink.h`
> - 파이프라인 주입 지점: `include/core/Pipeline.h`, `src/core/Pipeline.cpp`
> - 콘솔 어댑터: `src/sink/ConsoleSink.h`
> - 현재 네트워크 어댑터: `src/sink/MqttTopViewSink.h`, `MqttBlurSink.h`

Sink는 Compute Server 파이프라인의 **출력 포트(output port)** 다. 파이프라인은 결과 프레임을
`ISink<T>`에 넘길 뿐, MQTT·HTTP·Kafka·IPC·파일 같은 전달 프로토콜을 알지 못한다.

현재 운영 구현이 MQTT인 것은 배포 선택이며 Sink 인터페이스의 전제 조건이 아니다. MQTT 연결,
TLS, topic, QoS, 재접속 및 Mosquitto 수명은 Sink 공통 계약이 아니라 MQTT 어댑터의 구현
세부사항이다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-29 | 2.0.0 | DevSunbi | Sink를 프로토콜 독립 출력 포트로 재정의하고 어댑터 및 테스트 하네스 계약 분리 |

---

## 1. 출력 포트 계약

```cpp
template <typename T>
class ISink {
public:
    virtual ~ISink() = default;
    virtual void send(const T& frame) = 0;
};
```

`ISink<T>`가 정의하는 것은 프레임 타입과 `send()` 호출뿐이다.

| 계약 | 내용 |
|------|------|
| 입력 타입 | `T`로 컴파일 시점에 고정 |
| 호출 주체 | Pipeline을 실행하는 스레드 |
| 입력 수명 | `send()` 호출이 끝날 때까지만 유효한 borrowed reference |
| 소유권 | 비동기 보관이 필요하면 구현체가 복사하거나 자체 표현으로 변환 |
| 지연 | Pipeline 처리율을 막지 않도록 빠르게 반환 |
| 실패 | 개별 출력 실패가 Pipeline 전체를 종료시키지 않도록 구현체 내부에서 격리 |
| 순서 | Pipeline은 생성 순서대로 호출하지만, 최종 전달 순서는 어댑터 정책에 따름 |

현재 `send()` 선언에는 `noexcept`가 없으므로 “예외를 외부로 내보내지 않는다”는 규칙은 타입
시스템이 아니라 구현 계약이다. 새 구현체는 외부 라이브러리, 직렬화 및 메모리 할당에서 발생한
예외를 어댑터 경계에서 처리해야 한다.

## 2. Pipeline과 Sink의 결합

Pipeline 생성자는 구체 클래스가 아니라 두 개의 출력 포트를 주입받는다.

```cpp
Pipeline(
    // 앞 단계 생략
    std::shared_ptr<ISink<veda::TopViewFrame>> riskSink,
    std::shared_ptr<ISink<veda::BlurFrame>> blurSink,
    const PipelineOptions& options);
```

```text
Pipeline
 ├─ ISink<TopViewFrame> ──> 선택된 TopView 출력 어댑터
 └─ ISink<BlurFrame>    ──> 선택된 Blur 출력 어댑터
```

Pipeline은 다음 항목을 알지 못한다.

- frame이 직렬화되는 형식
- network 또는 local I/O 사용 여부
- endpoint, topic, partition, file path
- 연결·재시도·인증 정책
- 내부 queue와 worker 유무
- 전달 보장 수준과 backpressure 정책

따라서 다른 프로토콜로 변경해도 Pipeline의 처리 순서와 프레임 생성 코드는 바뀌지 않아야
한다. 조립 단계에서 주입하는 Sink 구현만 교체한다.

## 3. 프레임별 의미 계약

프로토콜과 무관하게 Pipeline은 각 호출에서 해당 시점의 전체 상태 스냅샷을 전달한다.

### 3.1 TopViewFrame

- `ts`: 원본 프레임 시각
- `ch`: 입력 채널
- `objects`: 검증·분류·좌표 변환을 통과한 위험 객체
- 빈 `objects`도 “현재 위험 객체 없음”을 뜻하는 유효한 프레임

### 3.2 BlurFrame

- `ts`: 원본 프레임 시각
- `ch`: 입력 채널
- `blurs`: 화면 좌표계로 변환된 블러 대상
- 빈 `blurs`도 “기존 블러 영역 제거”에 필요한 유효한 프레임

프레임이 스냅샷이라는 성질은 실시간 어댑터가 오래된 백로그 대신 최신 프레임을 선택할 수 있는
근거다. 반면 감사 로그나 영속 저장처럼 모든 이벤트 보존이 필요한 어댑터는 별도의 queue 및
backpressure 정책을 선택할 수 있다.

## 4. 구현 전략

### 4.1 동기 어댑터

`ConsoleTopViewSink`와 `ConsoleBlurSink`는 가장 단순한 구현 예다.

```text
Pipeline thread -> send() -> 출력 -> 반환
```

개발·진단 용도로는 충분하지만 느린 stdout, 파일 또는 network I/O를 동기 실행하면 Pipeline
전체를 지연시킬 수 있다. 운영 어댑터는 호출 비용을 측정해 비동기 경계가 필요한지 결정한다.

### 4.2 비동기 어댑터

네트워크나 느린 저장장치를 사용하는 일반적인 구조는 다음과 같다.

```text
Pipeline thread
  -> 입력 검증/복사
  -> bounded queue
  -> 즉시 반환

Adapter worker
  -> 인코딩
  -> 전송 또는 저장
```

비동기 구현은 다음 정책을 명시해야 한다.

- queue의 프레임 수 또는 byte 상한
- queue full 시 drop, overwrite 또는 producer 제한 방식
- 최신 상태 우선인지 전체 이벤트 보존인지
- 연결 불가 상태에서 보관할 범위
- 종료 시 drain할지 폐기할지
- 실패 횟수와 지연을 관측하는 방법

이 정책은 `ISink<T>`가 일괄 강제하지 않는다. 출력 목적과 프로토콜 특성에 따라 어댑터가
선택한다.

### 4.3 수명주기

`ISink<T>`에는 `start()`나 `stop()`이 없다. 수명주기가 필요 없는 Sink와 필요한 Sink를 모두
수용하기 위한 최소 인터페이스다.

- 단순 Sink는 생성 직후 사용 가능하다.
- worker나 connection을 소유한 Sink는 애플리케이션 조립 계층에서 시작한다.
- 종료는 Pipeline 호출을 먼저 중단한 뒤 Sink의 worker와 transport를 정리한다.
- 비동기 callback이 Sink를 참조한다면 callback 해제와 worker join을 소멸 전에 완료한다.

특정 구현의 `start()`, 연결 listener 및 transport shutdown 순서를 `ISink<T>`의 공통
수명주기로 간주하면 안 된다.

## 5. 현재 제공되는 어댑터

| 구현 | 출력 대상 | 실행 방식 | 프로토콜 의존성 |
|------|-----------|-----------|-----------------|
| `ConsoleTopViewSink` | stdout | 동기 | 없음 |
| `ConsoleBlurSink` | stdout | 동기 | 없음 |
| `MqttTopViewSink` | MQTT broker | bounded queue + worker | MQTT |
| `MqttBlurSink` | MQTT broker | bounded queue + worker | MQTT |

MQTT 어댑터의 topic, QoS, TLS, 연결 상태 및 재접속 정책은
[MQTT Transport 레퍼런스](./mqtt_transport_reference.md)와 해당 어댑터 소스에서 다룬다.

향후 가능한 구현 예:

- `HttpTopViewSink`, `HttpBlurSink`
- `KafkaTopViewSink`, `KafkaBlurSink`
- Unix domain socket 또는 shared-memory Sink
- 파일 녹화·재생용 Sink
- 여러 출력으로 복제하는 `CompositeSink<T>`

새 구현은 `ISink<T>`를 구현하고 조립 단계의 주입 대상만 교체한다. Pipeline에 프로토콜 분기나
구체 Sink 타입 검사를 추가하지 않는다.

## 6. 검증 책임

검증은 두 층으로 구분한다.

| 계층 | 책임 |
|------|------|
| Pipeline/domain | 프레임의 의미를 만들고 잘못된 좌표 변환 결과를 제외 |
| Sink adapter | 외부 경계로 내보내기 전 schema, 크기, 인코딩 및 전송 제약 방어 |

여러 프로토콜이 같은 검증 규칙을 필요로 한다면 MQTT 기반 클래스에서 복사하지 말고
프로토콜 독립 validator 또는 frame preparation 구성요소로 추출한다.

다음 항목은 프로토콜 독립 후보이다.

- schema version
- timestamp와 channel 일치
- 객체 class와 finite 좌표
- normalized Blur rectangle
- frame element 수와 메모리 안전 상한

다음 항목은 어댑터 전용이다.

- MQTT topic/QoS/retain
- HTTP status와 timeout
- Kafka partition 및 acknowledgement
- 파일 rotation과 flush

## 7. 테스트 하네스

Pipeline 단위 테스트는 실제 broker나 network를 띄우지 않고 기록용 Sink를 주입해야 한다.

```cpp
template <typename T>
class RecordingSink final : public ISink<T> {
public:
    void send(const T& frame) override {
        frames.push_back(frame);
    }

    std::vector<T> frames;
};
```

```cpp
auto topViewHarness = std::make_shared<RecordingSink<veda::TopViewFrame>>();
auto blurHarness = std::make_shared<RecordingSink<veda::BlurFrame>>();

Pipeline pipeline(
    parser, imageMapper, sanitizer, router, ground, transform,
    topViewHarness, blurHarness, options);
```

하네스가 검증할 항목:

- 입력 packet 하나당 두 출력 포트가 호출되는지
- TopView/Blur 객체가 올바른 포트로 분리되는지
- timestamp와 channel이 보존되는지
- 빈 프레임도 호출로 전달되는지
- 한 Sink 구현을 교체해도 다른 출력과 Pipeline 결과가 바뀌지 않는지
- Sink가 받은 프레임을 수정해 Pipeline 내부 상태에 영향을 주지 않는지

프로토콜 어댑터 테스트는 별도로 구성한다.

- 공통 contract test: 빠른 반환, 예외 격리, 소유권 준수
- queue adapter test: overflow와 종료 정책
- MQTT adapter test: topic/QoS/reconnect
- 다른 프로토콜 adapter test: 해당 프로토콜의 전달 및 오류 정책

## 8. 확장 체크리스트

새 Sink를 추가할 때 확인한다.

- [ ] `ISink<FrameType>`만 구현하고 Pipeline을 수정하지 않았는가
- [ ] `send()`에서 장시간 I/O를 실행하지 않는가
- [ ] 비동기 사용 시 입력 frame을 호출 이후에도 안전하게 소유하는가
- [ ] queue와 메모리 사용량에 상한이 있는가
- [ ] overload와 연결 실패 정책이 문서화됐는가
- [ ] 외부 라이브러리 예외가 Pipeline으로 전파되지 않는가
- [ ] 종료 중 callback, worker 및 frame 소유권이 안전한가
- [ ] 프로토콜별 설정이 공통 Sink 인터페이스로 새지 않는가
- [ ] RecordingSink 기반 Pipeline 테스트와 어댑터별 테스트가 분리됐는가

## 9. 현재 구조의 개선 과제

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| 예외 계약 | 주석으로만 “던지지 않음” 명시 | `noexcept` 적용 가능성과 실패 보고 방식 검토 |
| 공통 검증 | MQTT 파생 Sink에 포함 | 프로토콜 독립 validator/preparer 추출 검토 |
| 수명주기 | 구체 구현과 조립 코드에 의존 | 필요 시 별도 lifecycle interface 도입 |
| 관측성 | 구현별 counter/log | 공통 Sink metric 모델 검토 |
| Contract test | 구현별 테스트 중심 | 모든 Sink 구현에 재사용할 하네스 구축 |

인터페이스는 작게 유지한다. 새 프로토콜이 필요하다는 이유만으로 endpoint, 연결 상태,
재시도 또는 직렬화 메서드를 `ISink<T>`에 추가하지 않는다. 그런 기능은 어댑터 또는 별도
transport interface의 책임이다.
