# Wise AI 기반 주차장 디지털 트윈 관제 시스템

Qt 6와 GStreamer로 구현한 다구역 주차장 안전 관제 애플리케이션입니다. 각 구역은 4개 RTSP 채널로
구성되며, MQTT 장비 상태, 위험 객체 좌표와 블러 메타데이터를 하나의 대시보드에서 실시간으로 표시합니다.

## 핵심 기능

- GStreamer 기반 구역별 RTSP CCTV 4채널 수신 및 자동 재연결
- 설정 파일 기반 구역·채널 확장과 구역 전환
- MQTT TLS 기반 장비 상태, 위험 객체, 블러 영역 수신
- 디지털 트윈 맵의 객체 위치, 이동 경로, 경고/위험 펄스 표시
- 얼굴과 차량 번호판 선택적 블러 처리
- 실시간 객체 목록과 이벤트 로그
- 채널별 장비 상태 및 CCTV 위험 테두리 표시
- 채널 신고 확인 및 Slack DM/채널 전송

## 구조

```text
client/
├─ assets/icons/          UI와 맵 아이콘
├─ config/                실행 설정 예제와 로컬 설정
├─ include/
│  ├─ config/             설정 모델과 로더
│  ├─ model/              객체, 이벤트, 장비 상태 모델
│  ├─ network/            MQTT 전송, 라우팅, 파서, 디스패처
│  ├─ overlays/           위험 및 장비 상태 오버레이
│  ├─ ui/                 화면, 패널, 다이얼로그
│  └─ video/              RTSP 수신과 블러 처리
├─ src/                   include와 동일한 계층의 구현 파일
├─ styles/                Qt Style Sheet
├─ CMakeLists.txt
└─ CMakePresets.json
```

영상 채널과 MQTT는 UI에서 분리되어 동작합니다. 채널별 `StreamReceiver`는 전용 스레드에서 실행되고,
MQTT 데이터는 라우터와 타입별 디스패처를 거쳐 queued signal로 UI와 영상 처리기에 전달됩니다.

## 요구 사항

- Windows 10/11 64-bit
- Qt 6.11.1 MinGW 64-bit: Widgets, Network, MQTT
- CMake 3.25 이상
- Ninja
- GStreamer 1.x MinGW x86_64 개발 및 런타임 패키지
- C++20 지원 컴파일러

기본 GStreamer 경로는 `C:/Program Files/gstreamer/1.0/mingw_x86_64`입니다. 다른 위치를 사용하면
CMake의 `GSTREAMER_ROOT` 값을 변경합니다.

## 빌드

```powershell
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

빠른 전체 빌드가 필요하면 Unity Build 프리셋을 사용할 수 있습니다.

```powershell
cmake --preset fast-debug-ninja
cmake --build --preset fast-debug-ninja
```

## 실행 설정

배포 환경과 성능 튜닝 값은 `config/app_config.json`에서 관리합니다. 이 파일에는 RTSP 인증 정보가
포함될 수 있으므로 Git에서 제외됩니다. 처음 실행할 때 다음 명령으로 예제 파일을 복사한 뒤 실제 값을
입력합니다.

```powershell
Copy-Item config/app_config.example.json config/app_config.json
```

CMake는 로컬 파일이 있으면 이를 빌드 디렉터리의 `config/app_config.json`으로 복사하고, 없으면 예제
파일을 복사합니다. 다른 위치의 설정을 사용하려면 `VEDA_CONFIG_FILE`에 절대 경로를 지정합니다.

```text
VEDA_CONFIG_FILE=C:\secure\veda\app_config.json
```

JSON에서 관리하는 주요 값은 다음과 같습니다.

- 창 크기
- 구역 구성과 구역별 4채널 카메라 ID, 표시명, RTSP URL, 활성 여부
- GStreamer 네트워크 latency, 의도적 영상 alignment delay, queue, sink, 재연결 설정
- 블러 동기화 및 영상 필터 설정
- MQTT Broker, TLS 인증서, keep-alive, 재연결 설정
- MQTT 토픽 필터와 QoS
- 위험 및 블러 디스패처 주기
- 로그 카테고리별 on/off (`logging`)
- 탑뷰 객체 아이콘 크기 (`digitalTwin.icons.vehiclePx`, `pedestrianPx`, 각 8~512, 기본 92/62)

### 구역 및 채널 확장

`video.areas`에 구역을 등록하고, 각 구역이 참조하는 스트림 4개를 `video.streams`에 추가합니다. 내부 전역
채널 인덱스는 다음 공식으로 계산합니다.

```text
globalChannelIndex = areaIndex * 4 + localChannelIndex
```

`areaIndex`와 `localChannelIndex`는 모두 0부터 시작합니다. 사용자 화면에는 각 구역마다 로컬 채널 번호
`CH 01`~`CH 04`를 표시하고, MQTT와 내부 모델은 전역 채널 인덱스를 사용합니다.

| 구역 | areaIndex | 전역 channelIndex | 화면 표시 |
| --- | ---: | ---: | --- |
| 제 1구역 | 0 | 0~3 | CH 01~CH 04 |
| 제 2구역 | 1 | 4~7 | CH 01~CH 04 |
| 제 3구역 | 2 | 8~11 | CH 01~CH 04 |

구역을 추가할 때는 다음 조건을 모두 지켜야 합니다.

- `areaId`와 `cameraId`는 전체 설정에서 중복되지 않아야 합니다.
- 각 `areas[].streamIds`에는 정확히 4개의 카메라 ID가 있어야 합니다.
- 전체 스트림 수는 `구역 수 * 4`여야 합니다.
- `channelIndex`는 0부터 `전체 스트림 수 - 1`까지 빠짐없이 연속이어야 합니다.
- 각 구역의 `streamIds` 순서는 해당 구역의 로컬 `CH 01`~`CH 04` 순서와 같아야 합니다.
- `initialAreaId`는 `areas`에 등록된 구역 ID여야 합니다.
- RTSP URL의 계정, 비밀번호와 호스트는 실제 장비 값으로 교체해야 합니다.

제 1구역에 `channelIndex` 0~3, 제 2구역에 4~7을 배치한 `app_config.example.json` 예시는 위 규칙을
충족하므로 그대로 사용할 수 있습니다. 테스트 환경에서는 두 구역이 같은 RTSP URL을 참조해도 되지만,
운영 환경에서는 구역별 실제 CCTV 주소를 지정합니다.

설정만 추가하면 다음 항목은 전체 채널 수에 맞춰 자동 확장됩니다.

- RTSP 스트림 세션과 구역별 CCTV 화면
- MQTT 장비 상태, 위험 `zoneId`, 블러 채널의 범위 검증과 라우팅
- 채널별 장비 상태 저장 및 선택 구역 표시
- 환경 변수 `VEDA_RTSP_URL_1`부터 `VEDA_RTSP_URL_N`까지의 URL 재정의

새 구역의 실제 지도 도면, 장치 아이콘 위치와 서버의 전역 `zoneId` 매핑은 설정만으로 생성되지 않습니다.
해당 자산과 배치는 별도로 추가하고, 서버도 같은 전역 채널 공식으로 값을 발행해야 합니다.

로그는 카테고리별로 켜고 끕니다. 전부 켜면 고빈도 항목(`mqttStatusPayload`, `mqttBlur`, `blurApply`)이
초당 수백 줄을 쏟아내 정작 봐야 할 줄이 묻히므로, 필요한 것만 `true`로 둡니다. 오류 로그는 어떤
설정으로도 꺼지지 않습니다.

| 키 | 로그 | 기본값 |
| --- | --- | --- |
| `mqttConnection` | `[MQTT]`, `[MQTT SUBSCRIBED]` 연결·구독 | `true` |
| `mqttStatusPayload` | `[MQTT RX]` 장비 상태 payload 원문 (매우 많음) | `false` |
| `mqttRisk` | `[MQTT RISK]` 위험 프레임 수신 요약 | `false` |
| `mqttBlur` | `[MQTT BLUR]` 채널별 블러 수신 (많음) | `false` |
| `blurApply` | `[BLUR APPLY]` 영상 프레임별 블러 적용 (많음) | `false` |
| `blurDispatch` | `[MQTT BLUR DISPATCH]` 블러 전달·병합 통계 | `false` |
| `riskDispatch` | `[TOPVIEW DBG] dispatch` 위험 전달·병합 통계 | `false` |
| `topview` | `[TOPVIEW]` 수신 요약, 이상치, 자동 경계 | `false` |
| `topviewDetail` | `[TOPVIEW DBG]` 객체별 프레임 좌표 상세 | `false` |
| `topviewDetailIntervalMs` | 위 상세 로그를 gid마다 이 주기로 제한 (`0`이면 매 프레임) | `1000` |

`topviewDetail`은 gid마다 `topviewDetailIntervalMs` 주기로만 남기되, **중앙값 필터나 속도 상한이 실제로
개입한 프레임은 주기와 무관하게 항상 남깁니다.** 평상시 분량을 20분의 1로 줄이면서 이상치는 하나도
놓치지 않습니다.

운영 자동화와 비밀 정보 주입을 위해 아래 환경 변수는 JSON보다 우선합니다.

| 환경 변수 | 설명 |
| --- | --- |
| `VEDA_CONFIG_FILE` | 사용할 JSON 설정 파일의 절대 경로 |
| `VEDA_RTSP_URL_1` ... `VEDA_RTSP_URL_N` | 설정된 전체 채널의 RTSP URL |
| `VEDA_MQTT_HOST` | MQTT Broker 주소 |
| `VEDA_MQTT_PORT` | MQTT TLS 포트 |
| `VEDA_MQTT_CA_FILE` | CA 인증서 경로 |
| `VEDA_MQTT_CLIENT_ID` | 고정 MQTT Client ID |
| `VEDA_MQTT_DEBUG` | MQTT 연결/구독 로그 활성화 (`logging.mqttConnection` 대체) |
| `VEDA_TOPVIEW_DEBUG` | 탑뷰 좌표 진단 로그 (`0`/`1`/`2`, `logging.topview*` 대체) |
| `VEDA_MAP_MIN_X`, `VEDA_MAP_MIN_Y` | 탑뷰 월드 좌표의 왼쪽·위쪽 경계 |
| `VEDA_MAP_MAX_X`, `VEDA_MAP_MAX_Y` | 탑뷰 월드 좌표의 오른쪽·아래쪽 경계 |
| `VEDA_MAP_INVERT_Y` | 월드 Y축을 화면 Y축으로 뒤집을지 여부 (`0`/`1`) |
| `QTCCTV_BLUR_SYNC_OFFSET_MS` | 영상과 블러 메타데이터 동기화 보정값 |
| `QTCCTV_DECODER_MODE` | `auto`, `software`, `d3d11` 디코더 선택 |
| `SLACK_BOT_TOKEN` | 신고 전송용 Slack Bot User OAuth Token |
| `SLACK_REPORT_TARGET` | 신고 대상 종류 (`dm` 또는 `channel`) |
| `SLACK_REPORT_USER_ID` | DM 수신 사용자 ID (`SLACK_REPORT_TARGET=dm`) |
| `SLACK_REPORT_CHANNEL_ID` | 수신 채널 ID (`SLACK_REPORT_TARGET=channel`) |

Windows Qt Creator에서는 **Projects > Run > Environment**에 환경 변수를 등록합니다.

`video.receiver.latencyMs`는 RTSP/RTP 네트워크 지터 흡수량이고, `video.receiver.alignmentDelayMs`는
AI/MQTT 처리 결과와 맞추기 위한 의도적 영상 표시 지연입니다. 서로 목적이 다르므로 독립적으로 조정합니다.
블러는 영상 프레임의 UTC와 MQTT `ts`를 비교해 가장 가까운 메타데이터를 적용합니다. TopView는 영상
프레임과 직접 동기화하지 않고 최신 `RiskFrame`을 수신 순서대로 반영하며, 객체 위치만
`digitalTwin.positionTransitionMs` 동안 부드럽게 전환합니다.

탑뷰 배치는 서버 `zoneId`가 어느 물리 CCTV 맵인지 정하고(`zoneId / 4`), 맵 안에서의 좌표는 그 구역의 월드
상자로 정규화합니다. 구역 상자는 `digitalTwin.world.zones`에 물리 CCTV 개수만큼(현재 2개) 적습니다.

```json
"zones": [
    { "minX": -58.0, "minY": -14.0, "maxX": -41.0, "maxY": 5.0 },
    { "minX": 42.0, "minY": -14.0, "maxX": 59.0, "maxY": 5.0 }
]
```

`zones`가 없으면 `minX..maxX`를 x로 반 갈라 두 구역에 나눠 쓰던 기존 동작으로 돌아갑니다. 두 구역이 도면에서
멀리 떨어져 있으면(예: 100m 간격의 15m짜리 구역 두 개) 반 가르기로는 표현할 수 없습니다. 창 두 개가 서로 붙어
있고 폭도 같아야 하므로, 한쪽을 맞추면 다른 쪽 객체가 전부 지도 끝에 겹칩니다. 상자가 실제 구역보다 크면 그
비율만큼 객체가 지도 가운데로 뭉쳐 보이므로, 현장 좌표 범위를 `VEDA_TOPVIEW_DEBUG=2` 로그로 확인해 맞춥니다.

## MQTT

토픽 이름과 QoS는 코드에 고정하지 않고 `app_config.json`의 `mqtt.topics`에서 설정합니다. 기본 예시는
다음 데이터를 구독합니다.

| 설정 키 | 기본 토픽 | 용도 |
| --- | --- | --- |
| `controllerStatus` | `veda/hw/ch/+/status` | 채널별 장비 피드백 |
| `centralStatus` | `veda/hw/status` | 중앙 장비 상태 |
| `sensorAlive` | `veda/ch/+/alive` | 채널 health/LWT |
| `centralEvent` | `veda/qt/event` | 중앙 이벤트 |
| `risk` | `veda/risk` | 위험 객체와 좌표 |
| `blur` | `veda/ch/+/blur` | 얼굴 및 번호판 블러 영역 |

Payload 계약과 채널 매핑은 [MQTT_INTEGRATION.md](MQTT_INTEGRATION.md)를 참고합니다.

## 보안

- 실제 RTSP 비밀번호가 든 `config/app_config.json`은 커밋하지 않습니다.
- 공개 저장소에는 `config/app_config.example.json`만 올립니다.
- TLS 인증서 검증을 비활성화하지 않습니다.
- 운영 환경에서는 RTSP URL과 인증서 경로를 환경 변수 또는 별도 보안 설정 파일로 주입합니다.

## 개발 원칙

- UI, 네트워크, 영상, 오버레이, 모델 계층을 분리합니다.
- GUI 객체는 GUI 스레드에서만 갱신합니다.
- 새 MQTT 데이터 종류는 `MqttTopicHandler` 구현을 추가해 확장합니다.
- 새 영상 수신 방식은 `StreamReceiver`와 `StreamReceiverFactory` 구현으로 확장합니다.
- `.clang-format`, `.clang-tidy`, `C_CppCodingConvention.md` 규칙을 따릅니다.
