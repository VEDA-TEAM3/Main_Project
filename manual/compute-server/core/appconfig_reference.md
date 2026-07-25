# `AppConfig.h` 설정 레퍼런스 매뉴얼 (compute-server)

> **대상**: compute-server `config.json` 을 작성·조정하는 현장/운영 엔지니어
> **원본**: compute-server/include/core/AppConfig.h
> **범위**: compute-server 는 **채널(카메라 방향)당 프로세스 1개**다. 이 문서의 모든 값은 그
> 채널의 실행 디렉터리(예: `cctv_01/ch0/`)에 두는 `config.json`에 들어간다.

---

## 0. 로딩 동작 (가장 먼저 알아야 할 것)

`AppConfig::load()` 는 **절대 예외를 던지지 않는다**.

- **파일 없음 / JSON 파싱 실패** → 경고를 찍고 **전체 기본값**으로 기동.
- **개별 키 없음** → 그 항목만 기본값.
- **잘못된 값** → 던지지 않고 **경고 + 자동 보정**. 특히 버퍼 크기/주기 같은 정수 값은
  `clampPositive()`로 **0 이하이면 기본값으로 되돌린다**.

> ⚠️ **호모그래피/이미지 매퍼 값이 구조적으로 잘못되면** `AppContext` 생성자가 예외를 던지고 `main`이 잡아 **프로세스를 종료**

---

## 1. 서버 (`channelId`)

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `channelId` | int | `0` | 이 프로세스가 담당하는 **채널 고유 ID**. MQTT 토픽(`veda/ch/<id>/...`)과 clientId 구분의 기준. **CCTV·채널 전역에서 유일**해야 한다. |

> **조정 가이드**: 다채널 CCTV 4채널이면 `channelId` 0/1/2/3. 2번째 CCTV 는 4/5/6/7 식으로
> 전역 유일하게. `mqttClientId` 도 채널마다 달라야 한다.

---

## 2. RTSP 접속 (`rtspIp` 외) — **현장 필수 입력**

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `rtspIp` | string | `""` | 카메라 IP (**점 표기 IPv4 만** 지원) |
| `rtspPort` | int | `554` | RTSP 포트 |
| `rtspUser` | string | `""` | Digest 인증 사용자 |
| `rtspPass` | string | `""` | Digest 인증 비밀번호 |
| `rtspSetupUri` | string | `""` | SETUP 대상 URI |
| `rtspPlayUri` | string | `""` | PLAY 대상 URI |

> ⚠️ 이 클라이언트는 DESCRIBE 없이 곧바로 SETUP 한다. `rtspSetupUri`/`rtspPlayUri`가 정확해야하며, **PLAY는 트랙이 아니라 세션 aggregate URL** 로 보내야 한다(**틀리면 404/455 로 즉시 끊김**).

---

## 3. RTSP 정책/튜닝 (`rtspConnectTimeoutSec` 외)

성능 기본값은 `performance/compute-server.md` 의 측정값이다.

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `rtspConnectTimeoutSec` | int | `5` | 논블로킹 connect + select 타임아웃(초). 카메라 무응답/방화벽 SYN drop 시 무한 대기 방지 |
| `rtspRecvTimeoutSec` | int | `5` | 수신 소켓 `SO_RCVTIMEO`(초) |
| `rtspSocketRecvBufBytes` | int | `1048576` | 커널 수신 버퍼 `SO_RCVBUF`(bytes) |
| `rtspReadBufBytes` | int | `65536` | 사용자 공간 recv 버퍼(bytes) |
| `rtspMaxMetadataFrameBytes` | int | `1048576` | 재조합 중 metadata frame 상한(bytes) marker 누락/손상 시 무한 누적 방지 → 넘으면 연결 재수립 |
| `rtspKeepAliveIntervalSec` | int | `30` | 세션 유지용 GET_PARAMETER 전송 주기(초) |
| `rtspReconnectBackoffInitialSec` | int | `1` | 재연결 백오프 시작(초) |
| `rtspReconnectBackoffMaxSec` | int | `30` | 재연결 백오프 상한(초, 지수) |

- 위 8개는 모두 `clampPositive()` 적용(0 이하 → 기본값 + 경고).
- `rtspReconnectBackoffMaxSec < Initial` 이면 시작값으로 맞춤(경고).

> **조정 가이드**: 대부분 기본값 유지. 링크가 불안정하면 타임아웃을 조금 늘린다. 백오프는
> 실제로 데이터를 받은 세션에서만 초기화되므로, 잘못된 URI 로 인한 재접속 폭풍은 자동 억제된다.

---

## 4. 소스 링버퍼 (`sourceRingCapacity`)

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `sourceRingCapacity` | int | `8` | Source → Pipeline 링버퍼 슬롯 수 <br> 가득 차면 **drop-oldest** |

> **조정 가이드**: 파이프라인이 순간적으로 밀릴 때 흡수하는 완충 
> 메모리 여유가 적은 RPi 에서는 너무 키우지 않는다.

---

## 5. 로깅 (`logLevel` 외)

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `metricsReportIntervalMs` | int | `5000` | 네트워크/소스 성능 지표 로그 주기(ms) |
| `logLevel` | string | `"info"` | `debug`\|`info`\|`error`\|`off` <br> 알 수 없으면 경고 후 `info` |
| `logToConsole` | bool | `true` | 콘솔 출력 |
| `logToFile` | bool | `true` | CSV 기록 |
| `logFileName` | string | `"veda.csv"` | **고정 파일명** <br> 비면 경고 후 `veda.csv` |
| `logFlushIntervalMs` | int | `500` | 로그 워커 flush 주기(ms) |
| `logMaxPendingEntries` | int | `10000` | 로그 큐 상한 (초과 시 `drop-oldest`, RPi OOM 방지) |

> ⚠️ Docker 배포에서는 채널별로 `logFileName`을 `logs/veda_ch0.csv`처럼 **다르게** 준다.
> 회전/압축은 호스트 `logrotate` 담당 → 이름은 고정

---

## 6. 파서 / 새니타이저 (`edgeEpsilon`, `sanitizer*`)

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `edgeEpsilon` | double | `0.002` | bbox 가 프레임 경계에 '닿았다'고 볼 정규화 좌표 오차율 |
| `sanitizerIouThresh` | double | `0.5` | 규칙 A: risk 객체가 blur객체와 IoU이 값 초과면 팬텀으로 제거 |
| `sanitizerContainThresh` | double | `0.9` | 규칙 B: 같은 클래스에서 작은 bbox가 큰 bbox에 IoMin이 값 초과로 포함되면 제거 |

> **조정 가이드**: 중복 탐지(팬텀)가 많으면 `sanitizerIouThresh`를 낮춘다.

---

## 7. 이미지 매퍼 (`imageMap*`) — blur 표시 좌표

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `imageMapScaleX` / `imageMapScaleY` | double | `1.0` | 표시 좌표 스케일 |
| `imageMapOffsetX` / `imageMapOffsetY` | double | `0.0` | 표시 좌표 오프셋 |

---

## 8. 호모그래피 / 로컬 좌표 (`homography` 외) — **좌표 정확도의 핵심**

risk 경로에서 이미지 지면점 → **카메라 로컬 지상 좌표(m)** 로 변환

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `homography` | double[9] | 항등행렬 | 3×3 호모그래피 행렬 <br> 크기가 9가 아니면 경고 후 기본값 유지 |
| `homographySpace` | string | `"normalized"` | 행렬이 정의된 입력 좌표계 ( `normalized`([0,1]) \| `pixel` ) <br> 알 수 없으면 경고 후 `normalized` |
| `imageWidth` / `imageHeight` | double | `0.0` | `pixel` 일 때 캘리브레이션에 쓴 이미지 해상도 |
| `localBoundsEnabled` | bool | `false` | 변환 결과 로컬 좌표 유효 범위 검사 on/off |
| `localMinX`/`localMaxX`/`localMinY`/`localMaxY` | double | `0.0` | 로컬 좌표 유효 범위(m) <br> `enabled` 인데 `max <= min` 이면 검사 끔(경고) |

> ⚠️ **호모그래피가 잘못되면 그럴듯하지만 틀린 좌표가 나온다**(오류가 안 남). `HomographyTransform`
> 생성자는 구조적으로 잘못된 행렬이면 예외를 던져 프로세스를 죽인다 — 조용히 틀리는 것보다 낫다.
> **반드시 현장 캘리브레이션 값으로 채운다.** `homographySpace` 는 캘리브레이션을 어느 좌표계에서 했는지와 일치해야 한다.

---

## 9. 라우터 (`riskEdgePolicy`)

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `riskEdgePolicy` | string | `"dropBottomTruncated"` | bbox가 잘린 risk 객체 처리 (`keep`(통과) \| `dropBottomTruncated`(아래변 잘린 것만 버림) \| `dropAnyEdge`(어느 변이든 경계에 닿으면 버림)) <br> 알 수 없으면 경고 후 `dropBottomTruncated` |

> **왜 기본이 dropBottomTruncated 인가**: 아래변이 잘리면 발 위치(지면점)를 모른 채 잘린 지점을 지면으로 오인
> → 호모그래피가 실제보다 훨씬 먼 곳으로 사상한다. 그래서 아래변 잘린 객체는 버린다.

---

## 10. MQTT 브로커 / 정책 (`mqttHost` 외) — **현장 필수 입력**

| 키 | 타입 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `mqttHost` | string | `""` | 브로커 IP/호스트 <br> **비면 연결 시작 자체가 실패**(재시도 루프 진입) |
| `mqttPort` | int | `8883` | 브로커 포트(TLS 기본) |
| `mqttCaFile` | string | `""` | TLS CA 인증서 경로 <br> **비면 연결 실패**(TLS 필수 구조) |
| `mqttClientId` | string | `""` | MQTT clientId <br> (**채널마다 반드시 달라야 함**) |
| `mqttKeepAliveSeconds` | int | `30` | MQTT keepalive(초) |
| `mqttRetryIntervalMs` | int | `2000` | 최초 연결 실패 시 재시도 간격(ms) |
| `mqttReconnectDelaySec` | int | `1` | mosquitto 자동 재접속 시작 대기(초, 지수) |
| `mqttReconnectDelayMaxSec` | int | `10` | 자동 재접속 최대 대기( 초 < Delay 면 Delay로 맞춤) |
| `mqttBlurMaxQueueSize` | int | `8` | blur 발행 큐 크기(초과 시 drop-oldest) |
| `mqttTopViewMaxQueueSize` | int | `8` | risk(TopView) 발행 큐 크기 |

> ⚠️ compute-server는 **TLS 전용 구조**다. `mqttHost`/`mqttCaFile` 가 비어 있으면 연결이 시작되지 않는다.
> Docker 배포에서 `mqttCaFile`은 컨테이너 내부 경로
> blur와 risk는 **큐가 분리**되어 있어, blur가 밀려도 risk(안전 크리티컬) 발행은 지연되지 않는다.

---

## 11. 기동 시 경고(`[Config] 경고:`) 한눈에 보기

| 트리거 | 처리 |
| --- | --- |
| 파일 없음 / JSON 파싱 실패 | 전체 기본값 |
| `clampPositive` 대상(버퍼/타임아웃/주기 등)이 0 이하 | 기본값으로 복원 |
| `rtspReconnectBackoffMaxSec < Initial` | 시작값으로 맞춤 |
| `logLevel` 이 debug/info/error/off 아님 | `info` |
| `logFileName` 빈 문자열 | `veda.csv` |
| `homography` 배열 크기 ≠ 9 | 기본(항등) 유지 |
| `homographySpace` 가 normalized/pixel 아님 | `normalized` |
| `localBoundsEnabled` 인데 `max <= min` | 검사 끔 |
| `riskEdgePolicy` 미지원 값 | `dropBottomTruncated` |
| `mqttReconnectDelayMaxSec < Delay` | Delay로 맞춤 |

---

## 12. 현장 엔지니어가 가장 자주 만지는 값 (요약)

1. **`channelId` / `mqttClientId`** — 채널마다 전역 유일 (겹치면 MQTT 세션이 서로 끊는다.)
2. **`rtspIp` / `rtspUser` / `rtspPass` / `rtspSetupUri` / `rtspPlayUri`** — 카메라 접속 (필수)
3. **`homography` / `homographySpace`** — 좌표 정확도의 핵심 (잘못되면 조용히 틀린다)
4. **`mqttHost` / `mqttCaFile`** — 브로커·TLS (비면 연결 안 됨)
5. **`logFileName`** — 채널별로 다르게(`logs/veda_chX.csv`)
6. **`riskEdgePolicy`** — 잘린 객체 처리 정책

> 값 변경 후에는 **기동 로그의 `[Config] 경고:` 유무**를 반드시 확인한다.