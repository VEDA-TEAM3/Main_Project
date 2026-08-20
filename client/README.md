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

구역은 **최대 6개**입니다. 지도 도면의 커버리지 격자가 3열 x 2행이라 그보다 많은 구역은 그릴 자리가
없습니다. 더 늘리려면 `DigitalTwinMapSceneBuilder`의 격자부터 다시 설계해야 합니다.

#### 앱에서 추가·수정하기 (권장)

설정 팝업의 **구역 관리** 탭에서 왼쪽 목록의 줄을 고르면 오른쪽 폼이 그 줄을 다룹니다.

- **실행 중인 구역**을 고르면 그 구역의 이름·계정·채널 주소가 폼에 실리고, 화면도 그 구역으로
  전환됩니다. 값을 고치고 `변경 저장`을 누르면 `video.areas[].name`과 그 구역 채널의 `url`만
  바뀝니다(구역 개수·채널 번호·구역 상자는 그대로).
- **빈 자리**를 고르면 새 구역 입력이 됩니다. `구역 추가`를 누르면 `video.areas`,
  `video.streams`, `digitalTwin.world.zones`가 함께 늘어납니다.

두 경우 모두 나머지 설정값은 손대지 않습니다. 계정과 비밀번호는 주소와 따로 입력하고 네 채널에
함께 적용되며, 채널마다 계정이 다른 구역은 이 화면에서 고칠 수 없습니다(설정 파일을 직접 고치세요).

- 새 구역의 월드 상자는 **기존 구역들의 간격과 크기를 그대로 이어 붙여** 자동 생성됩니다. 실제 도면
  좌표에 맞추려면 저장 후 `digitalTwin.world.zones` 마지막 항목을 아래 유도 규칙대로 보정하세요.
- 영상 수신기·MQTT 구독·지도 도면은 모두 시작할 때 한 번 구성되므로 **프로그램을 다시 시작해야**
  반영됩니다. 팝업의 구역 목록은 아직 반영되지 않은 구역을 `재시작 후 적용`으로 표시합니다.
- 서버가 새 채널(`veda/hw/ch/<n>/status`, `veda/ch/<n>/blur`, risk `zoneId`)을 실제로 발행해야
  지도에 객체가 그려집니다. 구역 추가는 클라이언트의 **수용 범위**만 넓힙니다.

#### 설정 파일을 직접 고치기

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
- `digitalTwin.world.zones`를 적었다면 상자 개수가 구역 수와 같아야 합니다. 키를 아예 빼면 `bounds`를
  구역 수만큼 x로 균등하게 갈라 씁니다(도면 좌표가 맞지 않으므로 임시 확인용입니다).

제 1구역에 `channelIndex` 0~3, 제 2구역에 4~7을 배치한 `app_config.example.json` 예시는 위 규칙을
충족하므로 그대로 사용할 수 있습니다. 테스트 환경에서는 두 구역이 같은 RTSP URL을 참조해도 되지만,
운영 환경에서는 구역별 실제 CCTV 주소를 지정합니다.

설정만 추가하면 다음 항목은 전체 채널 수에 맞춰 자동 확장됩니다.

- RTSP 스트림 세션과 구역별 CCTV 화면
- MQTT 장비 상태, 위험 `zoneId`, 블러 채널의 범위 검증과 라우팅
- 채널별 장비 상태 저장 및 선택 구역 표시
- 환경 변수 `VEDA_RTSP_URL_1`부터 `VEDA_RTSP_URL_N`까지의 URL 재정의
- 탑뷰 도면의 구역 활성화. 커버리지 격자 6칸 중 앞에서부터 구역 수만큼이 활성 구역(네온 테두리, 채널
  사분면, 장치 상태 칩)이 되고 나머지는 `구역 N · 확장 예정`으로 남습니다
- 구역별 채널 위험 오버레이와 장치 상태 아이콘

서버는 같은 전역 채널 공식으로 값을 발행해야 하고, 새 구역의 월드 상자는 실제 도면 좌표에 맞춰
보정해야 합니다.

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

`video.receiver.processingWidth`/`processingHeight`는 **시스템 메모리로 내려받기 전에 GPU에서 줄일 해상도**입니다
(`d3d11` 디코더 경로에서만 적용, 둘 다 `0`이면 원본 유지). 블러와 전처리가 CPU에서 돌기 때문에 프레임은
`d3d11download`로 한 번 내려왔다가 sink에서 다시 올라가는데, 여기서 줄이면 **전송량과 CPU 픽셀 수가 함께**
줄어듭니다(1080p→720p 기준 약 55%). 2x2 그리드 타일이 화면상 약 430x240이라 720p로도 표시 해상도의 3배이며,
채널을 확대했을 때만 약간 부드러워집니다. 너비와 높이를 모두 지정해도 `d3d11scale`이 pixel-aspect-ratio로
화면비를 보정하므로 4:3 카메라도 왜곡되지 않습니다.

`alignmentDelayMs`는 큐의 `min-threshold-time`으로 구현한 **고정 지연선**입니다. 임계값 아래로 내려가면
출력이 멈추므로 쌓인 분량을 지터 흡수에 쓸 수 없습니다. 지터를 흡수하고 프레임을 버리는 지점은
`renderQueueMaximumBuffers` 하나뿐입니다. 싱크가 `sinkSync: false`로 도착 즉시 렌더하므로 평시에는 이 큐가
비어 있어 깊이를 늘려도 지연이 늘지 않고, 블러나 GPU 업로드가 한 프레임 늦어질 때만 채워집니다. 1로 두면
흡수량이 0이라 짧은 지연도 곧바로 드롭이 되므로 8을 기본값으로 씁니다(720p NV12 기준 프레임당 약 1.4 MB).

탑뷰 배치는 서버 `zoneId`가 어느 물리 CCTV 맵인지 정하고(`zoneId / 4`), 맵 안에서의 좌표는 그 구역의 월드
상자로 정규화합니다. 구역 상자는 `digitalTwin.world.zones`에 `video.areas` 개수와 똑같이 적습니다(최대 6개).

### 월드 좌표는 도면 전체 하나를 기준으로 유도합니다

도면(scene) 1000 x 520 단위가 주차장 한 층 전체입니다. **축척과 원점 두 개만 정하면 나머지 값은 전부
계산으로 나옵니다.** 구역 상자를 각자 정하지 마세요.

| 기준 | 값 | 근거 |
| --- | --- | --- |
| 축척 `S` | **10 / 158 = 0.0632911 m/도면 단위** | 실측: 채널 하나가 5 m → 구역 한 변(도면 158단위) = 10 m |
| 원점 | 도면 `(162, 230)` = world `(0, 0)` | 구역 1 객체 영역의 좌하단 |

```text
world_x = (scene_x - 162) * 10.0 / 158.0
world_y = (230 - scene_y) * 10.0 / 158.0      // world y는 위쪽이 +
```

여기서 유도된 값이 그대로 설정값입니다.

| 도면 영역 | scene | world |
| --- | --- | --- |
| 도면 전체 (`world.bounds`) | (0, 0) ~ (1000, 520) | x -10.253..53.038, y -18.354..14.557 (63.29 x 32.91 m) |
| 구역 셀 | 194 x 202 | 12.28 x 12.78 m |
| **객체 영역** (`zones`와 1:1) | **158 x 158** | **10 x 10 m** (채널당 5 m) |
| 구역 1 객체 영역 | (162, 72) ~ (320, 230) | **x 0..10, y 0..10** |
| 구역 2 객체 영역 | (364, 72) ~ (522, 230) | **x 12.785..22.785, y 0..10** |

```json
"zones": [
    { "minX": 0.0,    "minY": 0.0, "maxX": 10.0,   "maxY": 10.0 },
    { "minX": 12.785, "minY": 0.0, "maxX": 22.785, "maxY": 10.0 }
]
```

**객체 영역이 정사각형이므로 월드 상자도 정사각형입니다.** 비율이 1이 아니면 가로·세로 배율이 달라져
실제로 정사각형인 구역이 화면에서 찌그러집니다.

축척이 바뀌면 **`S` 하나만** 고치고 위 표를 다시 계산하면 됩니다. 두 구역 사이 거리(12.785 m)는 도면상
간격을 축척으로 환산한 값입니다. 실제 두 구역이 더 떨어져 있다면 구역 2의 `minX`/`maxX`만 실측값으로
바꾸면 됩니다(크기 10 x 10은 유지).

> ⚠️ **이 축척에서 도면의 주차 구획은 실제 크기가 아닙니다**(도면 16 x 36단위 = 1.01 x 2.28 m).
> 도면은 카메라 커버리지 배치를 보여 주는 그림이지 실측 도면이 아닙니다. 구획 치수를 도면에서 재서
> 쓰지 마세요.

탑뷰 도면은 주차장 한 층 전체를 그리고, 그 위에 물리 CCTV 커버리지를 **3열 x 2행 격자**로 얹습니다
(`DemoParkingMapSceneBuilder`). 열은 기둥 그리드, 행은 주 주행 통로에 맞춰 카메라가 통로 위에 오도록
배치했습니다. 앞의 두 칸만 실제 CCTV가 있고 나머지 여섯 칸은 `확장 예정`으로 비워 둡니다. 활성 구역
안의 대각선 두 개가 채널 경계(상/우/하/좌 = CH01~CH04)입니다.

CCTV를 늘릴 때는 세 곳의 개수를 같이 맞춥니다.

1. `DigitalTwinMapSceneLayout::zoneRects`의 배열 크기 (커버리지 격자에서 앞에서부터 활성화됩니다)
2. `app_config.json`의 `video.areas`와 `video.streams` (구역당 채널 4개)
3. `digitalTwin.world.zones` (구역별 월드 상자)

`zones`가 없으면 `minX..maxX`를 x로 반 갈라 두 구역에 나눠 쓰던 기존 동작으로 돌아갑니다. 도면 전체를 기준으로
잡으면 두 구역이 도면 한쪽에 몰릴 수 있으므로 `zones`를 명시하는 쪽을 씁니다. 상자가 실제 구역보다 크면 그
비율만큼 객체가 지도 가운데로 뭉쳐 보이므로, 현장 좌표 범위를 `VEDA_TOPVIEW_DEBUG=2` 로그로 확인해 맞춥니다.

`zoneId`를 못 받은 객체는 x 좌표로 구역을 추정하는데, 기준은 `world.bounds`의 중심이 아니라 **두 구역 상자
사이의 중간**입니다(`DigitalTwinWorldConfig::zoneSplitX()`). 도면 기준 좌표계에서는 두 구역이 원점 한쪽에
몰릴 수 있어 bounds 중심을 쓰면 경계가 엉뚱한 곳에 생깁니다.

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

### 관제 계정 (로그인)

프로그램을 켜면 로그인 화면이 대시보드를 덮고, **인증 전에는 RTSP 수신도 MQTT 접속도 시작하지
않습니다.** 계정은 설정 파일과 같은 폴더의 `config/users.json`에 저장되어 폴더를 옮기면 함께 갑니다
(gitignore 대상).

- **최초 실행**에는 계정이 없으므로 로그인 대신 **관리자 계정 생성** 화면이 뜹니다. 기본 계정을 심어
  배포하지 않습니다 — 현장에서 끝내 안 바뀝니다. 계정이 하나라도 생기면 이 경로는 닫힙니다.
- **비밀번호는 PBKDF2-HMAC-SHA256**(사용자별 salt, 반복 횟수는 레코드마다 저장)로 저장합니다.
  반복 횟수를 나중에 올려도 기존 계정이 그대로 열립니다. 최소 8자를 강제합니다 — 계정 파일이
  프로그램과 함께 옮겨 다니므로 파일이 남의 손에 들어가는 것을 전제로 잡아야 합니다.
- **없는 아이디에도 PBKDF2를 한 번 돌립니다.** 곧바로 실패로 빠지면 응답이 마이크로초 만에 돌아오고
  실제 계정은 수백 ms가 걸려서, 그 차이만으로 어떤 아이디가 있는지 훑을 수 있습니다(CWE-208).
  실패 메시지도 "없는 아이디"와 "틀린 비밀번호"가 같습니다.
- 연속 실패에는 지수 백오프(3회부터, 최대 30초)가 걸리고, 계정 파일은 소유자만 읽도록 권한을 좁힙니다.
- **역할은 `admin`과 `operator` 둘**입니다. 설정 팝업의 구역 관리 탭이 RTSP 계정과 비밀번호를 보여주고
  고칠 수 있으므로, `operator`에게는 상단 설정 버튼을 감추고 `openMapSettingsDialog()`에서 한 번 더 막습니다.
- **유휴 자동 잠금은 넣지 않았습니다.** 상황 발생 중에 관제 화면이 잠기면 안 됩니다.
- **로그인 창을 닫는 것은 "로그인 취소"가 아니라 "프로그램 종료"입니다.** Alt+F4로 로그인 창만 닫으면
  뒤의 대시보드가 그대로 드러나 관문을 통째로 건너뛰므로, 인증 전 닫기는 앱을 끝냅니다.
  로그인 화면에서도 Alt+Enter 전체 화면 전환과 창 이동·크기 변경이 그대로 되고, 로그인 창이 따라갑니다.
- 배포 준비용으로 `--create-user <이름> <비밀번호> <admin|operator>`가 있습니다.
  **비밀번호가 명령줄에 남아 프로세스 목록에서 보이므로 운영 중에는 쓰지 마세요.**

**이 로그인은 보안 경계가 아닙니다.** 포터블 실행이라 폴더를 복사해 간 사람은 계정 파일을 지우고
새 관리자를 만들면 그만이고, 파일을 암호화해도 키가 같이 다니므로 소용없습니다(같은 이유로 DPAPI 계열은
쓸 수 없습니다). 값어치는 **콘솔 앞 무단 조작 방지, 권한 분리, 행위 귀속**입니다. 비밀번호를 해싱하는
이유도 앱이 아니라 **사용자를 지키기 위해서**입니다 — 사람들은 비밀번호를 재사용합니다.

### RTSP 자격증명

`video.streams[].url`에는 계정과 비밀번호가 **평문으로** 들어갑니다. 지금 구조에서 남는 위험과 지켜야 할
운영 조건은 다음과 같습니다.

- **저장 매체를 신뢰 경계로 봅니다.** 설정 파일을 읽을 수 있는 사람은 카메라 계정을 얻습니다. 관제 PC는
  디스크 암호화(BitLocker)를 켜고, `app_config.json`은 관제 계정만 읽도록 ACL을 제한합니다. 설정 팝업의
  구역 추가는 저장 후 원본 파일 권한을 복원하지만, **처음 권한은 배포할 때 직접 걸어야 합니다.**
- **카메라 계정은 읽기 전용 최소 권한**으로 만듭니다. 스트림 조회만 되고 PTZ 제어나 장치 설정 변경은
  막습니다. 유출되더라도 피해가 영상 열람에 그칩니다.
- **가능하면 `rtsps://`를 씁니다.** `rtsp://`는 제어 채널이 평문이라 서버가 Basic 인증을 제시하면
  비밀번호가 base64로 그대로 나가고, RTP 페이로드도 암호화되지 않습니다. 설정 로더와 수신기는 `rtsps`를
  이미 받아들이므로 카메라/NVR 지원 여부만 확인하면 됩니다.
- **로그에 URL을 남기지 않습니다.** 현재 코드에는 RTSP URL을 출력하는 로그가 없고 Slack 신고 페이로드에도
  들어가지 않습니다. 새 로그를 추가할 때 `cameraId`나 채널 번호만 쓰고 URL은 넣지 마세요.
  운영 환경에서는 `GST_DEBUG`도 꺼 둡니다 — GStreamer가 요청 URL을 자체 로그에 찍습니다.
- **환경 변수 주입은 편의 수단입니다.** `VEDA_RTSP_URL_N`은 같은 사용자의 다른 프로세스와 크래시 덤프에서
  읽힐 수 있습니다. CI나 일회성 점검용으로만 쓰고 상시 운영에는 쓰지 않습니다.
- 설정 팝업에서 구역을 추가·수정할 때는 **계정과 비밀번호를 주소와 따로** 입력합니다. 비밀번호 칸은
  가려져 있어 저장된 값을 불러와도 화면에 드러나지 않고, `@`나 `:`가 든 비밀번호도 percent-encoding되어
  URL 파싱이 깨지지 않습니다. 주소에 계정을 직접 적으면 그 값이 우선하지만, 그 경우 비밀번호가 화면에
  그대로 보입니다.

### MQTT 입력 신뢰 경계

브로커에 발행 ACL이 없으면 **어떤 발행자든** 지도, 장비 패널, 영상 블러에 직접 입력을 넣습니다. TLS는
브로커를 위조·도청에서 지키지만 발행자가 진짜인지는 보장하지 않습니다. 코드 쪽 방어는
[`MqttPayloadLimits.h`](include/network/parsing/MqttPayloadLimits.h) 한곳에 모여 있습니다.

- **payload 256KB, RiskFrame 객체 256개 상한.** 초과분은 잘라 쓰지 않고 메시지 전체를 버립니다 —
  잘라 쓰면 안전 판단에서 어떤 객체가 빠졌는지 모른 채로 화면이 돕니다. 블러 영역만 64개로 잘라 쓰는데,
  그쪽은 일부라도 가리는 편이 아무것도 안 가리는 것보다 낫기 때문입니다.
- **RiskFrame의 `ts`는 로컬 UTC 기준 -60초 ~ +5초 창 안이어야 합니다.** 창을 벗어난 `ts`는 자동 경계
  warmup을 앞당기고 진단 통계를 망가뜨립니다.
- **블러 metadata의 `ts`에는 이 검사를 걸지 않습니다.** 걸어 봤더니 관제 PC와 서버의 시계가 조금만
  어긋나도 정상 metadata가 통째로 거부돼 **블러가 꺼진 채 얼굴과 번호판이 그대로 나갔습니다.**
  검사가 막으려던 것(미래 `ts` 하나가 `BlurProcessor`의 최신 timestamp를 끌어올려 이후 metadata를
  전부 과거로 버리는 것)보다 검사 자체가 더 자주 블러를 껐습니다. 다시 넣는다면 로컬 시계가 아니라
  **수신한 `ts`들의 흐름을 기준으로** 튀는 값만 걸러야 합니다.
- **월드 좌표는 현재 정규화 경계를 크게 벗어나면 버립니다.** 유한하지만 비정상인 좌표(`x=1e9` 등)는
  NaN 검사를 통과하고, 처음 보는 gid는 중앙값 필터와 속도 상한도 우회합니다. 그 좌표가 자동 경계 확장에
  들어가면 경계가 한 번 벌어진 뒤 되돌아오지 않아 정상 객체들이 지도 한 점에 뭉칩니다. 경계로 clamp하지
  않고 버립니다 — clamp하면 잘못된 위치가 정상처럼 보입니다.
  **자동 경계 warmup 구간에는 비교할 기준이 없으므로, 고정 경계 설정이 유일한 방어입니다.**
- **프로토콜 오류는 초당 하나만 올립니다.** 잘못된 메시지 하나마다 로그 한 줄과 스레드 경계를 넘는 signal이
  하나씩 나가므로, 제한이 없으면 작고 잘못된 payload를 쏟아붓는 것만으로 크기 상한에 걸리기 한참 전에 UI가
  밀립니다. 억제된 개수는 다음 오류 메시지에 `(+N suppressed)`로 붙습니다.

배포에서 확인해야 하는 것(코드로 막을 수 없음):

- **브로커 ACL** — 익명 발행 금지, 이 클라이언트는 subscribe 전용, `veda/risk`는 control-server만,
  HW 토픽은 지정된 HW 프로세스만 발행.
- **`app_config.json` 쓰기 권한** — 이 파일이 `mqtt.host`와 `caCertificatePath`를 정합니다. 파일을 고칠 수
  있는 사람은 클라이언트를 자기 브로커로 돌리고 자기 CA를 지정할 수 있고, 그러면 `VerifyPeer`는 **통과합니다.**
  관리자만 쓸 수 있는 위치에 설치하세요.
- **실행 위치와 PATH** — GStreamer와 Qt DLL을 PATH에서 찾으므로, 쓰기 가능한 디렉터리에서 실행하거나
  사용자가 통제하는 PATH 항목이 있으면 임의 DLL이 로드됩니다.

## 개발 원칙

- UI, 네트워크, 영상, 오버레이, 모델 계층을 분리합니다.
- GUI 객체는 GUI 스레드에서만 갱신합니다.
- 새 MQTT 데이터 종류는 `MqttTopicHandler` 구현을 추가해 확장합니다.
- 새 영상 수신 방식은 `StreamReceiver`와 `StreamReceiverFactory` 구현으로 확장합니다.
- `.clang-format`, `.clang-tidy`, `C_CppCodingConvention.md` 규칙을 따릅니다.
