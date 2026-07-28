# ParentBasedRouter 계층 보안·성능 권고 (Security & Performance Advisory)

> **대상 모듈**
> - 인터페이스: `include/interfaces/IObjectRouter.h`
> - 구현체: `src/route/ParentBasedRouter.h`, `.cpp`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | ParentBasedRouter 계층 논리적 보안성 검증, O(N) 성능 분석 및 할당 최적화 명세 |

---

## 1. 위협 모델 (Threat Model)

라우터는 **순수 분류 계층**이다. 소켓도, 파일도, 설정도, 좌표 계산도 없다. 판정에 쓰는 입력은 **`parentId`와 `cls` 단 두 필드**뿐이며, 위협은 그 두 필드의 **조작·누락**으로만 들어온다.

| 위협 범주 | 시나리오 | 성립 여부 / 영향 |
|-----------|----------|------------------|
| 룰 홍수 (CPU) | 룰 목록 폭증으로 O(N×M) 유발 | ✅ **구조적 불가** — 룰 목록이 존재하지 않음 |
| 설정 오염 | 악의적 `config.json` 룰 주입 | ✅ **구조적 불가** — 라우팅 설정 표면 없음 |
| 룰 재귀 | 룰이 서로를 참조해 스택 고갈 | ✅ **구조적 불가** — 룰 참조 개념도, 재귀도 없음 |
| 기하학적 이상값 | 음수/역전/NaN bbox 주입 | ✅ **무관** — 라우터는 bbox를 **참조조차 하지 않음** |
| 객체 홍수 (CPU) | 객체 대량 주입으로 O(N) 순회 부하 | ✅ 파서 상한 **256**으로 경계됨 |
| **클래스 문자열 불일치** | 벤더 표기 차이(`"Car"` 등) | ⚠️ 별칭 정규화로 **대폭 완화**, 완전 미지 문자열은 여전히 drop |
| **Parent 속성 조작** | risk 객체에 `Parent` 속성을 붙임 | ⚠️ 성립 — risk 객체가 blur로 흡수됨 |
| 개인정보 유출 | `Head`/`LicensePlate`가 risk 토픽으로 누출 | ✅ **경로 없음** |

### 실패 방향 기준

- **과잉 검출(안전 방향)**: 객체가 실제보다 많아짐 → 경보가 더 울림 <br> 성가시지만 **놓치지 않는다.**
- **과소 검출(위험 방향)**: 실제 위험 객체가 사라짐 → **경보가 울리지 않는다.**

라우터에서 위험 방향으로 실패하는 경로는 **모두 "risk 경로에서의 탈락"**

---

## 2. 구조적 면역 — 룰 엔진의 부재가 곧 방어다

이 계층에서 가장 중요한 보안 속성은 **구현되지 않은 것**에서 나온다.

| 확인 항목 | 결과 |
|-----------|------|
| 설정 파일에서 읽는 라우팅 룰 목록 | ❌ 존재하지 않음 |
| 룰 우선순위·조건식·DSL | ❌ 존재하지 않음 |
| 룰 간 참조 / 중첩 평가 | ❌ 존재하지 않음 |
| 실제 정책 | **소스에 하드코딩된 2-신호 판정** |

> `AppConfig`에 `// ==== Router Config ====` 섹션이 있으나 키는 `riskEdgePolicy` **하나뿐**이며, 이는 **라우터가 아니라 `Pipeline`이 소비**한다. 라우터는 어떤 설정값도 읽지 않는다.

**정책을 데이터가 아니라 코드로 고정한 결과**, 다음 위협군이 통째로 성립하지 않는다.

| 가정된 위협 | 왜 불가능한가 |
|-------------|---------------|
| 룰 홍수로 CPU 고갈 | 순회할 룰 컬렉션이 없음 → 복잡도에 `M` 축 자체가 없음 |
| 룰 수천 개로 OOM | 룰을 담을 저장소·설정 필드가 없음 |
| 룰 재귀로 스택 오버플로 | 룰이 다른 룰을 부르는 구조가 없음 |
| 설정 조작으로 정책 변조 | 라우팅 설정 표면이 없음 → 정책 변경은 **컴파일 타임 경로**(새 구현체 + `AppContext` 교체)로만 가능 |

> **➡️ 공격 표면을 줄이는 가장 확실한 방법은 만들지 않는 것이다.** 이 계층은 그 원칙의 사례다.

---

## 3. 스택 안전성 (Stack Safety)

### 3.1 ✅ 재귀 없음 — 구조적 면역

```cpp
for (const auto& o : frame.objects) {
    if (...) { ... } else if (...) { ... } else { ... }
}
```

- 함수 본체는 **단일 평면 range-for 루프**이며 내부는 분기 3개뿐이다.
- **자기 호출도, 상호 호출도, 룰 간 참조도 없다.**
- 호출 헬퍼(`isBlurClass`, `isRiskClass`)는 **`inline constexpr` 리프 함수**로 정수 비교 2회 후 즉시 반환한다. 대부분 인라인되어 스택 프레임조차 만들지 않는다.

### 3.2 ✅ 지역 저장소는 O(1)

지역 변수는 루프 참조 하나뿐이다. **출력 버퍼조차 호출자 소유**이므로 라우터의 스택 사용량은 입력 크기 `N`과 **완전히 무관**하다. 큰 지역 배열도 VLA도 없다.

> **➡️ 결론: 어떤 입력으로도 스택 고갈이 발생하지 않는다.**

---

## 4. 메모리 안전성 & 무할당 준수 (Memory Safety)

### 4.1 ✅ 입력은 상류에서 이중 경계된다

라우터 자체에는 상한 검사가 없지만, 입력 크기가 이미 두 번 제한된다.

| 경계 | 값 | 강제 지점 |
|------|-----|-----------|
| RTSP 프레임 바이트 | 1 MiB | `RtspClientV2` (`maxMetadataFrameSize_`) |
| 프레임당 객체 수 | **256** | `OnvifParser` (`kMaxObjectsPerFrame`) |

따라서 `frame.objects.size() <= 256`이 보장되고, 출력도 `blur.size() + risk.size() <= 256`을 넘을 수 없다.

**최악 메모리 사용량**:

```
2 × 256 × sizeof(domain::DetectedObject) ≈ 2 × 256 × 56 B ≈ 28 KB
```

`DetectedObject`는 **힙 멤버가 없는 값 타입**(`ObjectId`, `optional<ObjectId>`, `enum`, `NormBox`(double 4개), `bool` 2개)이므로 **원소 자체가 추가 할당을 유발하지 않는다.** 무제한으로 자라는 컨테이너가 없어 **OOM 위험이 없다.**

### 4.2 ✅ 무할당(zero-allocation) 파이프라인 규칙 준수

**질문: `RouteResult` 벡터가 프레임마다 할당하는가? → 아니다. 재사용된다.**

```cpp
void ParentBasedRouter::route(const domain::ChannelFrame& frame, RouteResult& outResult) {
    outResult.blur.clear();                        // size=0, capacity 유지
    outResult.risk.clear();
    outResult.blur.reserve(frame.objects.size());  // capacity 충분하면 no-op
    outResult.risk.reserve(frame.objects.size());
    ...
}
```

**동작 원리** — `std::vector::clear()`가 `capacity()`를 유지한다는 성질에 기반한다.

| 시점 | 동작 |
|------|------|
| **첫 프레임(warmup)** | `capacity == 0` → `reserve(N)`이 실제 할당 (2회) |
| **이후 모든 프레임** | `clear()`가 `size`만 0으로, 버퍼 유지 → `reserve`는 **no-op** → `push_back`이 남은 capacity 안에서 처리 → **할당 0** |

버퍼는 **`Pipeline`의 멤버(`routeResult_`)** 로 프로세스 수명 내내 살아 있고, 최대 객체 수 프레임의 capacity가 고수위로 유지된다. (28 KB 이내로 경계)

**파이프라인 전 계층 대조**:

| 계층 | 프레임당 힙 할당 | 기법 |
|------|------------------|------|
| Source (`RtspOnvifSourceV2`) | **0** | `std::swap` 버퍼 소유권 교환 |
| Sanitizer (`ContainmentSanitizer`) | **0** | 스택 `std::bitset` + in-place `resize` |
| **Router (`ParentBasedRouter`)** | **0** ✅ | 출력 파라미터 + `clear()` 재사용 |
| Sink (`MqttFrameSink`) | **0** | 재사용 `payloadBuf_` + `encodeInto` |

> **➡️ compute-server의 per-frame 경로가 전 계층 무할당으로 통일되어, zero-allocation 원칙이 예외 없이 성립한다.**

**검증**: 전역 `operator new`를 계측한 결과 — warmup 이후 **100회 연속 `route()` 호출에서 힙 할당 0회**, capacity 보존(blur 64 → 64, risk 64 → 64), 그리고 **직전 프레임 잔여물 없음** (64객체 프레임 뒤 4객체 프레임 → blur 2 / risk 2)

### 4.3 ⚠️ 버퍼 재사용의 유일한 위험 — `clear()` 누락

버퍼를 재사용하는 설계에서 **가장 위험한 회귀는 `clear()` 빠뜨림**이다.

- 빠뜨리면 객체가 **프레임마다 누적**되어 유령 객체가 위험 판정과 blur 발행에 섞인다.
- **컴파일러가 잡아주지 못한다** — 타입은 완벽히 유효하다.
- 방어책: `IObjectRouter` 헤더에 `@warning`으로 의무를 명문화했고, 위 검증 항목에 "직전 프레임 잔여물" 테스트를 포함했다.

> 새 `IObjectRouter` 구현체를 작성할 때 **반드시 두 벡터를 먼저 `clear()`할 것**

---

## 5. 논리적 취약점 (Logical Vulnerabilities)

라우터는 bbox를 읽지 않으므로 **기하학적 이상값(음수·역전·NaN·0 면적)은 이 계층과 무관**하다. 실제 위험은 **분류 신호의 조작·누락**에서 나온다.

### 5.1 ⚠️ 클래스 문자열 불일치 — 별칭 정규화로 대폭 완화됨

**구조적 배경 — 비대칭이 문제의 뿌리**:

| 경로 | 구제 신호 | `cls`가 `Unknown`일 때 |
|------|-----------|------------------------|
| **blur** | `parentId` **OR** `cls` (2개) | `parentId`가 구제 → 정상 라우팅 ✅ |
| **risk** | `cls` **단독** (1개) | 구제 수단 없음 → **drop** ⚠️ |

`Human`/`Vehicle`은 **최상위 객체라 `Parent` 속성이 없다.** 따라서 risk 판정은 `cls` 하나에 전적으로 의존하며, 이것이 **위험 방향(과소 검출) 실패의 유일한 통로**다.

**완화책 — 별칭 + 대소문자 무시 정규화**:

`objectClassFromString`이 정식 이름 외에 벤더 별칭을 받아들인다.

```
person, pedestrian, people           -> Human
car, truck, bus, motorcycle, bicycle -> Vehicle
face                                 -> Head
plate, license_plate                 -> LicensePlate
(대소문자 무시: "HUMAN", "vehicle" 등도 정상 매핑)
```

**현재 거동**:

| 카메라가 보내는 문자열 | 결과 |
|------------------------|------|
| `"Vehicle"` / `"Human"` (정식) | ✅ 정상 |
| `"Car"` / `"truck"` / `"bus"` | ✅ **Vehicle로 정규화** |
| `"Person"` / `"pedestrian"` | ✅ **Human으로 정규화** |
| `"HUMAN"` (대소문자 차이) | ✅ **Human으로 정규화** |
| `"Vehicle "` (후행 공백) | ⚠️ **Unknown → drop** |
| 완전히 미지의 문자열 | ⚠️ **Unknown → drop** (의도된 동작) |

**잔여 위험 (I3)**: 앞뒤 **공백 트리밍이 없다.** XML 텍스트 노드에 개행·들여쓰기가 섞여 들어오는 경우가 있어 실제로 발생 가능하며, 실패 방향은 **위험 측**이다. 진입부 trim으로 해소 가능하다.

**남은 탐지 수단**: 파서의 Unknown-Type 집계 로그(1건째 + 이후 100건마다, `sanitizeForLog`로 정화)가 유일한 신호다. 프로세스는 정상 기동하고 프레임도 계속 발행되므로, **신규 카메라 도입 시 이 로그 확인을 운영 체크리스트에 포함**할 것을 권고한다.

### 5.2 ⚠️ `Parent` 속성이 붙은 risk 객체는 blur로 흡수된다

```cpp
if (o.parentId.has_value() || veda::isBlurClass(o.cls)) {   // parentId 가 최우선
    outResult.blur.push_back(o);
}
```

`parentId`가 **`cls`보다 먼저** 평가되므로, `cls == Vehicle`이면서 `Parent` 속성을 가진 객체는 **blur로 라우팅**되고 risk 경로에서 사라진다.

- **정상 동작이다**: ONVIF에서 `Parent`는 "이 객체는 다른 객체의 부위"라는 뜻이므로, Parent를 가진 Vehicle은 통상 오분류된 부위 객체다.
- **다만 조작 가능하다**: 악의적 카메라가 모든 `Vehicle`에 `Parent="1"`을 붙이면 **전량 blur로 흡수 → risk 전멸 → 경보 미발생**
- **현실적 위험도 (낮음)**: 카메라를 완전히 장악해야 가능하고, 그 수준의 공격자는 애초에 빈 프레임을 보내는 편이 더 쉽다. **정보성으로 기록**한다.

### 5.3 ✅ 알 수 없는 클래스는 크래시 없이 fail-safe 처리된다

- `Unknown`은 `isBlurClass`·`isRiskClass` 모두 false → `else` 분기에서 **Debug 로그 후 drop**
- 예외를 던지지 않고, 널 역참조도 없다. `std::optional`은 **`has_value()`로 존재 여부만 확인**하고 값을 꺼내지 않으므로 **잘못된 접근 자체가 불가능**하다.
- **➡️ 어떤 클래스 값·`parentId` 조합에도 크래시하지 않는다.** ✅

---

## 6. 개인정보 격리 경계 (Privacy Isolation Boundary)

**검증 질문: `Head`/`LicensePlate`가 risk 토픽으로 새어나갈 논리적 경로가 존재하는가? → 존재하지 않는다.**

risk 경로에 진입하려면 **다음 두 조건을 동시에** 만족해야 한다.

```cpp
o.parentId.has_value() == false      // 조건 ①: Parent 속성이 없어야 함
&& veda::isBlurClass(o.cls) == false // 조건 ②: Head/LicensePlate 가 아니어야 함
&& veda::isRiskClass(o.cls) == true  // 조건 ③: Human/Vehicle 이어야 함
```

**조건 ②와 ③은 상호 배타적**이다 — `isBlurClass`와 `isRiskClass`는 서로소인 집합을 검사한다.

```cpp
isRiskClass: Human, Vehicle
isBlurClass: Head, LicensePlate      // 교집합 = ∅
```

따라서 `cls`가 `Head`나 `LicensePlate`인 객체는 **`isRiskClass`가 반드시 false**이며, risk 분기에 도달할 수 없다.

**모든 실패 조합을 열거해도 결론이 같다**:

| `parentId` | `cls` | 결과 |
|------------|-------|------|
| 있음 | `Head`/`LicensePlate` | **blur** ✅ |
| 있음 | `Unknown` (Type 파싱 실패) | **blur** ✅ (parentId가 구제) |
| 없음 (Parent 파싱 실패) | `Head`/`LicensePlate` | **blur** ✅ (cls가 구제) |
| 없음 (Parent 파싱 실패) | `Unknown` (Type 파싱 실패) | **drop** ✅ (risk 아님) |

> **➡️ 두 신호가 동시에 실패하는 최악의 경우조차 `Unknown`이 되어 `isRiskClass`가 false이므로 risk가 아니라 drop된다.** 개인정보가 risk 토픽으로 유출되는 경로는 **논리적으로 존재하지 않는다.**

**최악의 결과는 "blur 대상 유실"**(모자이크 누락)이며, 이는 **다른 종류의 개인정보 사고**다. 그래서 blur 판정을 2-신호 OR로 이중화해 그 확률을 낮춘 것이다.

> **⚠️ 유지보수 경계**: 이 격리는 **`isRiskClass`와 `isBlurClass`가 서로소**라는 사실에 의존한다. `shared/Contract.h`에서 두 집합이 겹치도록 수정하면 **격리가 즉시 무너진다.** 클래스 집합 변경 시 반드시 이 불변식을 재확인할 것

---

## 7. 계산 복잡도 (Computational Complexity)

### 7.1 ✅ 엄격히 O(N)임을 확인

```
비용 = N × (상수 판정 비용)
```

| 항목 | 값 |
|------|-----|
| 복잡도 | **O(N)** — 객체당 1회 순회, 중첩 루프 없음 |
| 객체당 연산 | `optional::has_value()` 1회 + `constexpr` 정수 비교 **최대 4회** |
| 최대 N | **256** (파서 상한) |
| 최악 총 연산 | 256 × 약 5회 ≈ **1,300회 비교** |
| 추정 소요 | 라즈베리파이 기준 **수 마이크로초** |
| 프레임 예산 | 5 fps → **200 ms** |

**➡️ 최악 부하에서도 프레임 예산의 0.01% 미만이다.**

### 7.2 ✅ 룰 엔진 부재의 복잡도상 의미

| 구조 | 복잡도 | 공격자가 키울 수 있는 축 |
|------|--------|--------------------------|
| 설정 주도 룰 엔진 (가정) | O(N × M) | **M (룰 수)** — 설정 파일로 조작 가능 |
| **현재 (하드코딩 2-신호)** | **O(N)** | N뿐 — **파서가 256으로 상한** |

**두 축 모두 봉쇄되어 이 계층을 통한 CPU 고갈은 불가능하다.**

### 7.3 ✅ 조기 종료 최적화

- `if / else if / else` 체인은 **첫 매치에서 확정**되고 나머지 검사를 건너뛴다.
- `||`의 **단축 평가**: `parentId.has_value()`가 참이면 `isBlurClass()`를 **호출하지 않는다.** 가장 저렴한 검사를 앞에 둔 순서가 적절하다.
- `isBlurClass`/`isRiskClass`가 `constexpr`이라 컴파일러가 인라인·상수 폴딩하며, 분포가 안정적이라 분기 예측도 잘 듣는다.

---

## 8. 감사 요약

| 영역 | 판정 | 근거 |
|------|------|------|
| **알고리즘 복잡도 (O(N) 검증)** | ✅ **All Clear** | 단일 평면 루프, 객체당 상수 비용, 중첩 없음 <br> **룰 축 M 부재**로 O(N) 확정 (N ≤ 256 → 프레임 예산의 0.01% 미만) |
| **OOM & 메모리 할당** | ✅ **All Clear** | 출력 파라미터 + `clear()` 재사용으로 **정상 상태 힙 할당 0** (100회 호출 검증). 상류 이중 상한으로 최악 28 KB 경계 <br> 원소가 힙 멤버 없는 값 타입 |
| **스택 안전성** | ✅ **All Clear** (구조적 면역) | 재귀·상호 호출·룰 참조 전무 <br> 지역 저장소 O(1), 출력 버퍼도 호출자 소유 |
| **논리적 취약점** | ⚠️ **Warning (경미)** | bbox 미참조로 기하 이상값 무관, Unknown fail-safe drop, 크래시 경로 없음 <br> **단 risk 경로의 단일 신호 의존이 잔여 위험** |
| **개인정보 격리** | ✅ **All Clear** (논리적 증명) | `isRiskClass` ∩ `isBlurClass` = ∅ 이므로 **모든 신호 실패 조합에서도 유출 경로 없음** |

### 지적 사항 목록

| # | 항목 | 등급 | 실패 방향 | 상태 |
|---|------|------|-----------|------|
| **I3** | 클래스 문자열 앞뒤 공백 미트리밍 (`"Vehicle "` → Unknown) | ⚠️ Warning | 🔴 위험(과소 검출) | ⬜ 미조치 — 진입부 trim으로 해소 가능 <br> XML 개행/들여쓰기로 실제 발생 가능 |
| **I1** | `Parent` 붙은 risk 객체가 blur로 흡수됨 | ℹ️ Info | 🔴 위험(이론상) | ⬜ 미조치 — 정상 동작이며 카메라 완전 장악이 전제라 현실적 위험도 낮음 |
| **I2** | `riskEdgePolicy`가 "Router Config"에 있으나 Pipeline이 소비 | ℹ️ Info | — | ⬜ 미조치 — 보안 영향 없음(검증·폴백 존재), 명명 혼동만 |
| **I4** | `clear()` 누락 시 유령 객체 누적 (컴파일러 미검출) | ℹ️ Info | 🔴 위험(오염) | ✅ 계약 명문화 — 헤더 `@warning` + 잔여물 회귀 테스트로 방어 |

**총평**: `ParentBasedRouter`는 **감사 대상 네 영역 모두에서 견고**하다.

- **복잡도**: 룰 엔진을 만들지 않은 덕분에 O(N×M)이 아닌 **엄격한 O(N)** 이며, 공격자가 키울 수 있는 축이 파서 상한(256)으로 봉쇄된 `N` 하나뿐이다.
- **메모리**: 출력 파라미터 + `clear()` 재사용으로 **정상 상태 할당 0**을 달성해, compute-server per-frame 경로가 **전 계층 무할당**으로 통일되었다.
- **개인정보 격리**: `isRiskClass`와 `isBlurClass`가 **서로소**라는 사실로부터, 두 신호가 동시에 실패하는 최악의 경우조차 유출이 아니라 drop으로 귀결됨을 **논리적으로 증명**했다.

Critical 결함은 없다. 남은 것은 **risk 경로가 `cls` 단일 신호에 의존**한다는 구조적 비대칭이며, 별칭 정규화로 대폭 완화되었으나 **공백 트리밍(I3)** 은 아직 열려 있다. 실패 방향이 위험 측이므로 후속 조치를 권고한다.
