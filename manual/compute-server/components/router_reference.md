# Router 모듈 레퍼런스 (ParentId 기반 2-신호 분류)

> **대상 파일**
> - 인터페이스: `include/interfaces/IObjectRouter.h`
> - 구현체: `src/route/ParentBasedRouter.h`, `.cpp`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | IObjectRouter 인터페이스 및 ParentBasedRouter(ParentId 기반) 동작 원리 명세 |

---

sanitize를 마친 객체 목록을 **blur 스트림과 risk 스트림으로 가르는 단일 분기점**이다. 판정 근거는 오직 두 개 — **`parentId`(ONVIF `Parent` 속성)** 와 **`cls`(ONVIF `<tt:Type>` 문자열)** 뿐이며, bbox 좌표는 **읽지도 않는다.**

```
Sanitizer ──> [ParentBasedRouter] ──+── risk ──> Ground ──> Transform ──> RiskSink
                                    └── blur ──> ImageMapper ──────────> BlurSink
```

> **설정 주도 룰 엔진이 아니다.** 라우팅 정책은 **소스에 하드코딩**되어 있으며, `config.json`에서 읽어오는 룰 목록·우선순위·조건식 같은 것은 **설계상 존재하지 않는다.** 정책을 바꾸려면 새 `IObjectRouter` 구현체를 만들어 `AppContext`에서 교체한다. (DI)

---

## 1. 인터페이스 명세 (Interface Contract)

```cpp
/**
 * @brief 라우팅 처리된 객체들을 분류하여 담는 결과 컨테이너
 */
struct RouteResult {
    std::vector<domain::DetectedObject> blur;  ///< 블러 경로로 전달할 객체 목록
    std::vector<domain::DetectedObject> risk;  ///< 위험 평가 경로로 전달할 객체 목록
};

class IObjectRouter {
public:
    virtual ~IObjectRouter() = default;
    virtual void route(const domain::ChannelFrame& frame, RouteResult& outResult) = 0;
};
```

### 1.1 계약이 정의하는 라우팅 정책

헤더 주석이 정책 자체를 계약의 일부로 못박고 있다.

| 조건 | 목적지 |
|------|--------|
| `parentId` 있음 **OR** `cls` ∈ {`Head`, `LicensePlate`} | **blur** |
| 그 외 + `cls` ∈ {`Human`, `Vehicle`} | **risk** |
| 그 외 | **drop** |

### 1.2 출력 파라미터 방식 — 무할당 계약

`route()`는 값을 반환하지 않고 **호출자가 소유한 버퍼를 채운다.**

```cpp
virtual void route(const domain::ChannelFrame& frame, RouteResult& outResult) = 0;
```

| 항목 | 내용 |
|------|------|
| **입력** | `const domain::ChannelFrame&` — 프레임을 **읽기만** 하고 변형하지 않는다 |
| **출력** | `RouteResult&` — 호출자(`Pipeline`)가 소유·재사용하는 버퍼 |
| **구현체 의무** | **반드시 두 벡터를 `clear()`로 비운 뒤** 채워야 한다. |

> **⚠️ `clear()` 의무가 계약인 이유**: 호출자가 **같은 버퍼를 계속 재사용**하므로, 비우지 않으면 이전 프레임의 객체가 그대로 남아 **프레임마다 누적**된다. 유령 객체가 위험 판정과 blur 발행에 섞여 들어가며, **컴파일러는 이 실수를 잡아주지 못한다.** `clear()`는 `capacity`를 유지하므로 재할당을 유발하지 않는다.

> **왜 값 반환이 아닌가**: 값 반환 시절에는 프레임마다 `RouteResult`를 새로 만들고 두 벡터를 `reserve`했기 때문에 **프레임당 힙 할당 2회**가 발생했다. 이는 compute-server per-frame 경로에 **유일하게 남아 있던 할당 지점**이었고, 출력 파라미터 전환으로 제거되었다.

### 1.3 아키텍처 의도

> **"정책이 바뀐다면 구현체만 바꾼다"** — 헤더에 명시된 설계 의도다. `Pipeline`은 `IObjectRouter` 인터페이스만 알고, 구체 정책은 `AppContext`가 주입한다.

---

## 2. 판정 트리 (Decision Tree)

### 2.1 구현 전문

```cpp
void ParentBasedRouter::route(const domain::ChannelFrame& frame, RouteResult& outResult) {
    outResult.blur.clear();                          // ① 이전 프레임 잔여물 제거
    outResult.risk.clear();

    outResult.blur.reserve(frame.objects.size());    // ② 첫 프레임만 실제 할당, 이후 no-op
    outResult.risk.reserve(frame.objects.size());

    for (const auto& o : frame.objects) {            // ③ 단일 평면 루프 — O(N)
        if (o.parentId.has_value() || veda::isBlurClass(o.cls)) {
            outResult.blur.push_back(o);
        } else if (veda::isRiskClass(o.cls)) {
            outResult.risk.push_back(o);
        } else {
            if (isLogEnabled(LogLevel::Debug)) {     // ④ 정책상 정의된 drop
                logDebug(kIface, "... 분류 불가 - drop");
            }
        }
    }
}
```

### 2.2 객체 하나의 판정 흐름

```
                    ┌──────────────────────────┐
                    │  parentId.has_value() ?  │
                    └───────────┬──────────────┘
                     ┌── 예 ────┘         └── 아니오 ──┐
                     ▼                                 ▼
                  [ blur ]              ┌───────────────────────────┐
             (부위 객체로 간주)          │ isBlurClass(cls) ?        │
                                        │  Head / LicensePlate      │
                                        └────────┬──────────────────┘
                                    ┌── 예 ──────┘      └── 아니오 ──┐
                                    ▼                                ▼
                                 [ blur ]        ┌──────────────────────────┐
                                                 │ isRiskClass(cls) ?       │
                                                 │  Human / Vehicle         │
                                                 └────────┬─────────────────┘
                                             ┌── 예 ──────┘     └── 아니오 ──┐
                                             ▼                               ▼
                                          [ risk ]                     [ drop + Debug 로그 ]
```

### 2.3 2-신호 이중화 — 이 구현의 핵심

blur 판정에 **두 개의 독립적인 신호**를 OR로 묶은 것이 설계의 요체다.

| 신호 | 출처 | 독립적 실패 가능성 |
|------|------|-------------------|
| `parentId` | ONVIF `Parent` **속성** 파싱 | 속성 누락, 숫자 파싱 실패 |
| `cls` | ONVIF `<tt:Type>` **텍스트** 파싱 | `objectClassFromString`이 모르는 문자열 → `Unknown` |

> **왜 OR인가**: 두 파싱은 **서로 독립적으로 실패**할 수 있다. 하나에만 의존하면 그 파싱이 실패했을 때 **blur 대상을 통째로 잃는다** — 얼굴/번호판이 모자이크되지 않고 노출되는 **개인정보 사고**다. OR로 묶으면 **둘 다 동시에 실패해야만** drop된다.

### 2.4 판정 순서와 조기 종료

`if / else if / else` 체인이므로 **첫 매치에서 즉시 확정**되고 나머지 검사는 실행되지 않는다.

1. **`o.parentId.has_value()`** — `std::optional`의 bool 검사(가장 저렴). 참이면 `||`의 **단축 평가**로 `isBlurClass()`를 **호출조차 하지 않는다.**
2. **`veda::isBlurClass(o.cls)`** — `inline constexpr`, 정수 비교 2회.
3. **`veda::isRiskClass(o.cls)`** — 마찬가지로 정수 비교 2회.

**객체당 최대 3회의 값 비교**로 판정이 끝나며, 순회할 룰 목록이 없으므로 **객체당 비용이 상수**다.

### 2.5 클래스 판별 헬퍼 (`shared/Contract.h`)

```cpp
inline constexpr bool isRiskClass(ObjectClass c) {
    return c == ObjectClass::Human || c == ObjectClass::Vehicle;
}
inline constexpr bool isBlurClass(ObjectClass c) {
    return c == ObjectClass::Head || c == ObjectClass::LicensePlate;
}
```

두 함수 모두 `constexpr` 순수 함수이며 `shared/`에 있어 **compute-server와 control-server가 같은 정의를 공유**한다. 분류 기준이 서버마다 갈라질 수 없다.

### 2.6 클래스 문자열 정규화 (벤더 별칭)

`cls`는 파서가 `objectClassFromString()`으로 만든다. 이 함수는 **정식 이름 + 벤더 별칭 + 대소문자 무시**를 받아들인다.

```cpp
// 1) 정식 이름 — 정확 비교 (compute-server 가 발행하는 값, 가장 흔한 경로)
"Human" / "Vehicle" / "Head" / "LicensePlate"

// 2) 벤더 별칭 — 대소문자 무시
person, pedestrian, people          -> Human
car, truck, bus, motorcycle, bicycle -> Vehicle
face                                 -> Head
plate, license_plate                 -> LicensePlate
```

> **왜 별칭이 필요한가**: ONVIF는 `<tt:Type>` 문자열을 표준화하지 않아 벤더마다 `"Car"`, `"Person"` 등을 보낸다. `Human`/`Vehicle`은 **최상위 객체라 `Parent` 속성이 없어 `cls`가 유일한 라우팅 신호**이므로, 문자열이 조금만 달라도 risk 경로에서 통째로 탈락했다. 별칭 정규화가 그 경로를 막는다.

### 2.7 blur와 risk의 구조적 비대칭 (반드시 이해할 것)

| 경로 | 구제 신호 개수 | `cls`가 `Unknown`일 때 |
|------|----------------|------------------------|
| **blur** | **2개** (`parentId` OR `cls`) | `parentId`가 구제 → 정상 라우팅 ✅ |
| **risk** | **1개** (`cls` 단독) | 구제 수단 없음 → **drop** ⚠️ |

`Human`/`Vehicle`은 **최상위 객체**라 `Parent` 속성이 없다. 따라서 risk 판정은 `cls` 하나에 전적으로 의존한다. 별칭 정규화가 이 단일 신호의 신뢰도를 끌어올리는 **유일한 방어선**이다.

### 2.8 drop 로그 정책

`Unknown` 클래스는 스트림에 섞여 들어오는 게 드물지 않아 **프레임마다 반복될 수 있다.** 정책상 정의된 정상 drop이므로 `Debug` 레벨이며, **문자열 조립까지 레벨 가드 안**에 넣어 운영(Info 이상)에서는 비용이 0이다.

> 원인 추적은 **파서의 Unknown-Type 집계 로그**(1건째 + 이후 100건마다)가 담당한다. 라우터가 같은 사실을 중복 보고하지 않도록 역할을 나눴다.

---

## 3. 메모리 동작 (버퍼 재사용)

```cpp
outResult.blur.clear();                        // size=0, capacity 유지
outResult.risk.clear();
outResult.blur.reserve(frame.objects.size());  // capacity 충분하면 no-op
outResult.risk.reserve(frame.objects.size());
```

| 시점 | 동작 |
|------|------|
| **첫 프레임 (warmup)** | `capacity == 0` → `reserve(N)`이 실제 할당 (2회) |
| **이후 모든 프레임** | `clear()`가 `size`만 0으로, 버퍼는 유지 → `reserve`는 **no-op** → `push_back`이 남은 capacity 안에서 처리 → **할당 0** |

버퍼는 `Pipeline`의 멤버라 **프로세스 수명 내내 살아 있고**, 가장 객체가 많았던 프레임의 capacity가 고수위로 유지된다. (파서 상한 256으로 경계)

> `MqttFrameSink::payloadBuf_`, `RtspClientV2::rtpPacket_`, `RtspOnvifSourceV2::ring_`과 **동일한 재사용 패턴**이다.

---

## 4. Pipeline 통합

### 4.1 호출 위치와 버퍼 소유권

```cpp
// Pipeline.h — 라우터 출력 버퍼를 파이프라인이 소유
RouteResult routeResult_;   ///< 프레임마다 재사용 (onPacket 은 단일 스레드 호출)

// Pipeline.cpp
void Pipeline::onPacket(const domain::RawPacket& raw) {
    domain::ChannelFrame frame = parser_->parse(raw);
    frame = sanitizer_->sanitize(std::move(frame));

    router_->route(frame, routeResult_);      // ★ 분기점 (멤버 버퍼 재사용)
    RouteResult& routed = routeResult_;

    // ==== risk 경로 (먼저 처리) ====
    ... routed.risk -> edge 정책 -> Ground -> Transform -> riskSink_
    // ==== blur 경로 ====
    imageMapper_->map(routed.blur, frame.channelId);
    ... routed.blur -> BlurTarget 변환 -> blurSink_
}
```

`onPacket()`은 **단일 스레드(main 루프)에서만** 호출되므로 멤버 버퍼에 락이 필요 없다.

### 4.2 라우터 뒤에 ImageMapper가 놓인 이유 (중요)

`IImageCoordinateMapper`는 **반드시 라우터 뒤 blur 분기에서만** 호출된다.

> 예전에는 매핑이 **파서 바로 뒤**에 있어 risk 경로까지 앱 표시 좌표계로 변환됐다. 호모그래피는 **메타데이터 이미지 평면**에서 캘리브레이션되므로, 이 매핑이 risk 경로에 섞이면 `imageMapScale/Offset`을 blur 정합용으로 조정하는 순간 **월드 좌표가 조용히 틀어진다.** 기본값이 항등이라 증상이 드러나지 않는 **잠복 결함**이었고, 라우터 뒤로 옮겨 구조적으로 차단했다.

**라우터의 분기가 이 격리의 경계선**이다.

### 4.3 risk를 blur보다 먼저 처리하는 이유

risk 경로가 **안전 크리티컬한 실시간 경로**이므로 sink로 나가는 시점을 앞당긴다. (비용 0의 순서 최적화)

### 4.4 라우터 출력의 하류 소비

| 경로 | 다음 단계 | 최종 산출물 |
|------|-----------|-------------|
| `routed.risk` | `RiskEdgePolicy` 필터 → `IGroundPointExtractor` → `ICoordinateTransform` | `veda::TopViewFrame` (카메라 **로컬** 좌표, m) |
| `routed.blur` | `IImageCoordinateMapper` → `toBlurTarget()` | `veda::BlurFrame` (앱 표시 정규화 좌표) |

> `riskEdgePolicy`는 `AppConfig`의 `// ==== Router Config ====` 섹션에 있지만 **라우터가 아니라 `Pipeline`이 소비한다.** (`PipelineOptions::edgePolicy` → `isEdgeRejected()`) 라우터는 이 설정을 전혀 보지 않는다.

---

## 5. Edge-Worker 원칙

- **좌표를 만들지도, 읽지도 않는다**: 라우터는 `parentId`와 `cls`만 본다. bbox를 **참조조차 하지 않으므로** 기하학적 이상값의 영향을 받지 않는다.
- **채널 단일성**: `frame.channelId`는 **로그 태그로만** 쓰인다. 다른 채널·전역 상태를 참조하지 않으며 `channelCount` 개념이 없다.
- **로컬 좌표 유지**: 라우터를 통과한 뒤에도 좌표는 여전히 카메라 로컬(정규화 이미지 평면)이다.
- **개인정보 경계**: blur/risk 분리는 **개인정보(얼굴·번호판)와 안전판정 데이터를 물리적으로 다른 MQTT 토픽으로 내보내기 위한 경계**다. 이 분기가 무너지면 개인정보가 risk 토픽으로 샐 수 있다.