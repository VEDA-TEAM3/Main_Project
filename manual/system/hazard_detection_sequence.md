# 위험 감지 시퀀스 다이어그램

## 1. 정상 Danger 감지부터 Qt·STM32 반영까지

```mermaid
sequenceDiagram
    autonumber
    participant CCTV as CCTV / ONVIF
    participant RTSP as RtspClientV2
    participant Source as RtspOnvifSourceV2
    participant Edge as compute Pipeline
    participant CMQ as compute MQTT Sink
    participant Broker as MQTT Broker
    participant Receiver as MqttChannelReceiver
    participant Agg as TimeWindowAggregatorV2
    participant Core as Transform + Fuser + Zone + Risk
    participant UART as SerialHwEventDispatcher
    participant STM as STM32 경보장치
    participant Sink as control MQTT Sink
    participant Qt as Qt 디지털 트윈
    participant Operator as 관제 운영자

    CCTV->>RTSP: RTSP interleaved RTP metadata
    RTSP->>Source: 재조립 ONVIF payload callback
    Source->>Edge: next(RawPacket)
    Edge->>Edge: parse → sanitize → route

    par 위험 객체 경로
        Edge->>Edge: bottom-center → homography(LocalPoint)
        Edge->>CMQ: TopViewFrame(ch, ts, objects)
        CMQ->>Broker: veda/ch/{ch}/topview, QoS 0
        Broker->>Receiver: TopViewFrame
    and 개인정보 경로
        Edge->>Edge: blur bbox map + scale
        Edge->>CMQ: BlurFrame(ch, ts, blurs)
        CMQ->>Broker: veda/ch/{ch}/blur, QoS 0
        Broker->>Qt: BlurFrame
        Qt->>Qt: 영상 ts와 매칭해 overlay 적용
    end

    Receiver->>Receiver: topic/schema/channel/size 검증
    Receiver->>Agg: push(TopViewFrame)
    Note over Agg: 설정된 시간 윈도우로 채널 프레임 집계
    Agg->>Core: aggregated frames
    Core->>Core: Local→World 변환
    Core->>Core: 교차 채널 dedup + GlobalId 추적
    Core->>Core: zone 배정 + hysteresis
    Core->>Core: 차량 기준 최근접 거리 계산

    alt distance <= dangerousDistance
        Core->>Core: zone= Danger, frame.level= Danger
        Core->>UART: RiskEvaluation(zoneLevels)
        UART->>STM: 27-byte downlink frame
        STM->>STM: LED/경광등/부저 정책 적용
        STM-->>UART: 19-byte ACK/heartbeat + indicator state
        UART-->>Core: hardware status callback
        Core->>Sink: ChannelStatus
        Sink->>Broker: retained hw status, QoS 1
        Broker->>Qt: ChannelStatus
        Core->>Sink: WorldFrame(level=Danger)
        Sink->>Broker: veda/risk RiskFrame, QoS 1
        Broker->>Qt: RiskFrame
        Qt->>Qt: 객체/zone 위험 경고 렌더링
        Qt->>Operator: 시각·청각 경고 표출
    else warningDistance 이내
        Core->>UART: Warning zone event
        Core->>Sink: RiskFrame(level=Warning)
        Sink->>Broker: veda/risk, QoS 1
        Broker->>Qt: Warning 표시
    else 차량 없음 또는 임계값 밖
        Core->>UART: None zone event
        Core->>Sink: RiskFrame(level=None)
        Sink->>Broker: veda/risk, QoS 1
        Broker->>Qt: 정상 상태 갱신
    end
```

## 2. 오류·복구 흐름

```mermaid
sequenceDiagram
    autonumber
    participant CCTV as CCTV
    participant Compute as compute-server
    participant Broker as MQTT Broker
    participant Control as control-server
    participant STM as STM32
    participant Qt as Qt

    alt RTSP 단절
        CCTV--xCompute: metadata stream 단절
        Compute->>Compute: session 종료, bounded exponential backoff
        Compute->>CCTV: 재접속
    else compute 프로세스 비정상 종료
        Broker->>Control: retained alive="0" LWT
        Control->>Broker: ChannelStatus(cameraAlive=false)
        Broker->>Qt: 카메라 경로 장애 표시
    else MQTT broker 단절
        Broker--xControl: connection lost
        Control->>Control: 모든 cameraAlive=false callback
        Compute->>Compute: reconnect 대기, bounded queue/drop
    else STM32 heartbeat timeout
        Control--xSTM: heartbeat 응답 없음
        Control->>Broker: ChannelStatus(hardwareAlive=false)
        Broker->>Qt: HW fault, indicator 값은 stale로 게이트
    end
```

## 3. 타이밍 기준점

| 구간 | 현재 구현 기준 | 요구사항 공백 |
| --- | --- | --- |
| CCTV→compute | 카메라 timestamp, RTSP recv timeout 기본 5초 | metadata fps, clock drift 허용값 |
| compute 내부 | packet 단위 동기 pipeline, sink는 비동기 queue | 단계별 p95/p99 예산 |
| compute→control | TopView QoS 0 | 허용 손실률, broker RTT |
| control 집계 | `windowSizeMs` 기본 220ms | 위험 경보용 승인 window |
| control→Qt | Risk QoS 1 | Qt 처리·렌더 지연과 stale 폐기 |
| control→STM32 | UART dispatch + ACK/heartbeat/retry | baudrate, 최대 반영 시간, fault escalation SLA |

종단 지연은 `CCTV ts → Qt render`와 `CCTV ts → STM32 output`을 분리 측정해야 한다. 현재 저장소만으로는 Qt render
시각과 물리 출력 확인 시각을 관측할 수 없으므로, 통합 시험 harness 또는 외부 계측점이 필요하다.
