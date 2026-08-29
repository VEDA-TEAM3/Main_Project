# 시스템 문서화 진단

- 기준일: 2026-08-07
- 기준 소스: `compute-server/`, `control-server/`, `shared/`, `deploy/`, `tests/`, `performance/`
- 판정 표기: **확인**은 코드로 검증된 사실, **추론**은 구현에서 도출한 요구, **미확정**은 외부 자료가 필요한 항목이다.

## 현재 파악 가능한 시스템 범위

| 영역 | 파악된 범위 | 상태 |
| --- | --- | --- |
| 영상 분석 입력 | CCTV가 생성한 ONVIF 메타데이터를 RTSP/TCP로 수신한다. 영상 디코딩이나 AI 추론은 저장소 범위 밖이다. | 확인 |
| 엣지 처리 | 채널별 `compute-server`가 파싱, 중복 제거, risk/blur 분기, 로컬 좌표 변환을 수행한다. | 확인 |
| 중앙 관제 | `control-server`가 MQTT 프레임을 시간 윈도우로 집계하고 월드 좌표 변환, 다채널 융합, zone 배정, 위험 판정을 수행한다. | 확인 |
| 사용자 화면 | Qt는 `RiskFrame`, `BlurFrame`, `ChannelStatus` 소비자로 계약에 등장하지만 구현 소스는 없다. 영상 수신 경로도 확인되지 않는다. | 미확정 |
| 현장 경보 | control-server가 zone별 위험도를 UART 프레임으로 STM32에 전달하고 ACK/heartbeat 및 표시 상태를 수신한다. | 확인 |
| 통신 | MQTT는 TLS를 지원/강제하는 경로가 있으나 RTSP 클라이언트는 현재 평문 TCP와 Digest MD5를 사용한다. | 확인 |
| 배포 | 채널당 compute-server 컨테이너 1개, CCTV당 4채널 예시와 Raspberry Pi 배포 절차가 있다. control-server/Qt 배포 정의는 없다. | 확인 |
| 검증 | GoogleTest 단위 테스트와 sanitizer/coverage CI 정의가 있다. 시스템·현장·Qt 연동 시험 규격은 없다. | 확인 |

## 현재 프로젝트 상태

- `compute-server` 단독 Release 빌드는 2026-08-07 로컬 검증에서 성공했다.
- 전체 빌드는 `control-server/src/core/Controller.cpp:142`에서 중단된다. `IRiskPolicy::evaluate`는
  `(WorldFrame&, RiskEvaluation&)`를 요구하지만 호출부는 인자 하나와 반환값을 사용한다.
- `SerialHwEventDispatcher.cpp`와 `DriverProtocolTest.cpp`는 little-endian 및 payload 검증 helper를 호출하지만
  현재 `shared/driver_protocol.h`에는 해당 선언이 없어 독립 문법 검사도 실패한다.
- `shared/Contract.h`는 `BlurFrame` 전송을 RTSP/RTP라고 설명하지만 실제 `MqttBlurSink`는
  `veda/ch/{ch}/blur`, QoS 0으로 MQTT 발행한다.
- `ChannelStatus` 주석은 “v2” 필드를 설명하지만 전역 `kSchemaVersion`은 1이다. Qt 호환 토픽도 병행 발행 중이므로
  스키마/마이그레이션 정책 확정이 필요하다.
- CI 주석의 테스트 범위 설명과 현재 `tests/CMakeLists.txt` 등록 상태가 일치하지 않는다.
- 기본 `channelCount=4`, `ChannelId` 주석 `0..3`, 배포 문서의 최대 12채널 설명 사이에 확장 범위 정의가 필요하다.

## 즉시 작성한 5개 핵심 문서

1. [요구사항 명세서](requirements_specification.md)
2. [유스케이스 명세](use_cases.md)
3. [시스템 구성도](system_architecture.md)
4. [컴포넌트 명세](component_design.md)
5. [위험 감지 시퀀스](hazard_detection_sequence.md)

## 추가 작성·보완이 필요한 문서

| 우선순위 | 문서 | 필요한 이유 |
| --- | --- | --- |
| P0 | 인터페이스 제어 문서(ICD) | MQTT 토픽, JSON schema, QoS/retain, UART byte order와 버전 호환을 단일 승인 문서로 고정해야 한다. |
| P0 | 검증·확인 계획(V&V) 및 요구사항 추적표 | 요구사항→단위/통합/현장 시험의 양방향 추적이 없다. |
| P0 | control-server/Qt 배포·복구 Runbook | 설치, 인증서, systemd/container, 롤백, 백업, 장애 대응 절차가 없다. |
| P0 | Qt 클라이언트 계약 및 화면 상태 명세 | 영상 경로, blur 동기화, stale hardware 상태 게이팅, 경고 UX가 저장소 밖이다. |
| P0 | 안전 분석(HARA/FMEA 또는 Safety Case) | 미검출, 오경보, 좌표 오보정, 경보장치 고장 시 안전 목표와 위험 수용 기준이 없다. |
| P0 | 승인된 설정 기준선 및 예제 | 카메라/zone/호모그래피/월드 캘리브레이션과 secret 배치의 검증 가능한 기준 파일이 없다. |
| P1 | 통합 보안 위협 모델 | MQTT TLS, 평문 RTSP, 자격증명, UART 위조, 인증서 갱신과 네트워크 분리를 시스템 수준에서 다뤄야 한다. |
| P1 | SLO·모니터링·알람 명세 | 종단 지연, 가용성, 프레임 드롭, 재연결, heartbeat에 대한 목표값과 알람 임계값이 미정이다. |
| P1 | 캘리브레이션 및 현장 인수 시험서 | 로컬/월드 좌표, zone 경계, 히스테리시스, 경보 채널 매핑을 현장에서 검증해야 한다. |
| P1 | 개인정보·데이터 보존 정책 | blur 메타데이터, 로그, 영상 접근, 저장 기간과 감사 정책이 없다. |
| P1 | 릴리스·스키마 마이그레이션 계획 | 서버/Qt/STM32의 호환 행렬, legacy 토픽 폐기 조건, 롤백 정책이 필요하다. |
| P2 | ADR, 용어집, SBOM/라이선스, 용량 계획 | 의사결정 근거, 공통 언어, 공급망, 확장 한계를 운영 가능한 형태로 유지해야 한다. |

## 확정이 필요한 질문

1. Qt 애플리케이션과 영상 수신/렌더링 소스는 어느 저장소에 있는가?
2. 위험 감지부터 화면·경광등 반영까지 허용할 p95/p99 지연과 시스템 가용성 목표는 얼마인가?
3. 운영 채널 상한은 4개인가, Raspberry Pi 문서처럼 12개 이상인가?
4. STM32는 단일 보드가 여러 zone을 제어하는가, 채널별 보드인가?
5. RTSP 구간은 물리적으로 격리된 신뢰망인가, RTSPS/VPN 적용이 필수인가?
6. Warning/Danger 거리와 zone 경계의 승인 주체 및 현장 변경 절차는 무엇인가?
