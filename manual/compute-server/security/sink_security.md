# Sink 계층 보안·성능 권고 (Security & Performance Advisory)

> **대상 모듈**
> - 출력 포트: `include/interfaces/ISink.h`
> - 파이프라인 주입 지점: `include/core/Pipeline.h`, `src/core/Pipeline.cpp`
> - 동기 구현: `src/sink/ConsoleSink.h`
> - 비동기 구현: `src/sink/MqttFrameSink.h`
> - 프레임 구현: `src/sink/MqttTopViewSink.*`, `MqttBlurSink.*`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-29 | 1.0.0 | DevSunbi | Sink 출력 경계의 가용성·개인정보·메모리·동시성 취약점 감사 및 하네스 검증 기준 |

---

## 1. 위협 모델 (Threat Model)

Sink는 Compute Server가 만든 `TopViewFrame`과 `BlurFrame`을 프로세스 밖으로 내보내는 마지막
신뢰 경계다. 현재 운영 어댑터는 MQTT지만, 보안 계약은 MQTT에 한정되지 않는다. HTTP, Kafka,
IPC, 파일 등 다른 구현도 동일한 `ISink<T>` 경계를 통과한다.

- **신뢰할 수 없는 입력**: RTSP/ONVIF metadata에서 파생된 객체 수, class, 좌표, timestamp,
  channel과 비정상적으로 큰 frame
- **신뢰할 수 없는 외부 상태**: 느린 소비자, 연결 단절, backpressure, callback 순서 및
  외부 라이브러리 오류
- **보호 자산**: Pipeline 가용성, 채널별 데이터 무결성, 얼굴·번호판 블러 정확성, 제한된
  RAM/CPU/thread 및 출력 대상의 접근 통제
- **설계 목표**: 출력 장애가 Pipeline 중단으로 확대되지 않고, frame이 잘못된 채널이나
  시점으로 전달되지 않으며, 개인정보 보호용 Blur 데이터가 조용히 손실되지 않아야 한다.

### 실패 방향

| 경로 | Fail-open 위험 | Fail-closed 위험 |
|------|----------------|------------------|
| TopView | 잘못된 객체가 위험 판단에 사용됨 | 정상 위험 객체가 누락됨 |
| Blur | 얼굴·번호판이 노출됨 | 정상 영역까지 과도하게 가려짐 |
| 공통 | 잘못된 channel로 정보 유출 | 출력 중단으로 관측성 상실 |

Blur 경로는 가용성보다 **개인정보 보호 실패**가 더 위험하다. 잘못된 target을 제거하고 빈
frame을 정상 발행하는 정책은 화면의 기존 블러를 지울 수 있으므로 별도 검토가 필요하다.

---

## 2. 인터페이스 계약과 구현 강제력

### 2.1 ⚠️ SINK-SEC-01 — 비차단·무예외 계약이 타입으로 강제되지 않음

`ISink<T>` 주석은 `send()`가 논블로킹이고 예외를 던지지 않아야 한다고 규정하지만 선언은
다음과 같다.

```cpp
virtual void send(const T& frame) = 0;
```

`noexcept`나 반환 상태가 없어 새 어댑터가 다음 문제를 만들어도 컴파일러가 차단하지 못한다.

- 동기 network/file I/O로 Pipeline 정지
- 직렬화·할당 예외가 Pipeline까지 전파
- 실패를 반환하거나 공통 metric으로 보고할 방법 부재

**현재 방어**: `MqttFrameSink::send()`는 `noexcept`이며 전체 enqueue 경로를 `try/catch`로
감싼다.

**잔여 위험**: 다른 프로토콜 구현은 동일 방어를 누락할 수 있다.

**권고**:

1. 모든 구현에 재사용하는 Sink contract harness를 둔다.
2. `send()`의 `noexcept` 적용 여부와 실패 관측 인터페이스를 별도 설계한다.
3. 외부 라이브러리 예외는 구체 어댑터 경계에서 종료한다.

### 2.2 ⚠️ SINK-SEC-02 — ConsoleSink의 동기 I/O와 정보 노출

`ConsoleTopViewSink`와 `ConsoleBlurSink`는 Pipeline thread에서 객체별 `std::cout`을 실행한다.

- 느린 stdout 또는 막힌 pipe가 Pipeline 전체를 정지시킬 수 있다.
- Blur object ID, class 및 좌표가 로그 수집기로 전달돼 개인정보 관련 메타데이터가 남는다.
- `std::ios`에 exception mask가 설정된 환경에서는 출력 실패가 예외로 전파될 수 있다.

**판정**: 개발용 구현으로는 허용 가능하지만 운영 사용은 부적합하다.

**권고**: production 조립에서 ConsoleSink를 금지하고, 필요한 경우 좌표·ID를 제거한
rate-limited 진단 Sink를 별도로 둔다.

---

## 3. 개인정보 및 데이터 무결성

### 3.1 ✅ TopViewFrame 전체 거부 정책

`MqttTopViewSink`는 다음 조건 중 하나라도 실패하면 frame 전체를 drop한다.

- schema version 불일치
- `timestamp <= 0`
- payload channel과 process channel 불일치
- object 수 256개 초과
- risk class가 아닌 객체
- NaN/Inf 좌표

부분 손상 frame을 정상 위험 정보로 오인하지 않는 fail-closed 정책이다. 특히 exact channel
검사는 다른 채널 토픽으로 데이터가 새는 것을 차단한다.

### 3.2 ⚠️ SINK-SEC-03 — 잘못된 BlurTarget의 부분 제거는 개인정보 fail-open 가능

`MqttBlurSink::prepare()`는 잘못된 target만 제외하고 나머지 frame을 발행한다.

```cpp
if (isValidBlurTarget(blur)) {
    out.blurs.push_back(blur);
} else {
    recordDrop("invalid blur target skipped");
}
```

정상 target까지 함께 버리지 않는 장점이 있지만, 제외된 target이 실제 얼굴이나 번호판이었다면
그 대상은 가려지지 않는다. 모든 target이 제외되면 빈 BlurFrame이 발행되어 소비자가 이전
블러 영역까지 지울 수 있다.

| 항목 | 판정 |
|------|------|
| 메모리 안전 | 안전 |
| 데이터 형식 | 유효한 payload 유지 |
| 개인정보 보호 | ⚠️ fail-open 가능 |

**권고**:

- 부분 제거가 제품 정책인지 명시적으로 승인한다.
- invalid target 수와 frame 단위 privacy-drop metric을 분리한다.
- 모든 target이 invalid인 frame은 빈 정상 frame과 구분 가능한 상태 신호를 검토한다.
- 대체 프로토콜에서도 동일한 검증을 쓰도록 protocol-neutral Blur validator로 추출한다.

### 3.3 ⚠️ SINK-SEC-04 — 검증이 MQTT 구현에 결합됨

schema, channel, class, 좌표 검증이 `MqttTopViewSink`와 `MqttBlurSink` 안에 있다.
`ConsoleSink`나 향후 HTTP/Kafka Sink는 `ISink<T>`만 구현하면 이 검증을 우회한다.

**영향**: 프로토콜 교체 시 보안 방어가 함께 사라질 수 있다.

**권고**: frame validator/preparer를 프로토콜 독립 구성요소로 추출하고 모든 외부 출력
어댑터가 공통으로 사용하도록 한다.

---

## 4. OOM 및 자원 고갈 방어

### 4.1 ✅ Frame과 queue 상한

| 자원 | 현재 상한 | 방어 |
|------|-----------|------|
| TopView objects | frame당 256 | 초과 frame 전체 drop |
| Blur targets | frame당 256 | 초과 frame 전체 drop |
| serialized payload | 1 MiB | publish 전 drop |
| queue frame 수 | Sink별 config, 상한 clamp | full 시 drop-oldest |
| worker 수 | MQTT Sink당 1개 | 무제한 thread 생성 없음 |

queue가 frame 개수로 제한되고 각 frame의 element 수도 제한되므로 무한 증가는 없다. 다만
byte 단위 예산은 없어 타입 크기나 payload 형식이 커지면 메모리 상한 계산이 달라진다.

### 4.2 ⚠️ SINK-SEC-05 — Staging buffer 반복 할당

`staging_`은 queue로 move된 후 생산자에게 돌아오지 않으므로 `prepare()`가 frame마다 vector
할당을 수행할 수 있다. 이는 메모리 누수는 아니지만 메모리 압박 시 `std::bad_alloc` 가능성을
높인다.

**현재 방어**:

- `prepare()`는 `noexcept`가 아니다.
- allocation 예외는 `send()`에서 catch되어 frame drop으로 바뀐다.
- `recordDrop()`의 로그 문자열 할당도 별도 catch로 격리된다.

**판정**: process termination 방어는 적용됐으며 잔여 위험은 처리량 저하와 frame 손실이다.

### 4.3 ✅ 백로그 합류와 CPU 상한

worker는 queue에 여러 frame이 쌓이면 최신 한 장만 남기고 이전 frame을 제거한다. 실시간
스냅샷을 오래된 순서대로 발행하는 고정 지연과 불필요한 직렬화 CPU 사용을 줄인다.

drop 계상 loop는 최대 queue 크기로 유계이고 정상 경로에서는 실행되지 않는다.

---

## 5. 동시성 및 수명주기

### 5.1 ✅ 중복 시작 방어

`start()`는 atomic `started_` CAS로 listener와 worker의 중복 생성을 막는다. thread 생성
실패 시 listener를 제거하고 상태를 원복한 뒤 예외를 다시 던져 반쪽 초기화를 남기지 않는다.

### 5.2 ✅ Lost wakeup 방어

연결 listener는 `queueMutex_`를 한 번 획득했다 놓은 뒤 condition variable을 깨운다.
worker의 술어 평가와 wait 진입 사이에 알림이 유실되는 창을 닫는다.

### 5.3 ✅ UAF와 순수 가상 호출 방어

worker가 `describe()` 같은 파생 virtual method를 호출하므로 파생 소멸자가 먼저 `shutdown()`을
호출한다.

```text
listener 제거
-> stopping 설정
-> queue 폐기
-> worker wakeup
-> worker join
-> 파생 객체 파괴
```

listener를 queue lock보다 먼저 제거해 listener lock과 queue lock의 역순 교착도 방지한다.

### 5.4 ⚠️ SINK-SEC-06 — null dependency 방어 부재

`MqttFrameSink`는 주입된 `transport_`를 null 검사 없이 사용하고 Pipeline도 주입된 Sink를
즉시 역참조한다. 현재 `AppContext`가 정상 객체만 조립하지만 잘못된 테스트/조립 코드는
즉시 crash할 수 있다.

**권고**: 생성자에서 null dependency를 fail-fast 검증하거나 non-null ownership wrapper를
사용한다.

---

## 6. 스택·성능·I/O 감사

### 6.1 ✅ 스택 오버플로 — 구조적 면역

- Sink 경로에 재귀와 입력 크기 기반 stack allocation이 없다.
- frame의 가변 데이터는 `std::vector`, payload는 `std::string`, queue는 `std::deque`로
  heap에 저장된다.
- worker loop는 반복마다 고정 크기의 지역 변수만 사용한다.

### 6.2 ✅ MQTT I/O 격리

Pipeline thread의 `send()`는 socket I/O를 수행하지 않고 queue 삽입 후 반환한다. 인코딩과
publish는 Sink worker가 담당하고 실제 network I/O는 transport/network thread가 담당한다.

### 6.3 ⚠️ 관측성 공백

published/drop 총계는 있지만 다음 지표는 없다.

- queue depth와 high-water mark
- frame이 drop된 정확한 정책별 집계
- enqueue-to-publish latency
- 개인정보 target 검증 실패 frame 수
- Sink 구현별 `send()` 실행 시간

장애와 공격성 입력을 구분하려면 drop reason별 counter와 latency metric이 필요하다.

---

## 7. Sink Contract Harness

저장소에 모든 `ISink<T>` 구현에 공통 적용되는 추적 하네스는 현재 없다. Pipeline 기능
테스트에는 network 대신 기록용 Sink를 주입한다.

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

보안 하네스는 다음 항목을 구현체별로 반복 검증해야 한다.

| Test | Expected |
|------|----------|
| 잘못된 schema/timestamp/channel | 외부 출력되지 않음 |
| NaN/Inf TopView 좌표 | frame 전체 거부 |
| invalid BlurTarget 혼합 | 정책대로 격리되고 privacy-drop 집계 |
| 256/257 element 경계 | 256 허용, 257 거부 |
| queue overflow | 메모리 증가 없이 정책대로 drop |
| 느린/끊긴 transport | Pipeline `send()`가 제한 시간 안에 반환 |
| allocation failure injection | process 종료 없이 drop |
| 중복 `start()` | listener/worker 추가 생성 없음 |
| Sink 파괴와 callback 경합 | UAF/deadlock 없음 |
| null dependency | 조립 시점에 명확히 실패 |
| ConsoleSink production 조립 | 정책 검사에서 거부 |

권장 계측:

- ASan/UBSan: UAF, OOB, 수명주기
- TSAN: callback/queue/shutdown 경합
- allocator failure injection: `prepare`, queue push, encode, log
- timeout harness: `send()` 비차단 계약
- RSS/high-water 측정: 연결 단절과 최대 frame 지속 입력

---

## 8. 잔여 위험 및 권고 (Residual Risks)

| ID | 등급 | 상태 | 내용 | 우선 조치 |
|----|------|------|------|-----------|
| SINK-SEC-01 | Medium | Open | 비차단·무예외 계약이 인터페이스에서 강제되지 않음 | 공통 contract harness |
| SINK-SEC-02 | Medium | Open | ConsoleSink 동기 I/O 및 객체/좌표 노출 | production 사용 금지 |
| SINK-SEC-03 | High | Review required | invalid BlurTarget 부분 제거의 privacy fail-open | 제품 정책 확정 및 상태 신호 |
| SINK-SEC-04 | Medium | Open | frame 검증이 MQTT 어댑터에 결합 | 공통 validator 추출 |
| SINK-SEC-05 | Low | Accepted | staging 반복 할당 | 측정 후 pool/ring 검토 |
| SINK-SEC-06 | Low | Open | null Sink/Transport 조립 시 crash | 생성자 fail-fast |

우선순위는 `SINK-SEC-03`의 개인정보 실패 방향 확정, `SINK-SEC-04`의 검증 독립화,
`SINK-SEC-01`의 공통 하네스 구축 순이다.

---

## 9. 감사 요약

| 영역 | 판정 | 핵심 근거 |
|------|------|-----------|
| **개인정보 보호** | ⚠️ 검토 필요 | BlurTarget 부분 제거가 민감 대상 미블러로 이어질 수 있음 |
| **데이터 무결성** | ✅ MQTT 구현 양호 | schema/timestamp/exact channel/class/finite 검증 |
| **프로토콜 독립성** | ⚠️ 개선 필요 | 보안 검증이 MQTT 구현 내부에 위치 |
| **OOM 위험** | ✅ 유계 | element·payload·queue 상한과 allocation 예외 격리 |
| **스택 오버플로** | ✅ 구조적 면역 | 재귀/VLA 없음, 가변 데이터 heap 저장 |
| **동시성/수명주기** | ✅ 양호 | 중복 시작, lost wakeup, listener UAF 및 worker join 방어 |
| **I/O 병목** | ✅ MQTT / ⚠️ Console | MQTT 비동기 격리, Console은 Pipeline thread에서 동기 출력 |
| **관측성** | ⚠️ 부족 | reason별 metric, queue high-water, latency 없음 |

**총평**: MQTT Sink의 메모리 상한과 동시성 방어는 양호하지만, Sink 계층 전체의 보안 계약은
아직 구체 MQTT 구현에 의존한다. 가장 중요한 잔여 위험은 invalid BlurTarget을 부분 제거하는
정책의 개인정보 fail-open 가능성이다. 프로토콜을 교체해도 동일 방어가 유지되도록 검증을
공통화하고, 모든 구현에 적용되는 Sink contract harness로 비차단·무예외·수명주기 계약을
자동 검증해야 한다.
