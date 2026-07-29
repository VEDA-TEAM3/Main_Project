# Wise AI 기반 주차장 디지털 트윈 관제 시스템

Qt 6와 GStreamer로 구현한 4채널 주차장 안전 관제 애플리케이션입니다. RTSP 영상, MQTT 장비 상태,
위험 객체 좌표와 블러 메타데이터를 하나의 대시보드에서 실시간으로 표시합니다.

## 핵심 기능

- GStreamer 기반 RTSP CCTV 4채널 수신 및 자동 재연결
- MQTT TLS 기반 장비 상태, 위험 객체, 블러 영역 수신
- 디지털 트윈 맵의 객체 위치, 이동 경로, 경고/위험 펄스 표시
- 얼굴과 차량 번호판 선택적 블러 처리
- 실시간 객체 목록과 이벤트 로그
- 채널별 장비 상태 및 CCTV 위험 테두리 표시
- 채널 신고 확인 및 완료 다이얼로그

## 구조

```text
Qt_Demo/
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
- 4채널 카메라 ID, 표시명, RTSP URL, 활성 여부
- GStreamer latency, timeout, queue, sink, 재연결 설정
- 블러 동기화 및 영상 필터 설정
- MQTT Broker, TLS 인증서, keep-alive, 재연결 설정
- MQTT 토픽 필터와 QoS
- 위험 및 블러 디스패처 주기

운영 자동화와 비밀 정보 주입을 위해 아래 환경 변수는 JSON보다 우선합니다.

| 환경 변수 | 설명 |
| --- | --- |
| `VEDA_CONFIG_FILE` | 사용할 JSON 설정 파일의 절대 경로 |
| `VEDA_RTSP_URL_1` ... `VEDA_RTSP_URL_4` | 채널별 RTSP URL |
| `VEDA_MQTT_HOST` | MQTT Broker 주소 |
| `VEDA_MQTT_PORT` | MQTT TLS 포트 |
| `VEDA_MQTT_CA_FILE` | CA 인증서 경로 |
| `VEDA_MQTT_CLIENT_ID` | 고정 MQTT Client ID |
| `VEDA_MQTT_DEBUG` | MQTT 수신 디버그 로그 활성화 |
| `QTCCTV_BLUR_SYNC_OFFSET_MS` | 영상과 블러 메타데이터 동기화 보정값 |
| `QTCCTV_DECODER_MODE` | `auto`, `software`, `d3d11` 디코더 선택 |

Windows Qt Creator에서는 **Projects > Run > Environment**에 환경 변수를 등록합니다.

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
