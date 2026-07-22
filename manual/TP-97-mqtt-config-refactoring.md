# TP-97 MQTT 설정 및 Sink 리팩터링

## 변경 목적

- `TopViewFrame`을 MQTT로 Control Server에 전달한다.
- `BlurFrame`의 Bounding Box를 MQTT로 Qt에 전달한다.
- Console Sink는 출력 전용 Stub으로 유지한다.
- 브로커 주소와 채널 수는 `config.json`에서 주입한다.
- 서로 다른 프레임이 같은 발행 큐를 점유하지 않도록 MQTT Sink를 역할별로 분리한다.

## 데이터 흐름

```text
Compute Pipeline
  ├─ TopViewFrame → MqttTopViewSink → veda/ch/{ch}/topview → Control Server
  └─ BlurFrame    → MqttBlurSink    → veda/ch/{ch}/blur    → Qt

Control Server
  └─ RiskFrame → MqttTransport → veda/risk → Qt
```

## 주요 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `compute-server/src/sink/MqttTopViewSink.{h,cpp}` | TopView 전용 검증, 큐, JSON 직렬화 및 MQTT 발행 |
| `compute-server/src/sink/MqttBlurSink.{h,cpp}` | Blur 전용 검증, 큐, JSON 직렬화 및 MQTT 발행 |
| `compute-server/src/sink/MqttSink.{h,cpp}` | 두 프레임을 함께 처리하던 통합 Sink 삭제 |
| `compute-server/src/core/AppContext.cpp` | 두 MQTT Sink를 각각 Pipeline에 주입 |
| `compute-server/src/sink/ConsoleSink.h` | MQTT 의존성 없는 콘솔 Stub 유지 |
| `compute-server/config.json` | MQTT 접속 정보와 `channelCount` 제공 |
| `control-server/config.json` | `channelCount` 제공 |
| 양쪽 `AppConfig.h` | `channelCount` 기본값을 `0`으로 두고 JSON에서 로드 |

## Sink별 발행 계약

| Sink | Frame | Topic | QoS | 수신자 |
|---|---|---|---|---|
| `MqttTopViewSink` | `TopViewFrame` | `veda::topic::topView(frame.ch)` | `veda::qos::kTopView` | Control Server |
| `MqttBlurSink` | `BlurFrame` | `veda::topic::blur(frame.ch)` | `veda::qos::kBlur` | Qt |

두 발행 모두 JSON payload에 `veda::encode(frame)`을 사용하고 retain은 `false`다.

## AppContext 조립

```cpp
auto riskSink = std::make_shared<MqttTopViewSink>(std::move(topViewConfig));
auto blurSink = std::make_shared<MqttBlurSink>(std::move(blurConfig));

pipeline_ = std::make_unique<Pipeline>(
    parser, imageMapper, sanitizer, router,
    ground, transform, riskSink, blurSink);
```

두 Sink가 별도 Mosquitto client를 사용하므로 Client ID도 구분한다.

```text
{mqttClientId}-topview
{mqttClientId}-blur
```

동일한 Client ID로 두 연결을 만들 때 발생할 수 있는 상호 연결 해제를 방지한다.

## 프레임 검증

공통 검증:

- 스키마 버전 일치
- 양수 타임스탬프
- `0 <= frame.ch < channelCount`

TopView 추가 검증:

- 위험 분석 대상 클래스
- 유한한 월드 좌표 `x`, `y`

Blur 추가 검증:

- Blur 대상 클래스
- Bounding Box 좌표가 유한하며 `[0, 1]` 범위
- `left <= right`, `top <= bottom`
- 빈 BlurFrame도 이전 Blur 영역 제거를 위해 정상 처리

## 큐 정책

각 Sink는 독립된 bounded queue와 worker thread를 가진다. 큐가 가득 차면 가장 오래된 프레임을 제거해 최신 프레임을 우선한다. TopView와 Blur가 서로의 큐 공간을 소비하지 않는다.

## 검증 결과

- CMake 재구성 성공
- compute-server 빌드 성공
- control-server 빌드 성공
- 기존 통합 `MqttSink` 실행 참조 제거
