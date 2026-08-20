# MQTT/TLS 연동 가이드

이 문서는 현재 Qt 클라이언트가 구독하는 MQTT 계약과 내부 전달 흐름을 설명합니다. 토픽과 QoS는 코드에
고정하지 않고 `config/app_config.json`의 `mqtt.topics`에서 관리합니다.

## 구독 토픽

| 설정 키 | 기본 토픽 | QoS | 용도 |
| --- | --- | ---: | --- |
| `controllerStatus` | `veda/hw/ch/+/status` | 1 | 채널별 카메라·장비 상태 스냅샷 |
| `centralStatus` | `veda/hw/status` | 1 | 중앙 장비 상태 및 처리 결과 |
| `sensorAlive` | `veda/ch/+/alive` | 1 | 채널 health/LWT |
| `centralEvent` | `veda/qt/event` | 1 | 중앙 위험 이벤트 |
| `risk` | `veda/risk` | 0 | 통합 위험 객체와 월드 좌표 |
| `blur` | `veda/ch/+/blur` | 0 | 채널별 얼굴·번호판 블러 영역 |

위험 좌표와 블러는 최신 값의 실시간성이 중요한 고빈도 데이터이므로 QoS 0을 사용합니다. 장비 상태와
이벤트는 전달 확인이 필요한 상태 데이터이므로 QoS 1을 사용합니다.

## 채널 번호

| 메시지 | Wire 값 | Qt 내부 인덱스 | UI |
| --- | ---: | ---: | --- |
| `veda/hw/ch/{ch}/status` | `ch` 0..3 | 0..3 | CH 01..04 |
| `veda/hw/status` | `channelId` 1..4 | 0..3 | CH 01..04 |
| `veda/qt/event` | `channelId` 1..4 | 0..3 | CH 01..04 |
| `veda/ch/{ch}/alive` | 토픽 `ch` 0..3 | 0..3 | CH 01..04 |
| `veda/ch/{ch}/blur` | 토픽과 payload `ch` 0..3 | 0..3 | CH 01..04 |

채널별 토픽은 wildcard 값과 payload 채널이 다르면 폐기합니다. 중앙 상태와 이벤트의 `channelId`는
1부터 시작하므로 0 기반 채널로 해석하면 안 됩니다. `veda/risk`는 이미 융합된 전역 프레임이므로 토픽에
채널 번호가 없습니다.

## 실행 흐름

1. `QtMqttTransport`가 TLS 연결과 구독을 담당합니다.
2. `MqttMessageRouter`가 토픽별 `MqttTopicHandler`로 payload를 전달합니다.
3. 각 handler는 JSON 계약을 검증하고 `DeviceStatusReport`, `CentralEventData`, `RiskFrameData`,
   `BlurFrameData`로 변환합니다.
4. 장비 상태와 이벤트는 서비스 계층을 거쳐 queued signal로 UI에 전달됩니다.
5. 고빈도 위험·블러 프레임은 타입별 디스패처가 짧은 구간 동안 최신 프레임으로 병합해 UI와 영상
   스레드에 전달합니다. 대기열이 무한히 증가하지 않으므로 MQTT 폭주가 영상 재생을 막지 않습니다.
6. 첫 유효 `RiskFrame`부터 GID 기반 증분 갱신으로 실데이터를 표시합니다.

## 장비 상태 계약

채널 상태 스냅샷은 0 기반 `ch`를 사용합니다.

```json
{
  "v": 1,
  "ts": 1785216139312,
  "ch": 1,
  "cameraAlive": true,
  "hardwareAlive": true,
  "ledGreen": true,
  "ledYellow": false,
  "ledRed": false,
  "sirenOn": false,
  "buzzerOn": false
}
```

이 형식에는 `detail`이 필요하지 않습니다. `hardwareAlive=false`인 프레임의 출력값은 확정 상태로
받아들이지 않고 마지막으로 확인된 상태를 유지합니다. 중앙 상태 및 이벤트는 별도 계약을 사용하며
`channelId`가 1 기반이라는 점에 주의합니다.

## 위험 프레임 계약

`veda/risk`는 공유 월드 좌표계의 통합 프레임입니다. 지도에 표시하는 클래스는 `Human`과 `Vehicle`이며,
다른 공유 계약 클래스는 해당 객체만 건너뛰고 프레임의 나머지 객체는 계속 처리합니다.

```json
{
  "v": 1,
  "ts": 1785216139312,
  "level": "Warning",
  "objects": [
    {
      "gid": 17,
      "cls": "Vehicle",
      "pos": { "x": 1.25, "y": -0.8 },
      "level": "Warning",
      "nearest": 23,
      "dist": 2.4
    }
  ]
}
```

`RiskFrame.ts`는 프레임 순번으로 사용하지 않습니다. 같은 `ts`라도 내용이 바뀔 수 있으므로 timestamp,
위험 상태와 객체 내용이 모두 같은 실제 중복만 제거하고 나머지는 도착 순서대로 반영합니다.

좌표는 미터 단위 월드 좌표입니다. 기본 설정은 `-5..5 m` 범위를 사용하며, 정확한 현장 표시에는
`digitalTwin.world` 또는 아래 환경 변수로 캘리브레이션 경계를 지정합니다.

```text
VEDA_MAP_MIN_X=<left bound>
VEDA_MAP_MIN_Y=<top bound>
VEDA_MAP_MAX_X=<right bound>
VEDA_MAP_MAX_Y=<bottom bound>
VEDA_MAP_INVERT_Y=1
```

고정 경계를 사용하지 않으면 초기 표본으로 범위를 추정하고, 범위를 벗어난 좌표가 여러 프레임 지속될
때만 경계를 확장합니다. 좌표는 3표본 중앙값과 8 m/s 이동 상한을 거친 뒤 화면 좌표로 정규화됩니다.
채널은 지도 중심을 지나는 45도 대각선 기준 CH 01(위), CH 02(왼쪽), CH 03(아래), CH 04(오른쪽)으로
계산하며 경계 흔들림을 줄이기 위한 히스테리시스를 적용합니다.

## 블러 프레임 계약

`veda/ch/{ch}/blur`의 토픽 채널과 payload `ch`는 모두 0 기반이며 서로 같아야 합니다. 지원 클래스는
`Head`와 `LicensePlate`입니다.

```json
{
  "v": 1,
  "ts": 1785216139312,
  "ch": 1,
  "blurs": [
    { "id": 8506, "cls": "Head", "box": { "l": 0.28, "t": 0.66, "r": 0.31, "b": 0.72 } },
    { "id": 8507, "cls": "LicensePlate", "box": { "l": 0.48, "t": 0.70, "r": 0.57, "b": 0.75 } }
  ]
}
```

박스 좌표는 0..1 정규화 값입니다. 빈 `blurs` 배열도 유효한 프레임이며 이전 블러가 계속 남지 않도록
반드시 전달해야 합니다. 영상 프레임 UTC는 RTCP reference timestamp를 우선 사용하고, 없으면 PTS와
초기 UTC anchor로 계산합니다. 클라이언트는 MQTT `ts`와 가장 가까운 블러 프레임을 찾아 적용하고 짧은
메타데이터 공백에는 마지막 프레임을 제한 시간 동안 유지합니다. 다음 프레임이 아직 도착하지 않은
구간에서는 같은 `id`의 직전 두 프레임 이동량으로 위치를 예측해 덮습니다
(`video.receiver.blur.maxExtrapolationMs`).

`QTCCTV_BLUR_SYNC_OFFSET_MS`는 카메라 영상과 메타데이터의 실제 시간차를 보정합니다. 이 값은
`video.receiver.latencyMs`나 `alignmentDelayMs`를 대신하지 않으며 현장 측정값으로 조정해야 합니다.

## 환경 변수

```text
VEDA_MQTT_HOST=<broker host>
VEDA_MQTT_PORT=8883
VEDA_MQTT_CA_FILE=<CA certificate path>
VEDA_MQTT_CLIENT_ID=<optional unique client id>
VEDA_MQTT_DEBUG=1
```

`VEDA_MQTT_DEBUG`는 연결·구독 로그를 켭니다. 좌표 진단은 `VEDA_TOPVIEW_DEBUG=1`로 1초 요약,
`VEDA_TOPVIEW_DEBUG=2`로 객체별 상세까지 활성화합니다. 고빈도 로그를 동시에 켜면 로그 출력 자체가
MQTT와 UI 스레드에 부담을 줄 수 있으므로 필요한 카테고리만 사용합니다.

## Slack 신고

Slack 신고는 MQTT와 독립된 HTTPS 경로입니다. 비밀값을 JSON이나 저장소에 넣지 않고 실행 환경으로
주입합니다.

```text
SLACK_BOT_TOKEN=xoxb-...
SLACK_REPORT_TARGET=dm
SLACK_REPORT_USER_ID=U...
```

채널로 전송할 때는 `SLACK_REPORT_TARGET=channel`과 `SLACK_REPORT_CHANNEL_ID=C...`를 사용합니다.

## 빌드

```powershell
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

Windows 실행 전 `VEDA_MQTT_CA_FILE`이 실제 CA 인증서를 가리키는지 확인합니다.
