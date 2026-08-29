# 요구사항 명세서 (SRS)

> **작성 기준**: 이 문서는 기획서가 아니라 **현재 저장소의 코드**를 역공학해 작성했다.
> 기획서와 어긋나는 항목은 코드를 정답으로 본다. 근거 열에 실제 심볼/파일을 적었으므로
> 요구사항이 살아 있는지는 그 심볼을 찾아 확인할 수 있다.
>
> **상태 표기**: **구현**(코드에 존재하고 동작 확인) / **부분**(경로는 있으나 검증·계약이 미완) /
> **제안**(승인 필요, 코드 근거 없음) / **전제**(범위 밖이지만 성립해야 하는 조건)
>
> **측정 표기**: **[실측]** 은 `performance/` 와 `manual/control-server/*_Performance_Metrics.md` 에
> 원본 출력이 남아 있는 값이고, **[추정]** 은 코드 상수와 링크 속도에서 산술로 유도한 값이다.
> 실측 대부분은 x86 개발 장비(WSL2, g++ -O2)이며 **라즈베리파이 4(ARM64)에서는 절대값이 3~5배**
> 커진다 — §4 예산 표의 "Pi4 예산" 열은 그 배수를 반영한 상한이다.

---

## 1. 목적과 범위

CCTV 다채널 ONVIF 메타데이터를 엣지(라즈베리파이)에서 좌표로 변환하고, 중앙에서 채널을 융합해
주차장 위험을 판정한 뒤 **STM32 경보장치**와 **Qt 관제 클라이언트**에 전달하는 시스템의 요구사항 기준선이다.

### 1.1 범위 안 (이 저장소가 책임짐)

| 구성요소 | 경로 | 책임 |
| --- | --- | --- |
| compute-server | `compute-server/` | 채널당 1프로세스. RTSP/ONVIF 수신 → 정규화 → 정제 → 라우팅 → 지면점/호모그래피 → MQTT 발행 |
| control-server | `control-server/` | 단일 프로세스. 다채널 집계 → 월드 변환 → 융합 → 주차 억제 → zone 배정 → 위험 판정 → UART·MQTT 발행 |
| client (Qt) | `client/` | 관제 대시보드. RTSP 영상 + blur 합성, 디지털 트윈, 이벤트 로그, 장비 상태, Slack 보고 |
| driver (STM32) | `driver/` | RS-485 슬레이브 펌웨어. LED/부저/경광등 구동, ACK/HEARTBEAT 상행 |
| 공유 계약 | `shared/` | `Contract.h` (MQTT JSON wire, schema v1), `driver_protocol.h` (UART 바이너리) |

### 1.2 범위 밖 (외부 시스템)

- CCTV 내부 AI 추론 정확도, 영상 인코딩/저장
- MQTT broker 운영 (제품 선정, ACL, cluster/HA)
- 도면 실측·캘리브레이션 데이터의 물리적 정확도 (`deploy/compute-server/calibration/` 은 도구만 제공)

### 1.3 최우선 품질 속성

이 시스템은 **실시간성**과 **자원 사용량**이 정확도·기능 확장성보다 우선한다. 근거:

- 배포 단위가 라즈베리파이 4(4GB)이고 컨테이너당 **CPU 0.75 core / RSS 256MB** 로 강제된다
  (`deploy/compute-server/compute_server_deployment.md`).
- 경보는 사람이 차량에 접근하는 상황을 다루므로, 늦은 정답보다 **예산 안의 답**이 가치가 크다.
- 따라서 모든 큐는 **bounded + drop-oldest** 이고, hot path 의 **힙 할당 0** 을 목표로 한다.

---

## 2. 시스템 개요

```
CCTV(4채널) --RTSP/TCP(TLS)--> compute-server × N --MQTT--> broker
                                                              |
                                    +-------------------------+-------------------+
                                    |                                             |
                             control-server                                    client(Qt)
                          (집계→융합→위험판정)                          (영상+blur, 디지털트윈)
                                    |
                          UART 115200 8N1 (RPi → 마스터)
                                    |
                          RS-485 (마스터 → 슬레이브 STM32)
```

| 토픽 | 방향 | QoS | retained | 계약 |
| --- | --- | --- | --- | --- |
| `veda/ch/{ch}/topview` | compute → control | 0 | 아니오 | `veda::TopViewFrame` |
| `veda/ch/{ch}/blur` | compute → client | 0 | 아니오 | `veda::BlurFrame` |
| `veda/ch/{ch}/alive` | compute → control/client | 1 | 예 (LWT) | `"1"` / `"0"` 1바이트 |
| `veda/risk` | control → client | 0 | 아니오 | `veda::RiskFrame` |
| `veda/hw/ch/{ch}/status` | control → client | 1 | 예 | `veda::ChannelStatus` |
| `veda/hw/status` | control → client | 1 | 예 | 레거시 호환 payload |

> **불일치 발견**: `Contract.h` 의 `topic::kRisk` 주석은 "QoS 1" 이라고 적혀 있으나 실제
> `qos::kRisk = 0` 이고 `control-server/src/sink/MqttTransport.cpp:282` 도 0으로 발행한다.
> 주석을 코드에 맞춰야 한다 (구 SRS 의 FR-017 이 "QoS 1" 로 적혀 있던 원인).

---

## 3. 기능 요구사항

### 3.1 compute-server (엣지)

| ID | 요구사항 | 검증 기준 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| FR-101 | 채널별 CCTV의 ONVIF 메타데이터를 RTSP/TCP interleaved 로 수신해야 한다. | 정상 스트림에서 `RawPacket` 이 프레임 단위로 조립되어 파이프라인에 전달된다. | `RtspClientV2`, `RtspOnvifSourceV2` | 구현 |
| FR-102 | RTSP 는 기본적으로 TLS(서버 인증서 검증)를 사용해야 하며, 평문은 명시적 설정으로만 허용해야 한다. | `rtspUseTls` 기본값이 `true` 이고 CA/SNI 불일치 시 연결이 거부된다. | `AppConfig::rtspUseTls` / `rtspCaFile` / `rtspServerName` | 구현 |
| FR-103 | 연결 단절 시 지수 backoff(1s → 30s 상한)로 재접속하고, 종료 요청 시 블로킹 I/O 를 즉시 해제해야 한다. | 단절 후 재접속하며 `stop()` 이후 worker 가 join 된다. | `RtspOnvifSourceV2`, `rtspReconnectBackoff*Sec` | 구현 |
| FR-104 | RTSP 세션은 `rtspKeepAliveIntervalSec`(기본 30s) 주기로 GET_PARAMETER 를 보내 유지해야 한다. | keep-alive 스레드가 주기적으로 요청을 보내고 세션이 만료되지 않는다. | `RtspClientV2::keepAliveLoop` | 구현 |
| FR-105 | ONVIF 좌표계([-1,1], 중앙 원점, +y 위)를 내부 표준([0,1], 좌상단 원점, +t 아래)으로 변환해야 한다. | `<tt:Transformation>` 반영 결과가 `NormRect` 계약과 일치한다. | `OnvifParser`, `Contract.h::NormRect` | 구현 |
| FR-106 | 객체 클래스는 벤더 별칭과 대소문자를 허용해 정규화해야 한다. | `Car` / `Person` / `VEHICLE` 등이 정식 `ObjectClass` 로 매핑된다. | `Contract.h::objectClassFromString` | 구현 |
| FR-107 | 비유한 좌표·과대 입력·중복/포함 객체를 정책에 따라 제거해야 한다. | invalid/overflow 입력이 drop 되고 정상 객체만 후속 단계로 간다. | `OnvifParser`, `ContainmentSanitizer` (IoU 0.5 / contain 0.9) | 구현 |
| FR-108 | 부모 없는 Human/Vehicle 은 risk 경로, 부모 있는 Head/LicensePlate 는 blur 경로로 분리해야 한다. | 동일 프레임에서 두 출력 목록이 규칙에 맞게 생성된다. | `ParentBasedRouter`, `isRiskClass` / `isBlurClass` | 구현 |
| FR-109 | 부모 정보로 개인정보 대상임이 확인됐지만 Type 을 식별 못한 객체도 blur 출력에 포함해야 한다. | `Unknown` + parent 있음 → blur 목록에 남는다. | `isBlurOutputClass` | 구현 |
| FR-110 | bbox 아래변이 잘린 risk 객체는 지면점을 신뢰할 수 없으므로 정책에 따라 폐기해야 한다. | 기본 `dropBottomTruncated` 에서 하단 잘림 객체가 drop 되고 누적 카운트가 로그된다. | `Pipeline::isEdgeRejected`, `riskEdgePolicy` | 구현 |
| FR-111 | risk 객체의 bbox 하단 중앙점을 승인된 호모그래피로 **CCTV 로컬** 지면 좌표(m)로 변환해야 한다. | 캘리브레이션 기준점 오차가 허용 범위 이내이고, 지평선 위/범위 밖은 `nullopt` 로 폐기된다. | `BottomCenterExtractor`, `HomographyTransform`, `localBounds*` | 구현 |
| FR-112 | compute-server 는 도면(월드) 좌표를 알아서는 안 된다. | 발행 타입이 `LocalPoint` 이며 `WorldPoint` 로는 컴파일되지 않는다. | `Contract.h::LocalPoint` | 구현 |
| FR-113 | blur bbox 는 앱 표시 좌표계로 매핑하고 지연 보상 배율(`blurBoxScale`, 기본 1.25)을 적용해야 한다. | 출력 bbox 가 `[0,1]` 이고 1.0 미만 축소는 설정으로도 불가능하다. | `AffineImageCoordinateMapper`, `kMaxBlurBoxScale = 2.0` | 구현 |
| FR-114 | 객체가 하나도 없는 프레임도 발행해야 한다 (빈 프레임과 채널 끊김의 구분). | 객체 0개인 `TopViewFrame` / `BlurFrame` 이 정상 발행된다. | `Pipeline::onPacket` | 구현 |
| FR-115 | MQTT LWT + retained 로 채널 alive/dead 상태를 제공해야 한다. | 연결 / 정상종료 / 비정상종료 각각에서 `veda/ch/{ch}/alive` 가 갱신된다. | compute `MqttTransport` | 구현 |
| FR-116 | 정상 종료(SIGINT/SIGTERM)에서는 dead 를 **직접 발행**하고 종료해야 한다 (LWT 대기 없이). | `docker stop` 후 control-server 가 1초 내 채널 다운을 인지한다. | `main.cpp`, 배포 매뉴얼 §4-4 | 구현 |

### 3.2 control-server (중앙)

| ID | 요구사항 | 검증 기준 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| FR-201 | 모든 채널의 topview/alive 토픽을 구독하고 topic·payload·schema·channel 을 검증해야 한다. | 잘못된 topic prefix, 64KB 초과 payload, schema version 불일치, 범위 밖 channel 이 drop 된다. | `MqttChannelReceiver` (`kMaxTopViewPayloadBytes = 64KB`) | 구현 |
| FR-202 | 수신 큐는 메시지 수와 바이트 수 양쪽에 상한을 두고 drop-oldest 로 동작해야 한다. | 과부하에서 큐가 `kMaxQueuedPayloadBytes = 8MB` 를 넘지 않고 최신 프레임이 살아남는다. | `MqttChannelReceiver::enqueue` | 구현 |
| FR-203 | 프레임은 `windowSizeMs`(기본 **220ms**) 시간 윈도우로 집계되어야 한다. | 윈도우 마감 시 채널별 최신 프레임 묶음이 정확히 한 번 콜백된다. | `TimeWindowAggregatorV2` | 구현 |
| FR-204 | 집계 윈도우는 소스 프레임 주기 `T_src`(실측 201.6ms)보다 **커야** 한다. | 220ms 에서 윈도우당 4/4 채널이 잡히고, 100ms 설정에서는 채널 조각화가 재현된다. | `AppConfig::windowSizeMs` | 구현 |
| FR-205 | 집계 콜백은 락 밖에서 호출되어야 한다 (다른 채널 `push()` 를 파이프라인 시간만큼 막지 않기 위해). | 평균 락 보유 시간이 10µs 미만이다 (실측 2.92µs). | `TimeWindowAggregatorV2`, `performance/control-server.md` | 구현 |
| FR-206 | 채널 로컬 좌표를 카메라 설치 위치·방위각으로 공통 월드 좌표로 변환해야 한다. | 캘리브레이션 기준점이 월드 좌표 허용오차 안에 들고, 타입이 `LocalPoint`→`WorldPoint` 로 바뀐다. | `AffineLocalToWorldTransform`, `CameraCalibration` | 구현 |
| FR-207 | 서로 다른 채널의 동일 클래스 근접 객체를 하나로 융합하고 안정적 `GlobalId` 를 부여해야 한다. | 중첩 시 단일 객체, 재관측 시 `trackMaxDistance`(4.0m) 내 동일 gid 유지. | `GridFuser` (spatial hash, 16384 버킷) | 구현 |
| FR-208 | 융합 객체는 기여 채널 집합(provenance)을 힙 할당 없이 보존해야 한다. | `sourceChannels` 가 최대 8개까지 실제 채널 ID 를 담고, 초과 시 `truncated` 로 표시된다. | `domain::SourceChannelSet` | 구현 |
| FR-209 | 미관측 트랙은 최대 **10 윈도우** 동안 마지막 좌표로 coast 유지해야 한다 (채널 인계 시 UI 깜박임 방지). | 채널이 잠시 놓친 객체가 UI 에서 사라졌다 나타나지 않는다. | `GridFuser::kMaxMissedWindows = 10` | 구현 |
| FR-210 | 정지 좌표의 잡음은 공간 히스테리시스(`positionJitterRadius`, 기본 0.15m)로 고정해야 한다. | 정지 객체 좌표가 반경 내에서 진동하지 않고, 실제 이동은 지연 없이 따라간다. | `GridFuser`, `RiskConfig::positionJitterRadius` | 구현 |
| FR-211 | 주차면 폴리곤 안에서 `stationaryDurationMs`(기본 5000ms) 이상 정지한 차량은 위험 판정에서 제외해야 한다. | 주차 차량이 `WorldFrame` 에서 제거되어 경보를 만들지 않는다. | `StationaryParkingPolicy` (tolerance 0.3m, gap 1000ms) | 구현 |
| FR-212 | coast 객체는 주차 판정의 **관측 증거로 쓰지 않아야** 한다. | `sourceChannels` 가 비면 시간 기준을 갱신하지 않고 기존 판정만 유지한다. | `StationaryParkingPolicy::shouldSuppress` | 구현 |
| FR-213 | 월드 객체를 공간 zone 에 배정하고 경계 jitter 에 히스테리시스(`hysteresisMargin`, 기본 0.5m)를 적용해야 한다. | 경계 부근 잡음에서 zone 이 진동하지 않는다. | `SpatialZoneMapper` | 구현 |
| FR-214 | zone 선택 이력은 최대 **5 누락 윈도우** 동안 유지해야 한다. | 220ms 기준 약 1.1초의 채널 누락에도 히스테리시스가 초기화되지 않는다. | `SpatialZoneMapper::kMaxMissedWindows = 5` | 구현 |
| FR-215 | 차량 기준 최근접 객체 거리가 임계값 이하면 Warning(≤5.0m)/Danger(≤2.0m)를 판정하고, 차량이 없으면 None 이어야 한다. | danger ≤ warning 경계와 "차량 없음 → 전부 None" 이 정책대로 동작한다. | `ThresholdRiskPolicy` | 구현 |
| FR-216 | 비유한 좌표 객체는 판정에 참여시키지 않아야 한다 (NaN 은 모든 비교가 false 라 조용한 무경보가 된다). | NaN 좌표가 O(N) 단계에서 한 번에 걸러지고 카운트된다. | `ThresholdRiskPolicy::evaluate` 1단계 | 구현 |
| FR-217 | 조립 시점에 임계값이 구조적으로 잘못되면 fail-fast 해야 한다. | 비유한/음수 임계값, `dangerous > warning`, null metric 에서 예외로 기동이 거부된다. | `ThresholdRiskPolicy` 생성자 | 구현 |
| FR-218 | Qt 전체 위험도와 STM32 zone 위험도는 **같은** `RiskEvaluation` 에서 파생되어야 한다. | 동일 프레임에서 `RiskFrame.level == max(zoneLevels.level)` 이다. | `Controller::processPipeline` | 부분 |
| FR-219 | 월드 객체와 위험도를 `veda/risk`(QoS 0)로 발행해야 하며, 위험 객체만이 아니라 프레임의 모든 객체를 보내야 한다. | 매 윈도우 `RiskFrame` 이 발행되고 객체 목록이 전체다. | control `MqttTransport`, `qos::kRisk = 0` | 구현 |
| FR-220 | 카메라·하드웨어 상태 변화는 채널별 retained `ChannelStatus` 로 발행해야 한다. | 재접속한 client 가 최신 상태를 즉시 수신한다. | `Controller::onChannelAlive` / `onHardwareStatus` | 구현 |
| FR-221 | 상태 발행(블로킹 가능한 MQTT publish)은 반드시 **락 밖에서** 수행해야 한다. | 상태 뮤텍스 보유 구간에 I/O 가 없다. | `Controller::buildStatusLocked` | 구현 |
| FR-222 | 상태는 **전이가 일어났을 때만** 발행해야 한다 (retained 재전달·반복 신호로 도배되지 않도록). | 동일 상태 반복 수신 시 추가 publish 가 없다. | `Controller`, `SerialHwEventDispatcher` dedup | 구현 |
| FR-223 | 설정 검증은 zone ID 유일성, 캘리브레이션 채널 유일성, zone 개수 = `channelCount` 를 강제해야 한다. | 중복/누락 설정에서 기동이 거부된다. | `AppConfig::validate` | 구현 |

### 3.3 하드웨어 경보 (control-server ↔ STM32)

| ID | 요구사항 | 검증 기준 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| FR-301 | zone 별 위험 이벤트를 **UART 115200 8N1** 로 마스터에 전송해야 한다. | 24바이트 payload + start/checksum/end = 27바이트 프레임이 전송된다. | `SerialHwEventDispatcher::open`, `veda_downlink_frame_t` | 구현 |
| FR-302 | 마스터는 수신한 이벤트를 **RS-485** 로 슬레이브 STM32 에 중계해야 한다. | 슬레이브가 자기 채널 범위(`MY_FIRST_CHANNEL`~`MY_LAST_CHANNEL`)의 이벤트만 반영한다. | `driver/rs485_receive_2/Core/Src/veda_rs485.c`, `veda_config.h` | 구현 |
| FR-303 | 하행 프레임은 XOR 체크섬과 start/end 바이트로 프레이밍해야 한다. | 체크섬 불일치 프레임은 폐기된다. | `veda_checksum`, `VEDA_START_BYTE` / `VEDA_END_BYTE` | 구현 |
| FR-304 | 다중 바이트 필드는 little-endian 명시 헬퍼로만 읽고 써야 한다 (RPi/STM32 이식성). | `veda_write_u16_le` / `veda_read_i64_le` 외 직접 캐스팅이 없다. | `driver_protocol.h` | 구현 |
| FR-305 | 변경분만 전송해야 한다 (동일 레벨 재전송 금지). | 같은 zone 의 레벨이 유지되는 동안 하행 프레임이 나가지 않는다. | `lastSentLevel_` | 구현 |
| FR-306 | STM32 는 ACK/HEARTBEAT 상행(16바이트)으로 **실제 표시 상태**(siren/buzzer/LED 3색)를 보고해야 한다. | `veda_uplink_packet_t` 필드가 `ChannelStatus` 로 전달된다. | `veda_uplink_packet_t`, `handleUplinkFrame` | 구현 |
| FR-307 | 하트비트가 `heartbeatIntervalMs × missedBeatsForTimeout`(기본 500×3 = **1500ms**) 동안 없으면 채널을 dead 로 판정해야 한다. | 보드 전원 차단 후 워치독 1주기 추가 지연 내에 dead 가 된다. | `watchdogLoop`, `HwHealthCheckConfig` | 구현 |
| FR-308 | 명령과 실제 표시 상태가 불일치하면 `mismatchRetryCount`(기본 2)회 재전송하고, 소진 시 fault 로 에스컬레이션해야 한다. | 강제 불일치 주입 시 재전송 후 fault 콜백이 발생한다. | `mismatchRetryAttempts_`, `faultState_` | 부분 |
| FR-309 | `hardwareAlive=false` 인 동안의 표시 상태 필드는 **stale** 로 계약해야 하며, client 는 활성 상태로 렌더링해서는 안 된다. | client 가 dead 채널의 사이렌을 켜진 것으로 그리지 않는다. | `Contract.h::ChannelStatus` STRICT 계약 | 부분 |
| FR-310 | 슬레이브는 위험 레벨을 색으로 표시해야 한다 (None=초록, Warning=주황, Danger=빨강). | NeoPixel 색상이 `veda_config.h` 정의와 일치한다. | `NEOPIXEL_{NONE,WARNING,DANGER}_*` | 구현 |

### 3.4 client (Qt 관제)

| ID | 요구사항 | 검증 기준 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| FR-401 | 로그인으로 관제 세션을 시작해야 하며, 계정 저장소는 실행 파일 옆 파일이어야 한다 (폴더째 이동 가능). | `users.json` 으로 인증되고 머신 종속 저장소(DPAPI/레지스트리)를 쓰지 않는다. | `LocalFileAuthGateway`, `LoginWindow` | 구현 |
| FR-402 | 채널별 RTSP 영상을 GStreamer 로 수신해 대시보드에 표시해야 한다. | 채널 수만큼 수신기가 기동되고 첫 디코딩 프레임 신호가 온다. | `GstRtspReceiver`, `StreamSessionManager` | 구현 |
| FR-403 | 수신기 기동은 분산해야 한다 (동시 기동으로 인한 대역/CPU 스파이크 방지). | `initialStartDelayMs`(1000) 후 `receiverStartSpacingMs`(3000) 간격으로 순차 기동한다. | `app_config.example.json`, `StreamSessionManager` | 구현 |
| FR-404 | `veda/ch/+/blur` 의 좌표만으로 영상 위 민감 영역을 가려야 한다 (픽셀 전송 없음). | 얼굴/번호판이 blur 처리되어 표시된다. | `BlurProcessor`, `BlurVideoFilter` | 구현 |
| FR-405 | blur 메타데이터는 영상 프레임의 UTC 시각으로 매칭해야 하며 RTP timestamp 를 쓰면 안 된다. | `matchToleranceMs`(250) 내 매칭되고 어긋난 프레임은 사용하지 않는다. | `VideoUtcClockMapper`, `BlurFrameBuffer` | 구현 |
| FR-406 | 메타데이터가 잠시 끊겨도 마지막 값을 제한 시간 동안 유지하고, 한계를 넘으면 외삽을 멈춰야 한다. | `holdLastMetadataMs`(1000) / `maxExtrapolationMs`(500) 를 넘으면 blur 가 해제된다. | `BlurRuntimeConfig` | 구현 |
| FR-407 | `veda/risk` 를 구독해 디지털 트윈(top-view)에 객체와 위험도를 그려야 한다. | 객체가 월드 좌표에 배치되고 위험 등급별로 스타일이 달라진다. | `DigitalTwinMapWidget`, `DigitalTwinObjectStyleProvider` | 구현 |
| FR-408 | 오래된 프레임은 만료 처리해 화면에 남기지 않아야 한다. | `frameExpiryMs`(5000) 이후 객체가 사라진다. | `DigitalTwinRuntimeConfig` | 구현 |
| FR-409 | 위험 쌍(차량↔사람)의 신규 발생·등급 변화를 이벤트 로그로 남겨야 한다. | 동일 쌍의 동일 등급이 중복 기록되지 않는다. | `EventLogGenerator::activePairRiskLevels_` | 구현 |
| FR-410 | `veda/hw/ch/+/status` 로 채널별 카메라/하드웨어 상태와 표시 상태를 대시보드에 표시해야 한다. | 카메라 dead 와 하드웨어 dead 가 구분되어 보인다. | `DeviceStatusPanel`, `DeviceStatusService` | 구현 |
| FR-411 | 운영자가 특정 채널 상황을 Slack 으로 보고할 수 있어야 한다. | 보고 요청이 큐잉되어 순차 전송되고, 실패 시 사유가 표시된다. | `SlackReportGateway`, `openReportConfirmationDialog` | 구현 |
| FR-412 | client 도 payload 크기·schema 를 검증하고 잘못된 메시지를 drop 해야 한다. | malformed JSON 이 UI 를 깨뜨리지 않는다. | `MqttPayloadLimits`, `RiskMessageParser`, `BlurMessageParser` | 부분 |
| FR-413 | 월드 표시 범위는 고정 경계 또는 자동 경계(워밍업 후 표본 기반)를 선택할 수 있어야 한다. | `fixedBoundsEnabled` 전환 시 좌표계가 일관되게 유지된다. | `DigitalTwinRuntimeConfig`, `MapSettingsDialog` | 구현 |

### 3.5 공통 / 운영

| ID | 요구사항 | 검증 기준 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| FR-501 | 모든 서버 설정은 JSON 파일로 주입하며, 파싱 실패·파일 없음에서도 **예외 없이** 기본값으로 계속해야 한다. | 깨진 config 로도 프로세스가 뜨고 경고가 남는다. | compute `AppConfig::load` | 구현 |
| FR-502 | 설정 파일은 크기 상한(1MB)을 검사해야 한다. | 초과 파일은 읽지 않고 기본값을 쓴다. | `kMaxConfigFileBytes` | 구현 |
| FR-503 | 문자열 설정은 제어문자와 길이 상한을 검사해야 한다 (로그 인젝션·URI 변조 방지). | 제어문자 포함 값이 비워지고 경고가 남는다. | `rejectUnsafeText` | 구현 |
| FR-504 | 로그 파일 경로는 상위 경로(`..`)를 포함하지 않는 **상대 경로**여야 한다. | 절대경로/탈출 경로가 기본값으로 되돌려진다. | `isSafeRelativePath` | 구현 |
| FR-505 | 로깅은 비동기 큐 기반이어야 하며 호출 스레드에서 I/O 를 하지 않아야 한다. | `logSuccess` / `logError` 호출 비용이 2µs 대다. | `shared/Logger.h` | 구현 |
| FR-506 | 프레임마다 도는 정상 이벤트는 Debug 레벨이어야 하며 기본값(`info`)에서 문자열 조립조차 하지 않아야 한다. | Debug 경로 비용이 ~2ns 다. | `isLogEnabled` 가드 | 구현 |
| FR-507 | 정상 종료 시 worker 스레드와 네트워크 자원을 회수해야 한다. | SIGINT/SIGTERM 후 제한 시간 내 종료하고 fd/스레드 누수가 없다. | 양 서버 `main.cpp`, 각 `stop()` | 구현 |
| FR-508 | 채널당 컨테이너 1개로 배포하고 설정은 읽기 전용 마운트여야 한다. | `config.json` 이 `ro` 로 마운트된다. | `deploy/compute-server/setup_cctv.sh` | 구현 |
| FR-509 | `channelId` 는 브로커 전역에서 유일해야 한다 (정책: `cctvId × 4 + 로컬채널`). | 중복 시 control-server 에서 채널이 겹치는 증상이 재현된다. | 배포 매뉴얼 §2, 부록 A | 구현 |
| FR-510 | 데모용 `demoPedestrianProxy` 는 기본 off 여야 하며, 켜져 있으면 error 레벨 경고를 남겨야 한다. | 운영 기동 로그에서 즉시 식별된다. | `AppConfig::demoPedestrianProxy` | 구현 |

---

## 4. 성능 요구사항 (실시간성)

시각 기준점은 **CCTV ONVIF `UtcTime`** 이다. 이 값이 파이프라인 전 구간의 `ts` 로 전파된다.

### 4.1 종단 지연 예산

| # | 구간 | x86 실측 / 근거 | **Pi4 예산** | 출처 |
| --- | --- | --- | --- | --- |
| 1 | RTSP 프레임 조립 | 27.2 ms [실측, 평문] | **≤ 40 ms** | `RtspClientV2`, `performance/compute-server.md` |
| 2 | Source 링버퍼 큐 지연 | 0.051 ms [실측] | **≤ 0.5 ms** | `RtspOnvifSourceV2` |
| 3 | compute 파이프라인 (parse→sanitize→route→ground→homography) | < 1 ms [추정, 객체 20개] | **≤ 3 ms** | `Pipeline::onPacket` |
| 4 | MQTT publish → broker → control 수신 | 1~3 ms [추정, 동일 LAN, QoS 0] | **≤ 5 ms** | compute `MqttTransport` |
| 5 | **집계 윈도우 대기** | 0~220 ms, 평균 110 ms | **평균 110 ms / 최대 220 ms** | `windowSizeMs = 220` |
| 6 | control 파이프라인 (transform→fuse→parking→zone→risk) | 0.05~0.3 ms [실측, N=20~200] | **≤ 5 ms** | risk 0.6µs@N=20, metric 41.9µs@N=200 |
| 7a | UART 하행 프레임 직렬화 (27 B @ 115200 8N1) | 2.34 ms [추정, 산술] | **≤ 3 ms** | `veda_downlink_frame_t` |
| 7b | RS-485 중계 + STM32 반영 (LED/부저) | — | **≤ 10 ms** [추정] | `veda_rs485.c`, `veda_strip.c` |
| 8a | MQTT risk publish → client 수신 | 1~3 ms [추정] | **≤ 5 ms** | `qos::kRisk = 0` |
| 8b | client 렌더 틱 | 50 ms 주기 | **≤ 50 ms** | `renderIntervalMs = 50` |

| ID | 요구사항 | 목표 | 상태 |
| --- | --- | --- | --- |
| PR-001 | **CCTV `ts` → STM32 표시 변경** 종단 지연 (구간 1~7) | p50 **≤ 200 ms**, p95 **≤ 350 ms**, p99 **≤ 500 ms**<br>전형 합: 27 + 0.05 + 1 + 2 + 110 + 0.3 + 2.3 + 10 ≈ **153 ms** | 제안 |
| PR-002 | **CCTV `ts` → Qt 디지털 트윈 반영** 종단 지연 (구간 1~6, 8) | p50 **≤ 250 ms**, p95 **≤ 400 ms**, p99 **≤ 550 ms**<br>전형 합 ≈ **196 ms** | 제안 |
| PR-003 | **CCTV `ts` → Qt 영상 위 blur 표시** 지연 | 영상 파이프라인 `latencyMs = 350` 이 지배. p95 **≤ 600 ms** | 제안 |
| PR-004 | 관측 지연(샘플링): 사건 발생 → CCTV 가 메타데이터를 내보내기까지 | `T_src = 201.6 ms` [실측 4.96 fps]. **예산 밖(외부)** 이지만 안전 논의 시 반드시 합산 | 전제 |

> **PR-001 의 지배 항목은 집계 윈도우(220ms)** 다. 이 값을 줄이려면 CCTV fps 를 먼저 올려야 하고
> (`windowSizeMs > T_src` 불변식), 줄일 때 `trackMaxDistance` 를 반드시 재계산해야 한다 (§7 INV-02).

### 4.2 단계별 처리 예산

| ID | 요구사항 | 목표 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| PR-010 | `TimeWindowAggregatorV2::push()` 지연 | p50 ≤ 0.5 µs, **p99 ≤ 1 µs** (x86 실측 p50 0.10 / p99 0.22 µs) | Aggregator 지표 | 구현 |
| PR-011 | 집계기 평균 락 보유 시간 | **< 10 µs** (실측 2.92 µs, V1 71.95 µs 대비 95.9% 감소) | `performance/control-server.md` | 구현 |
| PR-012 | 윈도우 마감 주기 지터 | 220 ms ± 20 ms | `flushLoop` | 부분 |
| PR-013 | 거리 계산 비용 | **≤ 3 ns/쌍** (실측 `sqrt` 2.1~2.7 ns/쌍) — `hypot` 사용 금지 | `EuclideanMetric`, Metric 지표 | 구현 |
| PR-014 | 위험 판정 1프레임 | N=20 ≤ 5 µs, N=200 ≤ 250 µs (x86 실측 0.6 µs / 50 µs, Pi 5배 가정) | Risk 지표 | 구현 |
| PR-015 | 로그 1건 호출 비용 | `logSuccess` ≤ 2.5 µs, 차단된 Debug ≤ 10 ns (실측 1.9 µs / 2 ns) | Logger 지표 | 구현 |
| PR-016 | RTSP `recv()` syscall | ≤ 2.5 회/프레임 (실측 2.04, V1 6.52 대비 68.7% 감소) | `RtspClientV2` | 구현 |
| PR-017 | 하드웨어 dead 판정 지연 | ≤ **2000 ms** (하트비트 타임아웃 1500 ms + 워치독 폴링 1주기 500 ms) | `watchdogLoop` | 구현 |
| PR-018 | 카메라 dead 인지 지연 — 정상 종료 | ≤ **1000 ms** (프로세스가 dead 를 직접 발행) | compute `main.cpp` | 구현 |
| PR-019 | 카메라 dead 인지 지연 — 비정상 종료(LWT) | ≤ **45 s** (`mqttKeepAliveSeconds = 30` × 1.5) | broker LWT 규약 | 구현 |
| PR-020 | RTSP 재접속 소요 | 초기 1 s, 상한 30 s (지수 backoff) | `rtspReconnectBackoff*Sec` | 구현 |
| PR-021 | MQTT 재접속 소요 | 초기 1 s, 상한 10 s | `mqttReconnectDelay*Sec` | 구현 |

---

## 5. 자원 요구사항

| ID | 요구사항 | 한계값 | 근거 | 상태 |
| --- | --- | --- | --- | --- |
| RR-001 | compute-server 컨테이너 1개(= 채널 1개)의 자원 | CPU **0.75 core**, RSS **256 MB** | `setup_cctv.sh` compose 제한 | 구현 |
| RR-002 | 라즈베리파이 4(4GB) 1대 수용량 | 최대 **12채널**(CCTV 3대). 12 × 256M = 3GB, OS/dockerd 여유 ~700MB | 배포 매뉴얼 §엣지 수용량 | 구현 |
| RR-003 | 채널당 metadata 네트워크 대역 | ~**11.3 KB/s** [실측] | `performance/compute-server.md` | 구현 |
| RR-004 | 정상 상태 hot path 힙 할당 | **0 회/프레임** (집계기·위험판정·라우터·파이프라인) | Aggregator 지표 [실측 26,002회 → 1회] | 구현 |
| RR-005 | Source 링버퍼 | 기본 8, 상한 256, **drop-oldest** | `sourceRingCapacity` | 구현 |
| RR-006 | MQTT Sink 전송 큐 | 기본 8, 상한 **1000** (1000 × 256객체 × ~40B ≈ 10MB/Sink) | `kMaxMqttQueueSize` | 구현 |
| RR-007 | control 수신 큐 | 메시지 수 + **8 MB** 바이트 상한, drop-oldest | `kMaxQueuedPayloadBytes` | 구현 |
| RR-008 | 로그 큐 | 기본 10,000건, 상한 100,000건, 초과 시 drop-oldest | `logMaxPendingEntries` | 구현 |
| RR-009 | 메시지당 객체 수 상한 | **256개** | `kMaxObjectsPerMessage` | 구현 |
| RR-010 | JSON payload 상한 | 전역 1 MB, topview 수신 **64 KB**, 파싱 깊이 32 | `kMaxJsonPayloadBytes`, `kMaxTopViewPayloadBytes`, `kMaxJsonDepth` | 구현 |
| RR-011 | RTSP 버퍼 | 사용자 read 64 KB, 커널 `SO_RCVBUF` 1 MB, metadata frame 상한 1 MB (모두 상·하한 검사) | `rtspReadBufBytes` 외 | 구현 |
| RR-012 | 융합 객체당 provenance 채널 | 인라인 **8개**(힙 할당 0), 초과분은 `truncated` | `SourceChannelSet::kCapacity` | 구현 |
| RR-013 | 집계 버퍼 상주량 | `channelCount × (1 + poolSize)` 로 **상한 고정** (12채널 = 60버퍼) | `FrameBufferPool` | 구현 |
| RR-014 | 주차면 설정 상한 | 공간 4096개, 폴리곤 정점 16개 | `kMaxParkingSpaces`, `kMaxParkingVertices` | 구현 |
| RR-015 | 장시간 가동 시 RSS | 단조 증가하지 않아야 함 (해제 없는 버퍼 회전 구조라 단편화 요인 제거됨) | Aggregator 지표 §메모리 상주량 | 부분 |

---

## 6. 비기능 요구사항

| ID | 품질 속성 | 요구사항 | 검증 기준 | 상태 |
| --- | --- | --- | --- | --- |
| NFR-001 | 가용성 | 일시적 RTSP/MQTT 단절에서 자동 재접속하고 프로세스 장애 시 자동 재기동해야 한다. | broker/camera fault injection + `restart: always` 재기동 시험 통과 | 부분 |
| NFR-002 | 가용성 | control-server 는 현재 단일 인스턴스·단일 MQTT 연결·단일 UART 경로다. HA 가 필요하면 별도 설계가 있어야 한다. | 이중화 설계 문서 존재 | 제안 |
| NFR-003 | 메모리 안정성 | 외부 입력과 비동기 큐는 개수/바이트 상한을 가지며 drop-oldest 를 써야 한다. | 장시간 과부하에서 RSS bounded, OOM 없음 | 구현 |
| NFR-004 | 보안 | MQTT/RTSP 는 서버 인증서를 검증하는 TLS 를 기본값으로 써야 하고, secret 은 이미지/저장소에 포함하지 않아야 한다. | 잘못된 CA/SAN 연결 거부 + secret scan 통과 | 부분 |
| NFR-005 | 보안 | broker ACL 과 client 인증 정책은 저장소에서 확정되지 않았다 — 운영 전 확정해야 한다. | ACL 정의서와 현장 적용 증적 | 제안 |
| NFR-006 | 무결성 | MQTT JSON 은 schema/version/channel/유한성/크기를, UART 는 endian/size/checksum/필드를 검증해야 한다. | malformed/fuzz 입력이 crash 없이 drop | 구현 |
| NFR-007 | 무결성 | UART XOR 체크섬은 **전송 오류 검출용이지 메시지 인증이 아니다**. 물리 접근 통제가 전제다. | 배선 경로가 통제 구역 안에 있음 | 전제 |
| NFR-008 | 시간 | 모든 메시지 시각은 UTC epoch millisecond 이고 출처는 항상 CCTV `UtcTime` 이어야 한다. | 파이프라인 전 구간 `ts` 가 동일 값 | 구현 |
| NFR-009 | 시간 | 카메라 시각 동기(NTP/PTP) 편차를 감시해야 한다. 편차는 `ts` 를 통해 **지연 측정과 blur 매칭을 동시에** 망가뜨린다. | 편차 감시 지표 존재, out-of-order 처리 시험 | 제안 |
| NFR-010 | 안전 | 좌표·캘리브레이션·임계값이 구조적으로 잘못되면 조용한 오판보다 **fail-fast 또는 명시적 fault** 를 우선해야 한다. | invalid config 에서 기동 거부 또는 error 로그 | 구현 |
| NFR-011 | 개인정보 | 서버 간에는 원본 영상이 아니라 좌표 metadata 만 전달하고, client 는 blur 계약대로 민감 객체를 가려야 한다. | 패킷/로그 검사 + UI 인수 시험 | 구현 |
| NFR-012 | 개인정보 | blur 박스 축소는 설정 실수로도 불가능해야 한다. | `blurBoxScale` 하한이 1.0 으로 clamp | 구현 |
| NFR-013 | 관측성 | 연결·drop·재시도·윈도우·위험 판정·HW 하트비트를 5초 주기 지표 로그로 추적할 수 있어야 한다. | `metricsReportIntervalMs` 로그에서 원인 구분 가능 | 구현 |
| NFR-014 | 관측성 | 비동기 로깅은 비정상 종료 시 마지막 flush 주기(기본 500ms)를 유실한다 — 사후 분석 시 전제해야 한다. | 알려진 한계로 문서화 | 구현 |
| NFR-015 | 호환성 | wire 변경은 `kSchemaVersion` 을 올리고 compute/control/client/STM32 호환 행렬로 관리해야 한다. | 구·신 버전 조합 시험 및 rollback 가능 | 제안 |
| NFR-016 | 이식성 | 프로덕션 코드는 C++20/Linux 기준으로 빌드되고, UART 공유 계약은 **C11 과 C++20 양쪽**에서 컴파일되어야 한다. | 지원 compiler/architecture matrix 빌드 성공 | 부분 |
| NFR-017 | 시험성 | 핵심 알고리즘은 단위 시험, 경계는 통합 시험, concurrency 는 sanitizer 로 회귀 검증해야 한다. | CI 가 build/test/ASan/UBSan/TSan 통과 | 부분 |
| NFR-018 | 유지보수성 | 하드웨어 의존 계층은 인터페이스(`I*`)로 분리해 시험 대역(mock)으로 교체 가능해야 한다. | `IMetadataSource` / `IHwEventDispatcher` 등에 mock 구현 존재 | 구현 |

---

## 7. 설정 불변식 (위반 시 조용히 틀림)

> 아래는 **컴파일 오류도 런타임 오류도 로그도 없이** 시스템을 잘못 동작시키는 결합이다.
> 설정 리뷰 체크리스트로 쓸 것.

| ID | 불변식 | 위반 시 증상 | 근거 |
| --- | --- | --- | --- |
| INV-01 | `windowSizeMs > T_src` (현재 220 > 201.6) | 윈도우당 일부 채널만 잡히고, 어느 채널이 잡히는지가 주기적으로 순환 (100ms 에서 평균 2/4 채널, 약 12.5초 주기) | `AppConfig::windowSizeMs` |
| INV-02 | `trackMaxDistance ≥ v_max × max(windowSizeMs, T_src)` (4.0m / 0.22s = 18.2 m/s ≈ 65 km/h) | 그 속도를 넘는 객체가 매 프레임 새 gid 를 받아 UI 에서 사라졌다 생김 | `RiskConfig::trackMaxDistance` |
| INV-03 | `dedupMergeDistance < warningDistance` | 정상 Danger 케이스가 채널 간 dedup 병합으로 삼켜짐 | `RiskConfig` |
| INV-04 | `dangerousDistance ≤ warningDistance` | Warning 이 영영 도달 불가 — 생성자에서 fail-fast 하므로 **막혀 있음** | `ThresholdRiskPolicy` 생성자 |
| INV-05 | 호모그래피는 **CCTV 로컬 지면 평면**에서 캘리브레이션해야 함 | 도면 좌표로 캘리브레이션하면 control-server 가 한 번 더 회전시켜 예외·경고 없이 완전히 틀린 위치가 나옴 | `Contract.h::LocalPoint` |
| INV-06 | `channelId` 는 브로커 전역 유일 (`cctvId × 4 + 로컬채널`) | 채널이 겹쳐 서로의 프레임을 덮어씀 | 배포 매뉴얼 부록 A |
| INV-07 | `demoPedestrianProxy = false` (운영) | 실제 보행자가 전부 차량으로 판정되어 "사람 옆의 사람"이 경보를 울림 | `AppConfig::demoPedestrianProxy` |
| INV-08 | zone 개수 = `channelCount`, zone ID 유일, 캘리브레이션 채널 유일 | 기동 거부 — **막혀 있음** | `AppConfig::validate` |

---

## 8. 제약 및 수용 전제

- **위험 판정 범위**: "차량과 최근접 객체의 거리"에 한정된다. 속도, 궤적 충돌 예측, 주차면 점유율은
  현재 범위가 아니다. 주차 차량 억제(`StationaryParkingPolicy`)만 예외적으로 존재한다.
- **AI 정확도**: CCTV 가 객체를 검출하고 ONVIF metadata 를 제공한다. 서버는 오탐/미탐에 책임지지 않으며,
  `ContainmentSanitizer` 는 **중복 출력**만 정리할 뿐 검출 품질을 개선하지 않는다.
- **측정 환경**: `performance/` 의 RTSP/Source 수치는 **평문 RTSP** 기준이다. TLS 가 기본값이 된 이후
  재측정하지 않았으므로, TLS 운용 처리율은 재측정 전까지 단정하지 않는다.
- **Pi 실측 부재**: 집계기·위험판정·거리계산 지표는 x86(WSL2) 값이다. §4 의 "Pi4 예산" 열은 3~5배
  보정한 **추정 상한**이며, 실기 재측정으로 대체해야 한다.
- **승인 필요**: PR-001~PR-003 의 목표 지연, 오탐/미탐 허용률, 12채널 초과 확장, 가용성 SLA 는
  사업/안전 책임자의 승인이 필요하다.

---

## 9. 추적성 요약

| 요구사항군 | 주 구현 | 단위 시험 |
| --- | --- | --- |
| FR-101~104 | `RtspClientV2`, `RtspOnvifSourceV2` | `tests/compute-server/unit/NetworkTest.cpp`, `SourceTest.cpp` |
| FR-105~107 | `OnvifParser`, `ContainmentSanitizer` | `ParserTest.cpp`, `SanitizerTest.cpp` |
| FR-108~110 | `ParentBasedRouter` | `RouterTest.cpp` |
| FR-111~114 | `Pipeline`, `HomographyTransform`, `AffineImageCoordinateMapper` | `PipelineTest.cpp`, `MapperTest.cpp` |
| FR-501~504 | compute `AppConfig` | `AppConfigTest.cpp` |
| FR-201~205 | `MqttChannelReceiver`, `TimeWindowAggregatorV2` | `AggregatorTest.cpp` |
| FR-206~210 | `AffineLocalToWorldTransform`, `GridFuser`, `EuclideanMetric` | `TransformEquivalenceTest.cpp`, `FuserTest.cpp`, `MetricTest.cpp` |
| FR-211~212 | `StationaryParkingPolicy` | `ParkingPolicyTest.cpp` |
| FR-213~214 | `SpatialZoneMapper` | `ZoneMapperEquivalenceTest.cpp` |
| FR-215~217 | `ThresholdRiskPolicy` | `RiskTest.cpp` |
| FR-218~223 | `Controller` | `ControllerTest.cpp`, `PipelineTrajectoryTest.cpp` |
| FR-301~310 | `SerialHwEventDispatcher`, `driver/` | `DriverProtocolTest.cpp` |
| FR-401~413 | `client/` | `client/tests/*Check.cpp` |
