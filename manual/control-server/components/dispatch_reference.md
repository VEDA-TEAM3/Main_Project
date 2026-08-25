# Dispatch 레퍼런스

> **대상 파일**
> - 디스패치 포트: `include/interfaces/IHwEventDispatcher.h`
> - 현재 구현: `src/dispatch/SerialHwEventDispatcher.h`, `.cpp`
> - 인코딩 헬퍼: `src/dispatch/SerialEventEncoding.h`
> - 와이어 프로토콜: `shared/driver_protocol.h` (STM32 펌웨어와 **공유**)

Dispatch는 Control Server 파이프라인의 **출력 경계(output boundary)** 이다. 판정된 위험 등급을
UART로 STM32에 통지하고,  STM32가 올려보내는 실제 표시 상태를 되받아 상류에 통지한다.

이 계층은 LED·경광등·부저를 **직접 제어하지 않는다.** 채널별 등급만 알려주고, 어떤 출력을 켤지는
STM32가 로컬에서 판단한다. 서버가 아는 것은 **"무엇을 지시했는가"** 와 **"무엇이 켜져 있다고
보고받았는가"** 두 가지뿐이며, 이 둘의 불일치를 감지하는 것이 이 계층의 핵심 책임이다.

하나의 물리 UART를 하행(이벤트 통지)과 상행(ACK/HEARTBEAT)이 공유하므로 두 방향을 인터페이스
하나로 묶었다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-08-13 | 1.0.0 | 재효 | Dispatch 계층의 양방향 계약·프레임 동기화·불일치 감지·워치독 수명 규칙 문서화 |

---

## 1. 디스패치 포트 계약

```cpp
class IHwEventDispatcher {
public:
    using StatusCallback = std::function<void(veda::ChannelId ch, bool alive,
                                              const HwIndicatorState& indicators)>;
    using FaultCallback  = std::function<void(veda::ChannelId ch, bool faulted)>;

    virtual void dispatch(const domain::RiskEvaluation& eval) = 0;
    virtual void setStatusCallback(StatusCallback callback) = 0;
    virtual void setFaultCallback(FaultCallback callback) = 0;
};
```

포트가 정의하는 것은 **하행 통지(`dispatch`)와 두 종류의 상행 콜백**뿐이다. 프레임 포맷,
재시도 정책, 타임아웃 판정은 모두 구현체의 세부사항이다.

| 계약 | 내용 |
|------|------|
| 입력 타입 | `domain::RiskEvaluation` (`zoneLevels`) |
| 호출 주체 | 파이프라인 스레드 (`Controller::processPipeline`) |
| 입력 수명 | `dispatch()` 호출 동안만 유효 |
| 콜백 실행 위치 | **`readerLoop` / `watchdogLoop` 스레드** — 파이프라인 스레드가 아님 |
| 콜백 호출 조건 | `alive` 또는 `indicators` 중 하나라도 **바뀔 때만** |
| 등록 시점 | 임의 — 등록 즉시 현재 스냅샷이 재생된다 (§6.2) |
| 스레드 안전 | `dispatch()` 와 콜백이 서로 다른 스레드에서 동시 진입 가능 |

### 1.1 ⚠️ `indicators` 는 `alive` 로 게이트되는 값이다

`HwIndicatorState` 는 단독으로 해석하면 안 된다.

```
alive == true   ->  indicators 는 실시간 유효값
alive == false  ->  indicators 는 끊기기 직전 '마지막으로 확인된' 값 (= stale)
```

> **어느 계층도 `alive` 를 무시한 채 표시 상태만 활성으로 해석해서는 안 된다.**

`alive=false` 로 콜백이 불릴 때도 `indicators` 는 비워지지 않는다. 끊긴 순간의 마지막 상태를
그대로 담아 보낸다. 이 계약은 `Controller` → `veda::ChannelStatus` → Qt 까지 그대로 이어진다.
중간 어느 계층에서든 "빨간 LED가 켜져 있다"만 보고 경보를 표시하면, **이미 죽은 보드의 1분 전
상태**를 실시간인 것처럼 띄우게 된다.

---

## 2. 하행 — 변경분만 전송

`dispatch()` 는 매 프레임(초당 10회) 호출되지만, 값이 바뀐 채널만 내보낸다.

```
파이프라인 스레드 ──▶ dispatch() ──▶ veda_downlink_frame_t (27B) ──▶ /dev/serial0 ──▶ STM32
```

### 2.1 ⚠️ 비교 기준은 "마지막 전송 **성공** 값"

```cpp
ssize_t written = write(fd_, &frame, sizeof(frame));
if (written != static_cast<ssize_t>(sizeof(frame))) {
    logError(kIface, "전송 실패: 채널 " + std::to_string(zone.zoneId) + ...);
    continue;  // lastSentLevel_ 갱신 안 함 -> 다음 프레임에서 재시도됨
}
lastSentLevel_[zone.zoneId] = zone.level;
```

`write()` 가 실패했는데 `lastSentLevel_` 을 갱신해 버리면, 다음 프레임에서 "값이 안 바뀌었다"는
이유로 전송이 생략된다. 등급이 다시 바뀌기 전까지 STM32는 **영원히 옛 등급에 머문다.**

전송 실패는 조용히 넘어가도 되는 사건이 아니라 **재전송해야 하는 사건**이므로, 성공했을 때만
기준값을 옮긴다.

### 2.2 인코딩 경계 — `SerialEventEncoding.h`

`dist_mm` 은 `uint16_t` 이고 `0xFFFF(65535)` 는 "값 없음" sentinel로 예약되어 있다.

| 입력 | `dist_mm` | 근거 |
|------|-----------|------|
| 음수 / NaN / 무한대 | `VEDA_DIST_MM_NONE` | 비유한값을 정수로 변환하면 UB |
| 65.535m 이상 | `65534` | sentinel과 충돌 방지 — 포화시켜 "먼 거리"로 전달 |
| `zoneId` < 0 또는 > 255 | (전송 생략) | `channel_id` 가 `uint8_t` |

범위 확인을 **곱셈보다 먼저** 수행한다. `distanceMeters * 1000.0` 을 먼저 계산하면
`uint16_t` 범위를 넘긴 뒤에 검사하게 되어 변환 자체가 정의되지 않은 동작이 된다.

### 2.3 바이트 순서는 명시적으로 쓴다

```cpp
veda_write_i64_le(&ev.timestamp_ms, eval.timestamp);
veda_write_u16_le(&ev.dist_mm, serial_event::encodeDistanceMm(zone.minDist));
```

구조체에 직접 대입하지 않고 `driver_protocol.h` 의 LE 헬퍼를 거친다. RPi(ARM)와 STM32가
같은 엔디안이라는 사실에 기대지 않기 위함이며, 와이어 포맷을 코드에 못 박는 역할도 한다.

---

## 3. 상행 — 프레임 동기화

`readerLoop()` 전용 스레드가 1바이트씩 읽으며 STM32의 `rx_task` 와 **대칭인 상태머신**을 돈다.

```
WAIT_START ──(0x53 'S')──▶ READ_PAYLOAD(16B) ──▶ READ_CHECKSUM ──▶ WAIT_END(0x45 'E')
     ▲                                                                     │
     └───────────── 체크섬/END/필드 검증 실패 시 버리고 재동기화 ──────────┘
```

| 단계 | 처리 |
|------|------|
| START 탐색 | `0x53` 이 나올 때까지 앞의 바이트는 **조용히 버린다** |
| payload | 16바이트 고정 수집 |
| checksum | XOR 체크섬 대조 |
| END | `0x45` 확인 |
| 필드 검증 | `veda_uplink_payload_is_valid()` 통과 시에만 처리 |

라인 노이즈로 프레임이 깨지는 것은 정상 운용 중에도 일어나므로, 재동기화 자체는 로그를 남기지
않는다. 다만 **체크섬·END를 통과했는데 필드가 이상한 경우**는 프로토콜 불일치를 의미하므로
에러 로그를 남긴다.

### 3.1 필드 검증이 잡는 것

```cpp
static inline int veda_uplink_payload_is_valid(const veda_uplink_packet_t* payload) {
    // reason 이 ACK/HEARTBEAT 중 하나인가
    // siren_on/buzzer_on/led_* 가 0 또는 1 인가
    // reserved0[0] == 0 인가
}
```

체크섬은 **전송 오류**를 잡지만 **의미상 잘못된 값**은 통과시킨다. `led_red = 7` 같은 값이
그대로 들어오면 `static_cast<bool>` 에서 `true` 가 되어 조용히 잘못된 판정으로 이어진다.
와이어 ABI 자체는 `static_assert(sizeof(...) == 16)` 으로 컴파일 시점에 고정되어 있다.

### 3.2 ⚠️ 상행에는 `risk_level` 필드가 없다

하행 `veda_risk_event_t` 와 달리, STM32는 **실제로 켜고 있는 LED on/off 상태만** 올려보낸다.
서버가 신호등 관례로 되돌려 해석한다.

| 상행 LED | 디코드 |
|----------|--------|
| `led_red` | `Danger` |
| `led_yellow` | `Warning` |
| 그 외 | `None` |

두 LED가 동시에 켜진 경우(전이 중 순간 등)에는 **더 위험한 쪽을 우선**한다. 이 비대칭은
프로토콜 설계상 의도된 것이다 — 서버는 "지시"를 보내고 STM32는 "사실"을 보고한다.

---

## 4. 불일치 감지와 에스컬레이션

ACK든 HEARTBEAT든 **매 상행 프레임마다** 대조한다.

```
일치              -> 재시도 카운터 리셋, fault 였다면 해소 통지
불일치 (n회 이내)  -> 같은 등급을 재전송
불일치 (재시도 소진) -> mismatchEscalateAfterRetries 에 따라 fault 통지
```

HEARTBEAT로도 검증되므로 **새 명령이 없어도 드리프트를 계속 감시**할 수 있다. 명령을 보낸 적
없는 채널은 비교 기준이 없으므로 건너뛴다.

재전송 시 `dist_mm` 은 `VEDA_DIST_MM_NONE` 으로 보낸다. 원본 거리값은 보관하지 않으며,
재전송의 목적은 "이 채널이 어떤 등급을 표시해야 하는지" 를 다시 알리는 것이지 거리 측정값
복원이 아니다.

### 4.1 ⚠️ `zoneId == channelId` 는 하중을 견디는 가정이다

```cpp
// 하행(dispatch):  lastSentLevel_[zone.zoneId]           // zoneId 를 키로 저장
// 상행(여기):      lastSentLevel_.find(pkt.channel_id)   // channel_id 를 키로 조회
```

이 대조는 **두 키가 같은 정수라는 사실에 전적으로 의존한다.** 현재 zone과 채널이 1:1이라
성립한다.

> 향후 ZoneId와 ChannelId를 분리하면(한 zone이 여러 채널을 구동 등) 둘 다 `int` 라
> **컴파일은 통과하지만 엉뚱한 채널끼리 비교**하게 된다.

그 결과는 불일치 감지·재전송·fault 에스컬레이션이 조용히 오작동하는 것이다. 하드웨어는 틀린
상태인데 서버는 정상이라고 착각한다(**silent hardware failure**). 분리할 경우 반드시
`lastSentLevel_` / `mismatchRetryAttempts_` / `faultState_` 를 번역된 ChannelId로 다시 키잉한다.

---

## 5. 하트비트 워치독

`watchdogLoop()` 이 `heartbeatIntervalMs` 마다 깨어나, 마지막 HEARTBEAT로부터
`heartbeatIntervalMs × missedBeatsForTimeout` (기본 500 × 3 = **1500ms**) 이 지난 채널을
dead로 판정한다.

### 5.1 ⚠️ 락을 쥔 채로 통지하면 데드락이다

```cpp
std::vector<veda::ChannelId> timedOutChannels;
{
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    // ... 타임아웃 채널을 '모으기만' 한다
}
for (veda::ChannelId ch : timedOutChannels) {
    reportAlive(ch, false);   // <- 락 밖에서 통지
}
```

`reportAlive()` 가 같은 `heartbeatMutex_` 를 다시 잠근다. `std::mutex` 는 재귀 획득이
불가능하므로 락 안에서 부르면 그 자리에서 멈춘다. **모으기와 통지를 분리하는 이 두 단계는
최적화가 아니라 정합성 요건이다.**

### 5.2 판정 규칙

| 상황 | 동작 | 이유 |
|------|------|------|
| 이미 `alive == false` | 건너뜀 | 통지는 어차피 dedup되지만, 가드가 없으면 dead 로그가 폴링마다 도배된다 |
| HEARTBEAT를 한 번도 못 받음 | 대상 아님 | `reportedState_` 초기값이 이미 `alive=false` — 통지할 전이가 없다 |
| 하트비트 재개 | `alive=true` 로 복구 | `reportAlive(true)` 가 `lastHeartbeatAt_` 도 함께 갱신 |

`lastHeartbeatAt_` 은 **전이 여부와 무관하게 항상** 갱신해야 한다. `alive` 가 이미 `true` 라고
갱신을 건너뛰면 시각이 멈춰 정상 동작 중인 채널이 타임아웃된다.

---

## 6. 동시성

| 스레드 | 역할 |
|--------|------|
| 파이프라인 스레드 | `dispatch()` — 하행 전송 |
| `readerThread_` | `readerLoop()` — 상행 수신·파싱·불일치 검증 |
| `watchdogThread_` | `watchdogLoop()` — 하트비트 타임아웃 판정 |

| 뮤텍스 | 보호 대상 |
|--------|-----------|
| `sendStateMutex_` | `lastSentLevel_`, `mismatchRetryAttempts_`, `faultState_`, `faultCallback_` |
| `heartbeatMutex_` | `lastHeartbeatAt_`, `reportedState_`, `statusCallback_` |

두 뮤텍스는 **함께 잠기지 않는다.** 하행 상태와 하트비트 상태는 독립적이며, 이 분리가 락 순서
문제를 구조적으로 없앤다.

### 6.1 종료 수명

소멸자는 `running_ = false` 후 두 스레드를 `join()` 한다. 실제 대기 시간은
`readerLoop` 의 `read()` 타임아웃(`VTIME = 10` → 최대 1초)과 `watchdogLoop` 의 폴링 주기
(`heartbeatIntervalMs`) 중 큰 쪽이 지배한다.

### 6.2 콜백 등록 시점의 공백 메우기

`readerLoop()` 는 생성 시점부터 돌기 때문에, 콜백을 뒤늦게 등록하면 그 이전에 파악된 상태를
놓친다. 그래서 `setStatusCallback()` / `setFaultCallback()` 은 **등록 즉시 현재 스냅샷을 한 번
통지**한다. 이 재생이 없으면 조립 순서에 따라 대시보드 초기 상태가 비어 보인다.

---

## 7. 설정 (`AppConfig::hwHealthCheck`)

| 키 | 기본값 | 설명 |
|----|--------|------|
| `dispatcher` | `"serial"` | `"serial"` = 실제 UART / `"console"` = 하드웨어 없이 콘솔 확인 |
| `devicePath` | `"/dev/serial0"` | STM32가 연결된 시리얼 장치 |
| `heartbeatIntervalMs` | `500` | STM32의 상태 보고 주기 |
| `missedBeatsForTimeout` | `3` | 연속 유실 시 dead 판정 기준 |
| `mismatchRetryCount` | `2` | 불일치 시 재전송 횟수 |
| `mismatchEscalateAfterRetries` | `true` | 재시도 소진 시 fault 통지 여부 |

UART는 **115200 8N1** 이다. 포트를 열지 못해도 예외를 던지지 않고 `fd_ == -1` 로 남아
`dispatch()` / `readerLoop()` 가 조용히 스킵한다 (`AppConfig::load` 와 동일한 원칙).

---

## 8. 관측성

| 로그 | 시점 | 수준 |
|------|------|------|
| `연결됨 (115200 8N1)` | 포트 오픈 성공 | Success |
| `포트 열기 실패` | 오픈 실패 — 전송/수신 비활성 | Error |
| `UART 통지 채널 N -> LEVEL` | 등급 전이 시에만 | Success |
| `전송 실패: 채널 N` | `write()` 실패 | Error |
| `명령-상태 불일치: 기대=X 실제=Y (재시도 n/m)` | 상행 대조 실패 | Error |
| `재시도 소진 -> fault 에스컬레이션` | fault 전이 | Error |
| `UART 상행 payload 필드 검증 실패` | 프로토콜 불일치 의심 | Error |

등급 전이 시에만 찍히므로 Success 수준이어도 도배되지 않는다. **상태 전이는 운영자가 봐야 할
이벤트**라는 판단이다.

---

## 9. 테스트 하네스

현재 `tests/control-server/unit/` 에 Dispatch 테스트가 **없다.** 추가할 경우 PTY(의사 터미널)를
가짜 시리얼 포트로 물리면 `readerLoop` / `watchdogLoop` 실제 코드 경로를 그대로 검증할 수 있다.

```cpp
int master = posix_openpt(O_RDWR | O_NOCTTY);
grantpt(master); unlockpt(master);
SerialHwEventDispatcher dispatcher(ptsname(master), /* heartbeatIntervalMs */ 300, ...);
// master 쪽에 veda_uplink_frame_t 를 write -> 실제 파서가 소비한다
```

검증할 것:

- HEARTBEAT 수신 후 `alive=true`, 표시 상태 동반 통지
- 타임아웃 경과 후 `alive=false`, **중복 통지 없음**
- 하트비트 재개 시 복구, 재차 끊길 때 다시 판정
- 불일치 프레임에 대한 재전송 횟수와 fault 전이
- 소멸자의 스레드 join 소요 시간

> **가짜 seam은 '시간'도 모델링해야 한다.** 프레임 도착을 기다리는 시간이 타임아웃보다 길면
> "살아 있는 상태"를 한 번도 관측하지 못하고 오탐한다. 대기 시간과 타임아웃의 대소 관계를
> `static_assert` 로 못 박는 편이 안전하다.

---

## 10. 확장 체크리스트

새 디스패처 구현을 추가하거나 프로토콜을 변경할 때 확인한다.

- [ ] `IHwEventDispatcher` 만 구현하고 `Controller` 를 수정하지 않았는가
- [ ] 비교 기준이 "마지막 전송 **성공** 값" 인가 (실패 시 갱신하지 않는가)
- [ ] 콜백을 뮤텍스 **밖**에서 호출하는가
- [ ] `alive` 와 `indicators` 를 함께 통지하는가 (stale 계약을 문서화했는가)
- [ ] 콜백 등록 시 현재 스냅샷을 재생하는가
- [ ] 상행 필드 검증을 거치는가 (체크섬만 믿지 않는가)
- [ ] 바이트 순서를 LE 헬퍼로 명시했는가
- [ ] `driver_protocol.h` 를 고쳤다면 **STM32 펌웨어도 함께** 고쳤는가
- [ ] `static_assert` 로 와이어 크기가 고정되어 있는가

---

## 11. 현재 구조의 개선 과제

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| 재연결 | 없음 — 포트 오픈 실패 시 `fd_ == -1` 로 영구 비활성 | 백오프 재시도 루프 도입 (서버 재시작 없이 복구) |
| 워치독 폴링 주기 | `heartbeatIntervalMs` 와 동일 | 판정 지연이 최대 1주기 늘어난다 — 더 짧은 고정 주기 검토 |
| `missedBeatsForTimeout = 0` | 방어 없음 — 정상 하트비트도 즉시 dead | 조립 시점 검증 또는 최소 1로 clamp |
| 생성자 인자 | `uint32_t` 3개 연속 (`bugprone-easily-swappable-parameters`) | 설정 구조체(`HwHealthCheckConfig`)를 그대로 받기 |
| 테스트 | `tests/` 미등록 | §9의 PTY 하네스로 등록 |
| `zoneId == channelId` | 암묵 가정 (§4.1) | 타입 분리 시 키잉 재설계 필수 |

인터페이스는 작게 유지한다. 링크 상태 조회, 통계 노출, 강제 재전송 같은 메서드를
`IHwEventDispatcher` 에 추가하지 않는다. — 그런 기능은 구현체 또는 별도 관측 인터페이스의 책임이다.
