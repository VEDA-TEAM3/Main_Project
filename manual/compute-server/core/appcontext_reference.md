# `AppContext` 레퍼런스 매뉴얼 (compute-server)

> **대상**: compute-server 의 조립/생명주기 구조를 이해·수정하려는 개발자
> **원본**: AppContext.h, AppContext.cpp
> **관련 생명주기 파일**: main.cpp, RtspOnvifSourceV2, MqttTransport, MqttFrameSink

---

## 0. 한 줄 요약

`AppContext`는 **의존성 주입(DI) 팩토리이자 생명주기 관리자**다. `config.json` 하나로 모든 구현체를 만들어 서로 배선하고, 스레드를 띄우며, **소멸자(RAII)만으로** 역순으로 안전하게 정리한다. 별도의 `stop()` 메서드는 없다 — 정리는 전적으로 멤버 소멸 순서에 맡긴다.

---

## 1. 역할: Central Registry + Lifecycle Manager

- **DI 팩토리**: `Pipeline`/`Source` 는 인터페이스만 알고, **어떤 구현체를 쓸지는 오직 `AppContext` 가 결정**한다. 동작을 바꾸려면 구현체를 만들어 여기서 갈아끼우면 되고, `Pipeline`/`main` 의 코드는 건드리지 않는다.
- **배선(Wiring)**: 두 Sink가 **하나의 MQTT 커넥션(`transport_`)을 공유**하도록 연결하고, 파이프라인 스테이지들을 순서대로 엮어 `Pipeline` 을 완성한다.
- **생명주기**: 생성자에서 스레드를 기동하고, 소멸자에서 역순으로 join·정리한다.

### 공개 인터페이스

| 멤버 | 용도 |
| --- | --- |
| `explicit AppContext(const AppConfig&)` | 조립 + 스레드 기동 (조립 실패는 **예외**로 알림) |
| `IMetadataSource& source()` | main루프가 `next()`로 패킷을 당길 소스 참조 |
| `Pipeline& pipeline()` | main루프가 `onPacket()`을 호출할 파이프라인 참조 |

> `start()`/`stop()`이 **없다**. 시작 = 생성자, 종료 = 소멸자(RAII)
> main은 `unique_ptr<AppContext>`로 잡고 종료 시 `context.reset()`으로 소멸을 명시적 지점에서 트리거한다.

---

## 2. 관리 컴포넌트

### 2-1. 멤버로 직접 소유 (`AppContext.h`)

선언 순서가 **소멸 순서(역순)를 결정**하므로 의미가 있다.

| 멤버 | 타입 | 역할 |
| --- | --- | --- |
| `source_` | `shared_ptr<IMetadataSource>` (`RtspOnvifSourceV2`) | CCTV 에서 ONVIF 메타데이터를 당겨오는 소스 (생성 즉시 워커 스레드 시작) |
| `transport_` | `shared_ptr<IMqttTransport>` (`MqttTransport`) | 두 Sink 가 공유하는 **단일 MQTT 커넥션** |
| `pipeline_` | `unique_ptr<Pipeline>` | 파싱 → 새니타이즈 → 라우팅 → 좌표변환 → 발행 시퀀스 <br> (스테이지·Sink를 `shared_ptr`로 소유) |

> `transport_`를 `pipeline_`**보다 먼저 선언**한 것이 핵심이다.
> 파괴는 역순이므로, `transport_`를 참조하는 Sink가 **먼저** 정리된 뒤에야 `transport_` 가 사라진다.
> Sink도 shared_ptr로 잡혀 있어 이중으로 안전하지만, 선언 순서로도 의도를 못박아 둔다.

### 2-2. `pipeline_`이 소유하는 스테이지 (생성자에서 주입)

생성자에서 지역 `shared_ptr`로 만들어 `Pipeline`에 넘긴다. 생성자 종료 후에는 **`Pipeline` 만이 유일한 소유자**가 된다.

| 스테이지 | 구현체 | 설정 소스 |
| --- | --- | --- |
| 파서 | `OnvifParser` | `edgeEpsilon` |
| 이미지 매퍼(blur) | `AffineImageCoordinateMapper` | `imageMapScale/Offset X·Y` |
| 새니타이저(팬텀 제거) | `ContainmentSanitizer` | `sanitizerIouThresh`, `sanitizerContainThresh` |
| 라우터 | `ParentBasedRouter` | — |
| 지면점 추출 | `BottomCenterExtractor` | — |
| 좌표 변환(Homography) | `HomographyTransform` | `homography`, `homographySpace`, `localBounds*` |
| risk Sink | `MqttTopViewSink` | `transport_`, `channelId`, `mqttTopViewMaxQueueSize` |
| blur Sink | `MqttBlurSink` | `transport_`, `channelId`, `mqttBlurMaxQueueSize` |

---

## 3. 시작(Startup) 시퀀스 — 순서가 곧 정확성

1. **`source_` 생성** — `RtspOnvifSourceV2` 생성자가 곧바로 워커 스레드를 띄워 RTSP 연결/수신을 시작한다. 아직 아무도 `next()` 를 안 부르므로, 프레임은 링버퍼(`sourceRingCapacity`)에 쌓이고 가득 차면 **drop-oldest**
2. **파이프라인 스테이지 생성** — 파서/매퍼/새니타이저/라우터/지면점/호모그래피 <br>`HomographyTransform`·`AffineImageCoordinateMapper` 생성자는 값이 **구조적으로 잘못되면 예외를 던진다**.
3. **`transport_` 생성** — `MqttTransport`를 만들되 **아직 연결하지 않는다**.
4. **Sink 두 개 생성** — `transport_` 참조를 받아 생성 (연결 리스너는 아직 미등록)
5. **`riskSink->start()` → `blurSink->start()`** — 여기서 **연결 리스너를 등록**하고 싱크 워커 스레드를 띄운다.
6. **`transport_->start()`** — 이제서야 MQTT 접속을 시작한다.
7. **`pipeline_` 조립** — 모든 스테이지 + 두 Sink + `edgePolicy`를 묶어 완성

> ⚠️ **왜 5 → 6 순서인가**: 리스너 등록(`start()`)이 커넥션 시작(`transport_->start()`)보다 **반드시 먼저**여야, 최초 연결 성공 이벤트를 **어느 Sink 도 놓치지 않는다**.
> 순서를 뒤집으면 접속이 리스너 등록보다 앞서 일어나 첫 연결 신호를 잃을 수 있다.
> **왜 4·5 에서 Sink 생성을 생성자에서 안 하고 `start()` 로 분리했나**: 생성자 안에서 `this` 를 리스너로 넘기면 파생 클래스가 아직 완성되기 전에 콜백이 들어올 수 있기 때문

### 실행(Run)

`AppContext` 자체는 루프를 돌리지 않는다. `main.cpp`가 `while (source().next(raw)) pipeline().onPacket(raw);`로 구동한다. 생산자와 소비자는 링버퍼를 통해 분리된 SPSC다.

---

## 4. 조립 실패 처리 (fail-fast)

`AppConfig::load` 는 절대 예외를 던지지 않지만, **구현체 생성자는 던진다**. 잘못된 호모그래피로
계속 돌면 그럴듯하지만 틀린 좌표를 발행하게 되므로, 조용히 틀리느니 즉시 죽는 편이 안전하다.

- `main` 은 `make_unique<AppContext>(config)`를 **try/catch 로 감싼다**. 예외가 나면 원인을 로그로 남기고 `return 1`로 종료한다. (잡지 않으면 `std::terminate` 로 죽어 원인이 안 남는다)

---

## 5. 종료(Graceful Shutdown) 시퀀스 — RAII 역순

`AppContext` 는 `stop()`이 없다. 종료는 **멤버 소멸(역순)** 로만 이뤄진다. main 에서 `context.reset()`이 트리거한다.

**소멸 순서 = 선언의 역순:**

1. **`pipeline_` 소멸 (가장 먼저)** — `unique_ptr`이 `Pipeline`을 파괴 → 스테이지·Sink 에 대한 `shared_ptr` 참조를 놓는다. Sink의 참조 카운트가 0이 되어 **Sink 소멸자 → `shutdown()`** 실행:
   - 연결 리스너를 `transport_`에서 **떼고**,
   - 싱크 워커 스레드를 멈추고 `join`, 큐를 비운다.
2. **`transport_` 소멸 (두 번째)** — 참조 카운트 0이면 `MqttTransport::stop()`:
   - 정상 종료용 **dead 신호("0")를 즉시 발행**,
   - 재시도 스레드/네트워크 루프를 멈추고 mosquitto 클라이언트를 파괴
   - 이 시점엔 Sink가 이미 사라졌으므로, 리스너 콜백이 죽은 Sink를 부르는 일이 없다.
3. **`source_` 소멸 (가장 나중)** — `RtspOnvifSourceV2::~` → `stop()`:
   - 진행 중인 RTSP 세션을 `cancel()`로 즉시 끊어 blocking `recv()`를 깨우고,
   - 워커 스레드를 `join` (이게 없으면 스트림이 건강한 동안 `recv()`가 계속 성공해 `join`이 무한 대기했음)

> **왜 이 역순이 안전한가**: "그 자원을 **참조하는 쪽**을 먼저 없앤다"는 원칙이다.
> Sink는 `transport_`를 참조하므로 Sink(=`pipeline_`)를 먼저 그다음 `transport_`
> `source_` 는 아무도 참조하지 않는 독립 생산자라 마지막에 정리해도 무방하다.

> ⚠️ **잔여 프레임은 보장되지 않는다**: 종료 시 Sink 큐는 flush 하지 않고 drop 한다. (실시간 좌표라 종료 순간의 잔여 프레임은 가치가 없고, 브로커가 죽어 있으면 flush가 무한정 늘어질 수 있음)
> 즉 `SIGINT`/`SIGTERM` 시 마지막 프레임 전달은 의도적으로 미보장이다.

### main 의 종료 흐름 (요약)

1. 전용 `sigwait` 스레드가 SIGINT/SIGTERM 수신 → `source().stop()` → `next()`가 `false` 반환
2. main 루프 종료 → 감시 스레드를 깨워 `join`
3. **`context.reset()`** → 위 1 → 2 → 3 역순 소멸이 실제로 여기서 일어난다.

---

## 6. 엣지 워커 원칙 — 완전한 독립성

`AppContext`는 **오직 자신의 `config`(그 안의 `channelId`)만으로** 동작한다.

- **전역 상태 없음**: MQTT 토픽, clientId, Sink의 프레임 검증 모두 자기 `channelId`로만 결정된다.
- **채널 간 독립**: 한 CCTV의 4채널은 각각 별도 프로세스다. (서로를 모름)
  CCTV를 추가해도 **기존 채널의 어떤 설정도 건드릴 필요가 없다**.
- **책임 분리**: 채널 스티칭·월드 좌표 매핑·전체 위험 판정 같은 **전역 작업은 `control-server` 몫**이며, `compute-server`는 로컬 메타데이터만 출력한다.

---

## 7. 확장/수정 가이드

- **동작을 바꾸려면**: 새 구현체를 만들어 **`AppContext` 생성자에서만** 갈아끼운다.
  `Pipeline`/`main` 은 인터페이스만 아므로 손대지 않는다.
- **파이프라인 순서는 고정**이다.
  스테이지를 추가/교체할 수는 있어도, 시퀀스 자체를 `AppContext` 에서 재배열하지 않는다.
- **시작/종료 순서를 지켜라**: transport 생성 → Sink 생성·`start()` → transport `start()`, 그리고 멤버 선언 순서(= 소멸 역순)
