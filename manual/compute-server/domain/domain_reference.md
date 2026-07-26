# `domain` 레퍼런스 매뉴얼 (compute-server)

> **대상**: compute-server 파이프라인을 흐르는 핵심 데이터 구조를 이해·수정하려는 개발자
> **원본**: domain/RawPacket.h, domain/ChannelFrame.h, domain/DetectedObject.h, domain/NormBox.h
> **관련 값 타입**: shared/Contract.h

---

## 0. 한 줄 요약

`domain`은 compute-server의 **순수·무의존 데이터 코어**다. 파이프라인이 주고받는 데이터를 **평범한 struct**로 정의하며, 네트워크·MQTT·파서 같은 IO 계층을 전혀 알지 못한다. 이 계층의 **모든 공간 데이터는 카메라 로컬 좌표**이며 월드(도면) 좌표가 아니다.

---

## 1. 역할과 의존성 경계

- **순수 코어**: `domain/` 헤더는 오직 **`shared/Contract.h`(공유 값 타입) + 표준 라이브러리**에만 의존한다. 파서·소스·싱크·OpenSSL·mosquitto 등 **어떤 IO/구현 계층도 참조하지 않는다** → 단독으로 컴파일·테스트 가능하고, 파이프라인 전 단계가 공유하는 안정적 데이터가 된다.
- **얇은 계층**: 상당수가 공유 계약 값 타입의 **별칭(alias)** 이거나, 파이프라인 내부 전용 필드를 몇 개 얹은 구조체다. 즉 값 타입을 중복 정의하지 않고 **wire 계약(Contract.h)의 타입을 재사용**한다.
- **동작 없음**: 로직이 아니라 **데이터**만 담는다. 변환/판정은 파이프라인 스테이지의 몫이다.

---

## 2. 핵심 엔티티 (`domain/`)

### 2-1. `RawPacket` — 파이프라인 입력 (RawPacket.h)

CCTV에서 받은 **원본 메타데이터 바이트**. 소스가 만들어 파이프라인 입구로 넣는다.

| 필드 | 타입 | 의미 |
| --- | --- | --- |
| `channelId` | `veda::ChannelId` | CCTV 채널 ID |
| `bytes` | `std::vector<uint8_t>` | 원본 메타데이터 바이트(아직 파싱 전) |
| `recvTime` | `system_clock::time_point` | RPi 도착 시각. Δ = recvTime − 패킷 내 Timestamp (진단/로그용) |

### 2-2. `ChannelFrame` — 1 프레임 파싱 결과 컨테이너 (ChannelFrame.h)

파서가 `RawPacket` 을 해석해 만든 **한 프레임의 감지 결과 묶음**.

| 필드 | 타입 | 의미 |
| --- | --- | --- |
| `utcTime` | `veda::TimestampMs` | 프레임 UtcTime, epoch ms (CCTV 기준) |
| `channelId` | `veda::ChannelId` | CCTV 채널 ID |
| `objects` | `std::vector<DetectedObject>` | 프레임 내 감지 객체 목록 |

### 2-3. `DetectedObject` — 내부용 단일 객체 (DetectedObject.h)

파이프라인이 다루는 **객체 하나**. 정규화 이미지 좌표계 기준.

| 필드 | 타입 | 의미 |
| --- | --- | --- |
| `id` | `ObjectId` (=`veda::ObjectId`) | 추적 ID (**채널 내에서만** 유일) |
| `parentId` | `std::optional<ObjectId>` | 부모 ID. **Head/LicensePlate 만 값을 가짐**(라우팅 근거) |
| `cls` | `veda::ObjectClass` | 객체 종류 (Unknown/Human/Vehicle/Head/LicensePlate) |
| `box` | `NormBox` | 정규화 이미지 bbox [0,1] |
| `touchesBorder` | `bool` | bbox가 **어느 변이든** 프레임 경계에 닿음 |
| `bottomTruncated` | `bool` | bbox **아래변**이 하단 경계에 닿음 (지면점 신뢰도 판정) |

> **`touchesBorder`와 `bottomTruncated`를 왜 나눴나**: 지면점(bbox 아래변 중앙=발 위치)의 신뢰도가 "어느 변이 잘렸는가"에 따라 완전히 다르기 때문
> 좌/우/위 잘림은 발이 보이므로 지면점 유효, **아래변 잘림은 발 위치를 몰라** 호모그래피가 수 미터 먼 곳으로 사상한다.
> 이 판정은 **파서 (메타데이터 좌표계)에서만** 수행한다.

### 2-4. `NormBox` — 정규화 bbox (NormBox.h)

```cpp
using NormBox = veda::NormRect;   // { l, t, r, b } 모두 [0,1], 좌상단 원점
```

파이프라인 내부용 bbox 이름이지만 실체는 공유 계약의 `NormRect` 별칭이다 — 파서·새니타이저·매퍼가 같은 타입을 공유

### 2-5. 재사용하는 공유 값 타입 (`shared/Contract.h`)

`domain/`이 별도 정의 없이 그대로 쓰는 값 타입들:

| 타입 | 정의 | 의미 |
| --- | --- | --- |
| `ObjectClass` | enum | `Unknown/Human/Vehicle`(risk) / `Head/LicensePlate`(blur) <br> `isRiskClass()`, `isBlurClass()` 헬퍼 제공 |
| `ObjectId` / `ChannelId` / `TimestampMs` | 정수 별칭 | 추적 ID / 채널 ID / epoch ms |
| `NormRect` | `{l,t,r,b}` [0,1] | 좌상단 원점 정규화 사각형 |
| `LocalPoint` | `{x,y}` (m) | **카메라 로컬** 지상 좌표 <br> `x`=전방 기준 좌우 오프셋, `y`=전방 거리 |

---

## 3. 스테이지 사이를 흐르는 인접 타입 (`domain/` 밖)

`domain/`에 있지는 않지만 파이프라인에서 도메인 객체와 함께 흐르는 운반 타입들:

| 타입 | 위치 | 역할 |
| --- | --- | --- |
| `domain::ImagePoint` | interfaces/IGroundPointExtractor.h | `{u,v}` [0,1] 지면 접촉점 <br> `BottomCenterExtractor` 출력 → `HomographyTransform` 입력 |
| `RouteResult` | interfaces/IObjectRouter.h | `{ risk, blur }` 두 `vector<DetectedObject>` 라우터 출력 |
| `veda::TopViewFrame` / `TopViewObject` | shared/Contract.h | **risk 최종 산출물** <br> `TopViewObject{id, cls, pos:LocalPoint, edge}` |
| `veda::BlurFrame` / `BlurTarget` | shared/Contract.h | **blur 최종 산출물** <br> `BlurTarget{id, cls, box:NormRect}` |

> 최종 산출물(`TopViewFrame`/`BlurFrame`)이 `domain::`이 아니라 `veda::`(공유 계약)인 것은 의도적이다. — 이들은 **wire로 나가는 계약 타입**이라 control-server/Qt와 바이트 호환이어야 하기 때문

---

## 4. 데이터 흐름 & 생명주기

한 객체가 **원본 XML → 발행 프레임**까지 어떻게 바뀌는지:

| 단계 | 입력 → 출력 | 도메인 타입 변화 |
| --- | --- | --- |
| 소스 | RTSP 바이트 → **`RawPacket`** | `bytes` 채움, `channelId`/`recvTime` 기록 |
| 파서 | `RawPacket` → **`ChannelFrame`** | XML 파싱 → `DetectedObject` 목록 생성(`box`, `cls`, `parentId`, 경계 플래그) |
| 새니타이저 | `ChannelFrame` → `ChannelFrame` | 팬텀 `DetectedObject` 제거 (in-place 압축) |
| 라우터 | `ChannelFrame` → **`RouteResult{risk, blur}`** | `DetectedObject`를 두 목록으로 분류 |
| risk: 지면점 | `DetectedObject.box` → **`ImagePoint`** | bbox 아래변 중앙 추출 |
| risk: 변환 | `ImagePoint` → **`optional<LocalPoint>`** | 호모그래피(실패 시 폐기) |
| risk: 조립 | `LocalPoint` → **`TopViewObject`** → `TopViewFrame` | 발행용 프레임 구성 |
| blur: 매핑/조립 | `DetectedObject` → **`BlurTarget`** → `BlurFrame` | 앱 표시 좌표 매핑 후 발행용 프레임 구성 |

**소유권/생명주기 메모**

- `RawPacket.bytes`는 소스 링버퍼에서 **소유권 스왑(zero-copy)** 으로 main 스레드에 넘어온다 — 파이프라인이 만지는 순간 그 버퍼는 main 스레드 전용이다.
- `ChannelFrame` 은 파서가 값으로 만들어 `std::move` 로 새니타이저 → 라우터로 넘긴다. (복사 최소화)
- 라우팅 규칙: **`parentId` 가 있거나 blur 클래스 → blur**, risk 클래스 → risk, 그 외(Unknown) → drop
- 도메인 struct는 전부 값 타입이라 스테이지 경계에서 **명시적 소유권 이전**으로만 흐른다 — 숨은 공유/포인터 수명 문제가 없다.

---

## 5. 로컬 좌표 규칙 (엣지 워커 원칙)

**이 도메인 계층의 모든 공간 데이터는 '카메라 로컬' 좌표다. 월드(도면) 좌표는 하나도 없다.**

- **`NormBox`/`NormRect`** — 정규화 **이미지** 좌표 [0,1], 좌상단 원점. 특정 카메라 화면 기준일 뿐, 물리 공간이 아니다.
- **`LocalPoint`** — 호모그래피 변환 결과인 **카메라 로컬 지상 좌표(m)** (원점은 그 카메라이고 `x`=전방 기준 좌우 오프셋, `y`=전방 거리) <br> **타입 이름이 `LocalPoint`(≠ `WorldPoint`)인 것 자체가 계약**이다 — compute-server는 도면을 모르므로 로컬만 낸다.
- **월드 변환은 control-server의 몫**: 로컬 → 도면 공통 월드 좌표로 옮기고(카메라 설치 위치/방위 적용), 채널을 스티칭하는 전역 작업은 전부 control-server가 한다. compute-server 도메인은 그 경계 **이전**의 로컬 데이터만 표현한다.

> 이 규칙이 깨지면 위험 판정이 조용히 틀어진다. 그래서 타입 이름(`LocalPoint`)과 계층 경계로 **컴파일 타임에** 못박아 둔다.

---

## 6. 설계 노트

- **왜 이렇게 얇은가**: 값 타입을 `domain`과 `veda`(Contract) 양쪽에 중복 정의하면 필드가 어긋날 위험이 있다. 그래서 `NormBox`/`ObjectId` 등은 계약 타입을 **별칭**으로 재사용하고, 파이프라인 내부에서만 필요한 필드(`parentId`, `touchesBorder`, `bottomTruncated`)만 `DetectedObject` 에 얹었다.
- **`DetectedObject`(내부) vs `TopViewObject`/`BlurTarget`(발행)**: 내부 처리용 필드(경계 플래그, parentId)는 wire로 내보내지 않는다. 파이프라인 끝에서 발행에 필요한 최소 필드만 계약 타입으로 옮겨 담는다 — 내부 표현과 wire 표현의 분리
