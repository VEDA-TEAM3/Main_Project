# Control-Server
> 관제 서버 Sink 최적화 과정 및 지표를 정리한 마크다운입니다.

---

# ISink

### 이전: Control Sink (`7f8688e`)

#### 성능 지표

| 측정 경로 | 평균 |
| :--- | ---: |
| `encodeInto(RiskFrame, 0 objects)` | 0.117 us |
| `encodeInto(RiskFrame, 1 object)` | 0.628 us |
| `encodeInto(RiskFrame, 256 objects)` | 127.445 us |
| `MqttTransport construct + destruct` | 0.602 us |
| `publish(disconnected)` | 0.073 us |
| `send(invalid timestamp, reject)` | 0.073 us |
| `ConsoleSink::send(0 objects)` | 0.943 us |
| `ConsoleSink::send(1 object)` | 3.909 us |

#### 기존 동작

1. broker host/port와 CA의 하드코딩 기본값 존재
2. 실제 설정 누락이 기본 endpoint 접속 시도로 이어질 수 있음
3. RiskFrame hot path는 재사용 scratch/payload buffer를 사용
4. ConsoleSink는 객체별 정보를 `std::cout`으로 출력

---

### 이후: MqttTransport / ConsoleSink (`8ca541e`)

#### 성능 지표

| 측정 경로 | 평균 | 중앙값 | p95 | 최소 | 최대 | 반복 |
| :--- | ---: | ---: | ---: | ---: | ---: | :--- |
| `encodeInto(RiskFrame, 0 objects)` | 0.123 us | 0.118 us | 0.126 us | 0.116 us | 0.257 us | 30×10,000 |
| `encodeInto(RiskFrame, 1 object)` | 0.604 us | 0.592 us | 0.698 us | 0.584 us | 0.698 us | 30×10,000 |
| `encodeInto(RiskFrame, 256 objects)` | 127.765 us | 127.667 us | 128.608 us | 126.927 us | 129.134 us | 30×1,000 |
| `MqttTransport construct + destruct` | 0.431 us | 0.430 us | 0.437 us | 0.428 us | 0.450 us | 30×1,000 |
| `publish(disconnected)` | 0.071 us | 0.071 us | 0.072 us | 0.071 us | 0.074 us | 30×10,000 |
| `send(invalid timestamp, reject)` | 0.073 us | 0.073 us | 0.074 us | 0.073 us | 0.074 us | 30×10,000 |
| `ConsoleSink::send(0 objects)` | 0.850 us | 0.848 us | 0.858 us | 0.847 us | 0.858 us | 30×100 |
| `ConsoleSink::send(1 object)` | 3.763 us | 3.763 us | 3.839 us | 3.683 us | 3.962 us | 30×100 |

#### 개선 사항

1. MQTT broker URL과 TLS CA를 config에서 명시적으로 주입하도록 변경
2. scheme, port, TLS CA 누락 및 QoS 범위를 시작·발행 전에 검증
3. WorldFrame을 재사용 `RiskFrame`과 payload buffer로 변환해 정상 상태 할당 최소화
4. 미연결 publish와 잘못된 timestamp를 조기에 거부
5. ConsoleSink의 WorldFrame/ChannelStatus 출력 계약을 GTest로 고정

#### 이전 버전과의 비교

| 지표 | 이전 `7f8688e` | 이후 `8ca541e` | 변화량 |
| :--- | ---: | ---: | ---: |
| RiskFrame 0개 직렬화 | 0.117 us | 0.123 us | 5.1% 증가 |
| RiskFrame 1개 직렬화 | 0.628 us | 0.604 us | **3.8% 감소** |
| RiskFrame 256개 직렬화 | 127.445 us | 127.765 us | 거의 동일 |
| Transport 생성·소멸 | 0.602 us | 0.431 us | **28.4% 감소** |
| 미연결 publish | 0.073 us | 0.071 us | **2.7% 감소** |
| 잘못된 frame 거부 | 0.073 us | 0.073 us | 동일 |
| ConsoleSink 0개 | 0.943 us | 0.850 us | **9.9% 감소** |
| ConsoleSink 1개 | 3.909 us | 3.763 us | **3.7% 감소** |

#### 기능 테스트

| Test suite | 결과 | 검증 범위 |
| :--- | ---: | :--- |
| `ControlConsoleSinkTest` | 3/3 | WorldFrame 및 ChannelStatus 출력 |
| `ControlMqttTransportStateTest` | 1/1 | 초기 상태 |
| `ControlMqttTransportLifecycleTest` | 1/1 | stop 멱등성 |
| `ControlMqttTransportPublishTest` | 3/3 | topic/QoS/미연결 거부 |
| `ControlMqttTransportSubscribeTest` | 2/2 | 구독 입력과 저장 |
| `ControlMqttTransportConfigTest` | 4/4 | URL/port/TLS CA 검증 |
| `ControlMqttTransportSendTest` | 1/1 | timestamp 방어 |
| **합계** | **15/15** | **실패 0** |

#### 측정 환경

| 항목 | 값 |
| :--- | :--- |
| 장비 | Raspberry Pi, aarch64 |
| 컴파일러 | GCC 14.2.0 |
| libmosquitto | 2.0.21 |
| 옵션 | `-std=c++20 -O2 -DNDEBUG -pthread` |
| Clock | `std::chrono::steady_clock` |
| 로깅 | `LogLevel::Off` |
| Broker | 사용하지 않음 |
| 측정 일자 | 2026-07-30 |

#### 주의

- 연결된 `MqttTransport::send()`, `mosquitto_publish()`, TLS 및 QoS ACK는 측정하지 않았습니다.
- ConsoleSink는 `/dev/null` 출력 기준이며 실제 terminal/journald 비용은 포함하지 않습니다.
- 실제 broker 통합 GTest 1건은 로컬 단위 실행에서 제외했습니다.
