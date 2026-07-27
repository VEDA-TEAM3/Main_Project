# Sanitizer 계층 보안·성능 권고 (Security & Performance Advisory)

> **대상 모듈**
> - 인터페이스: `include/interfaces/IObjectSanitizer.h`
> - 구현체: `src/sanitize/ContainmentSanitizer.h`, `.cpp`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | Sanitizer 계층 메모리 안전성, 시간 복잡도(O(N^2)) 성능 병목 방어 및 기하학적 예외 검증 명세 |

---

## 1. 위협 모델 (Threat Model)

Sanitizer는 파서가 만든 객체 목록에서 **팬텀(중복) 객체를 제거**하는 순수 계산 계층이다. 소켓도, 파일도, 외부 I/O도 만지지 않는다. 따라서 위협은 **입력 데이터의 형태**로만 들어온다.

입력은 **여전히 신뢰 불가**하다. — 카메라가 통제하는 bbox 좌표와 객체 개수가 그대로 흘러들어오기 때문이다. (파서의 검증을 통과한 뒤)

| 위협 범주 | 시나리오 | 영향 |
|-----------|----------|------|
| **CPU 고갈 (bbox 홍수)** | 객체를 대량으로 실어보내 O(N²) 비교를 폭발시킴 | 파이프라인 지연 급증 → 실시간성 상실 |
| **Fail-open 우회** | 상한을 **일부러 넘겨** sanitize 자체를 건너뛰게 만듦 | 팬텀 제거 무력화 → 중복 객체가 하류로 유입 |
| **기하학적 이상값** | 역전(`l>r`, `t>b`)·음수·0 면적·NaN bbox 주입 | 0 나눗셈, 잘못된 판정, 정상 객체의 오삭제 |
| **설정 오염** | 임계값(`iouThresh`/`containThresh`)이 비정상 범위 | **정상 위험 객체의 대량 삭제** → 경보 미발생 |

### 판정 오류의 두 방향 — 어느 쪽이 위험한가

이 계층의 결함을 평가할 때 **실패 방향**이 등급을 좌우한다.

- **과잉 검출(fail-open, 안전 방향)**: 팬텀을 못 지움 → 객체가 실제보다 많아짐 → 경보가 더 울림 <br> 성가시지만 **놓치지는 않는다.**
- **과소 검출(fail-closed, 위험 방향)**: 실제 위험 객체를 지움 → 사람/차량이 사라짐 → **경보가 울리지 않는다.**

이 시스템은 위험 판정 시스템이므로 **과소 검출이 훨씬 치명적**이다. 이 원칙이 `kMaxObjectsPerFrame` 초과 시 **fail-open**을 택한 설계 근거이며, 아래 W2(임계값 미검증)를 높게 평가하는 이유이기도 하다.

---

## 2. 메모리 & 스택 안전성 (Memory & Stack Safety)

### 2.1 ✅ `std::bitset<128>`은 힙 할당이 구조적으로 불가능하다

```cpp
constexpr std::size_t kMaxObjectsPerFrame = 256;   // W1 패치로 128 -> 256 (파서와 정렬)
...
std::bitset<kMaxObjectsPerFrame> drop;   // 스택 32바이트 (256비트 = uint64 4개)
```

`std::bitset<N>`은 **N이 컴파일타임 상수인 고정 크기 타입**이며, 표준상 **할당자(allocator)를 갖지 않는다.** 내부는 `unsigned long` 배열이므로 어떤 상황에서도 힙을 건드릴 수 없다. `std::vector<bool>`(프레임마다 힙 할당)을 대체한 목적이 정확히 이것이다.

- 크기: 256비트 = **32바이트** — W1 패치로 16바이트에서 늘었으나 여전히 스택 부담이 사실상 없다.
- **✅ 결론: 완전히 힙 할당에서 자유롭다.** (`std::vector<bool>`이었다면 프레임당 1회 할당/해제가 발생)
- **상한을 키워도 OOM 방어가 훼손되지 않는 이유**: 크기가 **컴파일타임에 고정**되므로 상한을 올려도 "런타임에 커질 수 있는 버퍼"가 되지 않는다. 128 → 256은 스택 상수 16바이트 증가일 뿐이다.

### 2.2 ✅ 2단계 in-place 압축도 할당이 없다 — 3중 확인

```cpp
size_t writeIdx = 0;
for (size_t i = 0; i < n; ++i) {
    if (drop[i]) {
        continue;
    }
    if (writeIdx != i) {
        frame.objects[writeIdx] = std::move(frame.objects[i]);
    }
    ++writeIdx;
}
frame.objects.resize(writeIdx);
```

| 확인 항목 | 결과 |
|-----------|------|
| **① 새 벡터를 만들지 않는가** | ✅ 별도 컨테이너 없이 `frame.objects` 자체에서 앞으로 당긴다 |
| **② `std::move` 대입이 할당을 유발하는가** | ✅ **아니다.** `domain::DetectedObject`는 `ObjectId`, `std::optional<ObjectId>`, `enum`, `NormBox`(double 4개), `bool` 2개로 구성되어 **힙 멤버가 하나도 없다.** → move 대입 = 멤버별 복사, 할당 0 |
| **③ `resize(writeIdx)`가 재할당하는가** | ✅ **아니다.** `writeIdx <= n`이므로 **축소**이며, 축소 `resize`는 capacity를 유지하고 초과분만 파괴한다. |

### 2.3 ✅ 값 전달 시그니처가 복사를 유발하지 않는 이유 — 호출부와의 결합

인터페이스가 **값 전달·값 반환**이라 얼핏 프레임 전체 복사처럼 보인다.

```cpp
virtual domain::ChannelFrame sanitize(domain::ChannelFrame frame) = 0;   // 값 전달
```

**호출부를 확인한 결과 복사는 발생하지 않는다.**

```cpp
// Pipeline.cpp
domain::ChannelFrame frame = parser_->parse(raw);
frame = sanitizer_->sanitize(std::move(frame));   // ★ std::move -> 이동 생성
```

- 인자: `std::move(frame)` → 매개변수가 **이동 생성**된다. (벡터 버퍼 소유권 이전, 할당 0)
- 반환: `return frame;` — 매개변수를 값 반환하면 **암시적 이동**이 적용된다. (할당 0)
- 대입: `frame = ...` → **이동 대입** (할당 0)

> **⚠️ 취약한 결합 (I1)**: 이 무할당 속성은 **호출부의 `std::move`에 전적으로 의존**한다. 누군가 `sanitize(frame)`처럼 lvalue로 바꾸면 매개변수가 **복사 생성**되어 **프레임마다 힙 할당 + 전체 객체 복사**가 조용히 부활한다. 컴파일 오류도 경고도 나지 않는다. 성능 회귀가 소리 없이 들어올 수 있는 지점이다.

### 2.4 ✅ 스택 오버플로 — 구조적 면역

- **재귀가 전혀 없다.** 판정은 2중 평면 루프, 압축은 단일 평면 루프다.
- 헬퍼(`area`, `intersectionArea`, `iou`, `ioMin`)는 **모두 리프 함수**이며 지역 변수는 `double` 몇 개뿐이다.
- 지역 저장소 총량: `std::bitset<128>` 16바이트 + `size_t`/`double` 몇 개 → **O(1)**, 입력 크기 `N`과 무관
- **✅ 결론: 어떤 입력에도 스택 사용량이 증가하지 않는다.** 큰 지역 배열도, 가변 길이 배열도, 재귀도 없다.

---

## 3. 알고리즘 복잡도 & CPU DoS (Fail-open 정책)

### 3.1 O(N²)의 실제 비용 — 상한이 있으므로 폭발하지 않는다

판정 단계는 최악의 경우 모든 쌍을 검사한다.

```cpp
for (size_t i = 0; i < n; ++i) {          // 바깥
    if (!veda::isRiskClass(x.cls)) {
        continue;
    }
    for (size_t j = 0; j < n; ++j) { ... } // 안쪽 -> O(N²)
}
```

**최악 시나리오**: 모든 객체가 RISK 클래스이고 어떤 규칙에도 걸리지 않음(= 조기 `break` 없음).

| 항목 | 값 (W1 패치 반영) |
|------|-----|
| 최대 N | **256** (`kMaxObjectsPerFrame`, 파서와 정렬) |
| 최악 내부 반복 | 256 × 255 = **약 65,280회** |
| 쌍당 연산 | `iou`(교집합+면적 2회+나눗셈) + `ioMin`(교집합 재계산+면적 2회+나눗셈) ≈ **수십 flop** |
| 추정 소요 | 라즈베리파이 기준 **약 4~12 ms** (상한 128일 때의 약 4배) |
| 프레임 예산 | 5 fps → **200 ms** (실측 처리율 기준) |

**➡️ 최악 부하에서도 프레임 예산의 약 2~6% 수준이다.**

> **W1 패치의 비용 평가**: 상한을 128 → 256으로 올리면서 최악 계산량이 4배(N²)로 늘었지만, 절대값은 여전히 프레임 예산의 한 자릿수 퍼센트다. **팬텀 필터 우회 경로를 없애는 대가로 지불할 만한 비용**이며, "상수 천장" 성질은 그대로 유지된다.

### 3.2 ✅ 상한이 O(N²)을 "상수 천장"으로 바꾼다 — CPU DoS 불성립

핵심은 **N이 상한을 넘으면 비용이 커지는 게 아니라 오히려 O(1)로 떨어진다**는 점이다.

```cpp
if (n > kMaxObjectsPerFrame) {
    logError(...);      // 관측 가능하게 기록
    return frame;       // 즉시 반환 -> O(1)
}
```

공격자가 객체 수를 늘려 계산량을 키우려 해도 **129개째부터는 검사 자체가 수행되지 않는다.** 즉 **CPU 비용의 최댓값은 N=128에서 고정**되며, 그 이상 어떤 페이로드로도 초과할 수 없다.

> **➡️ 결론: 이 계층을 통한 CPU 고갈 공격(지연 급증)은 성립하지 않는다.** O(N²)이지만 N이 하드 상한을 가지므로 **실질적으로 상수 시간 상한**을 갖는다.

### 3.3 ✅ W1 — Fail-open 우회 (상한 불일치 사각지대) — **패치 완료**

> **상태: 조치 완료 (2026-07-27).** Sanitizer 상한을 파서와 정렬해 우회 경로를 제거했다.

#### 문제 (패치 이전)

**Fail-open 정책 자체는 올바른 선택이다.** (실패 방향 원칙: 위험 객체를 지우느니 팬텀을 남기는 편이 안전) 문제는 **정책이 발동하는 지점이 상류와 어긋나 있었다**는 것이다.

| 계층 | 상한 상수 | 패치 이전 | 패치 이후 |
|------|-----------|-----------|-----------|
| 파서 (`OnvifParser.cpp`) | `kMaxObjectsPerFrame` | 256 | **256** |
| Sanitizer (`ContainmentSanitizer.cpp`) | `kMaxObjectsPerFrame` | **128** ❌ | **256** ✅ |

파서는 최대 **256개**를 통과시키는데 Sanitizer는 **128개**를 넘으면 검사를 건너뛰었으므로, **객체 수 129~256 구간은 sanitize가 항상 생략되는 사각지대**였다.

**공격 시나리오**: 카메라가 프레임에 객체를 **129개**만 실어보내면 → `sanitize`가 즉시 반환 → **그 프레임의 팬텀 제거가 통째로 무력화**된다. 작은 `<tt:Object>` 몇십 개를 덧붙이는 것만으로 재현되는 저비용 우회였다.

#### 적용된 패치

Sanitizer의 상한을 **128 → 256**으로 올려 파서와 정확히 일치시켰다.

```cpp
/// @warning [W1] 이 값은 OnvifParser 의 동명 상수(kMaxObjectsPerFrame)와 **반드시 같아야 한다**.
///          파서가 256개까지 통과시키는데 여기가 128이면 129~256 구간이 'sanitize 가 항상 생략되는
///          사각지대'가 되어, 객체를 129개만 실어보내면 팬텀 필터를 통째로 우회할 수 있었다.
///          두 상한을 256으로 정렬해 그 우회 경로를 제거했다. 한쪽만 바꾸면 사각지대가 되살아난다.
constexpr std::size_t kMaxObjectsPerFrame = 256;
```

`drop` 마스크는 이미 `std::bitset<kMaxObjectsPerFrame>`으로 **상수에 바인딩**되어 있으므로, 상수 하나만 바꾸면 자동으로 `std::bitset<256>`이 된다. (별도 수정 불필요)

#### 패치의 효과

| 항목 | 결과 |
|------|------|
| **사각지대 제거** | 파서가 통과시킬 수 있는 **모든** 객체 수(0~256)에서 sanitize가 **항상 수행**된다. → 우회 경로 소멸 |
| **OOM 방어 유지** | `std::bitset`은 컴파일타임 고정 크기 → 스택 16 B → **32 B** 증가일 뿐, 런타임 가변 버퍼가 되지 않음 |
| **CPU 비용** | 최악 계산량 약 4배(16,256 → 65,280쌍) → **약 4~12 ms**, 프레임 예산의 2~6% |
| **fail-open 정책 유지** | 256을 넘는 비정상 폭주에서는 **여전히 스킵**한다. — 위험 객체를 지우느니 팬텀을 남기는 안전 원칙은 그대로 |

> **⚠️ 유지보수 주의**: 두 상수는 이름이 `kMaxObjectsPerFrame`으로 **동일**하고 서로 다른 TU의 익명 네임스페이스에 있어 컴파일러가 불일치를 잡아주지 못한다. **한쪽만 바꾸면 사각지대가 조용히 되살아난다.** 양쪽 파일 모두에 상호 참조 주석을 달아두었으니, 값을 조정할 때는 반드시 함께 수정할 것

### 3.4 ℹ️ W3 — 중복 계산 (최적화 여지)

무할당·무재귀는 지켜지지만, 내부 루프에 **불필요한 재계산**이 있다.

```cpp
// 규칙 B — area(x.box) 가 j 마다 다시 계산된다
if (x.cls == y.cls && area(x.box) < area(y.box) && ioMin(x.box, y.box) > containThresh_)
```

- **`area(x.box)`가 안쪽 루프마다 재계산**된다 → `i`당 O(N)회, 전체 **O(N²)회**. `i`당 1회로 호이스팅하면 O(N)회로 줄어든다.
- **`intersectionArea(x,y)`가 쌍당 최대 2회** 계산된다. (`iou`에서 1회, `ioMin`에서 다시 1회)
- `ioMin` 내부에서도 `area(a)`, `area(b)`를 또 계산한다.

**개선안(무할당 유지)**: 진입부에서 스택 배열로 면적을 1회 선계산.

```cpp
double areas[kMaxObjectsPerFrame];   // 1 KB, 스택 — 힙 할당 없음
for (size_t k = 0; k < n; ++k) areas[k] = area(frame.objects[k].box);
```

최악 부하에서 **약 2배 내외의 절감**이 기대되나, 현재도 프레임 예산의 1~2%에 불과하므로 **긴급도는 낮다.**

---

## 4. 기하학적 예외 검증 (Geometric Vulnerabilities)

이 계층은 **4개의 나눗셈**을 수행하므로(`iou`, `ioMin`), 이상 좌표가 0 나눗셈이나 오판정을 일으키는지가 핵심이다.

### 4.1 ✅ 역전 좌표 (`l > r`, `t > b`) — 안전

```cpp
double area(const domain::NormBox& box) {
    const double w = box.r - box.l;
    const double h = box.b - box.t;
    if (w <= 0.0 || h <= 0.0) return 0.0;   // ★ 역전/0폭 방어
    return w * h;
}
```

역전 bbox는 `w` 또는 `h`가 음수 → **면적 0**으로 처리된다. 음수 면적이 전파되어 IoU가 음수가 되는 일이 없다. ✅

`intersectionArea`도 동일하게 방어한다.

```cpp
if (r <= l || bt <= t) return 0.0;   // 겹침 없음/역전 -> 0
```

### 4.2 ✅ 0 면적 bbox — 나눗셈 방어 완비

**모든 나눗셈 경로에 분모 가드가 있다.**

```cpp
double iou(...) {
    const double uni = area(a) + area(b) - inter;
    if (uni <= 0.0) return 0.0;      // ★ 0 나눗셈 차단
    return inter / uni;
}

double ioMin(...) {
    const double minArea = std::min(area(a), area(b));
    if (minArea <= 0.0) return 0.0;  // ★ 0 나눗셈 차단
    return inter / minArea;
}
```

- 0 면적 객체는 항상 `0.0`을 반환받고, `0.0 > iouThresh_`(양수)는 거짓이므로 **어떤 규칙에도 걸리지 않고 그대로 통과**한다. ✅
- **0 나눗셈·`inf` 생성 경로가 존재하지 않는다.** ✅

### 4.3 ✅ 음수·범위 밖 좌표 — 수학적으로 안전

IoU/IoMin은 **평행이동 불변**이므로 좌표가 `[0,1]` 밖이거나 음수여도 계산이 성립한다. 크래시나 UB가 없다. ✅

> 파서의 `normX`/`normY`는 ONVIF `Transformation` 값에 따라 `[0,1]`을 벗어난 값을 낼 수 있으므로, **범위 밖 좌표는 정상적으로 도달 가능한 입력**이다. 이 계층이 이를 감내하도록 작성된 것은 적절하다.

### 4.4 ℹ️ NaN 거동 — 크래시는 없으나 상류 의존

`NaN`이 도달하면 어떻게 되는지 추적했다. **결론부터: 크래시·UB는 없고 fail-open으로 통과한다.**

- `w = NaN`일 때 `if (w <= 0.0 || h <= 0.0)` — **NaN 비교는 모두 false**이므로 가드를 통과해 `return w * h;` = **NaN**을 반환한다. (0이 아니다)
- 그 NaN이 `uni`로 전파되고 `if (uni <= 0.0)`도 false → `inter / uni` = NaN
- 최종 판정 `NaN > iouThresh_` → **false** → 규칙이 발동하지 않음
- **➡️ NaN 객체는 절대 제거되지 않고 그대로 통과한다.** (과잉 검출 = 안전 방향)

**현재 위험 없음**: 상류 파서가 `W2` 패치로 `std::isfinite` 검사를 수행해 NaN/Inf 좌표 객체를 이미 폐기하므로, 정상 경로에서 NaN은 이 계층에 도달하지 않는다.

> **ℹ️ 계층 결합 기록**: Sanitizer의 가드는 `<= 0.0` 형태라 **NaN을 0으로 정규화하지 않는다.** 즉 NaN 방어는 **파서에 의존**한다. 파서의 `isfinite` 검사가 제거되면 이 계층은 NaN을 조용히 통과시킨다. (크래시는 여전히 없음)

### 4.5 ✅ W2 — 임계값 미검증 (유일한 "위험 방향" 실패 경로) — **패치 완료**

> **상태: 조치 완료 (2026-07-27).** 생성자 검증을 추가해 잘못된 설정이 조립 시점에 즉시 실패하도록 했다.

#### 문제 (패치 이전)

```cpp
ContainmentSanitizer::ContainmentSanitizer(double iouThresh, double containThresh)
    : iouThresh_(iouThresh), containThresh_(containThresh) {}   // 검증 없음 ❌
```

`AppConfig`도 이 두 값을 **클램프하지 않는다.** (정수 항목에 쓰이는 `clampPositive` 같은 방어가 없다) 따라서 음수/1 초과 값이 그대로 흘러들어왔다.

**음수 임계값의 파급 효과 (심각)**: `iou()`와 `ioMin()`은 항상 `>= 0`을 반환한다. 따라서 `iouThresh_`가 **음수**면:

```cpp
if (veda::isBlurClass(y.cls) && iou(x.box, y.box) > iouThresh_)   // 0.0 > -0.1  ==  true
```

**겹치지 않아도 조건이 참이 된다.** 결과적으로 프레임에 **Head/LicensePlate가 단 하나라도 존재하면 모든 Human/Vehicle이 삭제**된다. → 위험 객체 전멸 → **경보 미발생**. **과소 검출(위험 방향) 실패**이며, 이 계층에서 유일하게 위험 방향으로 실패하는 경로였다.

| 설정값 | 패치 이전 거동 | 방향 |
|--------|----------------|------|
| 정상 `[0, 1]` | 의도대로 동작 | — |
| **음수** | 겹침과 무관하게 규칙 발동 → **위험 객체 대량 삭제** | 🔴 **위험 방향** |
| `> 1.0` | IoU는 1을 넘을 수 없어 규칙이 절대 발동 안 함 → 필터 무력화 | 🟢 안전 방향 |
| `NaN` | 모든 비교가 false → 아무것도 안 지움 | 🟢 안전 방향 |

#### 적용된 패치

생성자에서 두 임계값의 범위를 검증하고, 벗어나면 `std::invalid_argument`를 던진다.

```cpp
ContainmentSanitizer::ContainmentSanitizer(double iouThresh, double containThresh)
    : iouThresh_(iouThresh), containThresh_(containThresh) {
    // [W2] 임계값을 조립 시점에 검증한다 (설정 오류는 조용히 넘기지 않고 즉시 실패).
    //
    // iou()/ioMin() 은 항상 0 이상을 반환하므로, 임계값이 음수면 "0.0 > -0.1" 이 참이 되어
    // 겹치지도 않은 객체에까지 규칙이 발동한다 -> 프레임에 Head/LicensePlate 가 하나만 있어도
    // 모든 Human/Vehicle 이 삭제되고, 그 결과 위험 객체가 사라져 경보가 울리지 않는다.
    // 즉 '과소 검출(위험 방향)'으로 조용히 실패하는 유일한 경로였다.
    // 1.0 초과도 규칙이 절대 발동하지 않게 만들어 필터를 무력화하므로 함께 막는다.
    //
    // HomographyTransform / AffineImageCoordinateMapper 와 동일한 규약: 구조적으로 잘못된
    // 설정은 생성자가 던지고 main 이 잡아 프로세스를 종료한다 (CLAUDE.md: 조용히 틀린 값을
    // 내보내느니 즉시 죽는 편이 안전하다).
    if (iouThresh_ < 0.0 || iouThresh_ > 1.0) {
        throw std::invalid_argument("sanitizerIouThresh must be within [0.0, 1.0] (got " +
                                    std::to_string(iouThresh_) + ") - check config.json");
    }
    if (containThresh_ < 0.0 || containThresh_ > 1.0) {
        throw std::invalid_argument("sanitizerContainThresh must be within [0.0, 1.0] (got " +
                                    std::to_string(containThresh_) + ") - check config.json");
    }
}
```

#### 왜 이것이 조용한 오탐지(false negative)를 막는가

패치의 핵심은 **실패 시점을 "런타임의 조용한 오작동"에서 "기동 시점의 시끄러운 중단"으로 옮긴 것**이다.

- **패치 이전**: `config.json`에 `sanitizerIouThresh: -0.1` 오타가 있으면 프로세스는 **정상 기동**하고, 로그도 평온하며, MQTT로 프레임도 계속 발행된다. 다만 **위험 객체만 조용히 사라진다.** 운영자 관점에서는 "시스템이 잘 돌아가는데 경보가 안 울리는" 상태 — 안전 시스템에서 **가장 위험한 실패 양상**이다.
- **패치 이후**: 같은 오타에서 `AppContext` 생성자가 `std::invalid_argument`를 던지고, `main`이 이를 잡아 **원인을 로그에 남기고 즉시 종료**한다. 잘못된 설정으로는 **단 한 프레임도 발행되지 않는다.**

```cpp
// main.cpp — 기존 조립 오류 처리 경로가 그대로 이 예외를 받는다
try {
    context = std::make_unique<AppContext>(config);
} catch (const std::exception& error) {
    logError(kIface, std::string("초기화 실패 - config.json 설정을 확인하세요: ") + error.what());
    return 1;
}
```

오류 메시지에 **위반한 키 이름과 실제 값**을 담아(`sanitizerIouThresh must be within [0.0, 1.0] (got -0.100000)`) 운영자가 즉시 교정할 수 있게 했다.

#### 검증 결과

전용 하네스로 생성자 거동을 직접 확인했다.

| 입력 | 결과 |
|------|------|
| `(0.5, 0.9)` 정상 | ✅ accepted |
| `(0.0, 1.0)` 경계값 | ✅ accepted (경계 포함) |
| `iouThresh = -0.1` | ✅ **THREW** — `sanitizerIouThresh must be within [0.0, 1.0] (got -0.100000)` |
| `containThresh = -0.1` | ✅ **THREW** — `sanitizerContainThresh must be within [0.0, 1.0] (got -0.100000)` |
| `iouThresh = 1.5` | ✅ **THREW** |
| `containThresh = 2.0` | ✅ **THREW** |
| `iouThresh = NaN` | ⚠️ accepted (아래 잔여 항목 참고) |

> **ℹ️ 잔여 항목 (I3) — NaN은 이 검증을 통과한다.** `NaN < 0.0`과 `NaN > 1.0`은 **둘 다 false**이므로 현재 조건식으로는 걸러지지 않는다. 다만 분석했듯 NaN 임계값의 거동은 **모든 비교가 false → 아무것도 제거하지 않음**, 즉 **fail-open(안전 방향)** 이므로 경보 누락으로 이어지지 않는다. 완전한 방어를 원한다면 `AffineImageCoordinateMapper`처럼 `!std::isfinite(...)` 검사를 조건에 추가하면 된다.

---

## 5. 성능 최적화 (In-place 배열 압축)

### 5.1 ✅ 판정과 변형의 분리 — 정확성의 핵심

```cpp
// 1단계: 판정만 (frame.objects 를 절대 건드리지 않음)
for (...) { ... drop[i] = true; ... }

// 2단계: 판정 완료 후에만 in-place 압축
```

`i`의 판정은 **다른 모든 `j`의 "원본" bbox**를 참조한다. 만약 판정 도중 원소를 지우거나 앞으로 당기면, 이후 판정이 **이미 이동된 원소**를 보게 되어 결과가 입력 순서에 따라 달라진다. 2단계 분리는 성능 기법이 아니라 **정확성 요구사항**이다. ✅

### 5.2 ✅ in-place 압축의 이점

| 항목 | 효과 |
|------|------|
| 별도 결과 벡터 없음 | 힙 할당 0 |
| `std::move` 대입 | `DetectedObject`에 힙 멤버가 없어 멤버별 복사 (실질 memcpy) |
| 축소 `resize` | 재할당 없음, capacity 보존 → **다음 프레임에서도 재사용** |
| `writeIdx != i` 검사 | 드랍이 없는 정상 프레임에서는 **대입 자체가 일어나지 않음** (가장 흔한 경로가 가장 저렴) |

특히 마지막 항목이 중요하다 — 팬텀이 없는 **정상 프레임에서는 2단계가 사실상 인덱스 순회만** 하고 끝난다.

### 5.3 ✅ 로그 비용 차단

```cpp
if (isLogEnabled(LogLevel::Debug)) {
    logDebug(kIface, "ch=" + std::to_string(...) + ...);   // 문자열 조립도 가드 안쪽
}
```

제거 로그는 프레임마다 발생하는 **정상 동작**이므로 `Debug` 레벨이며, **문자열 조립 자체가 레벨 가드 안**에 있다. 운영(Info 이상)에서는 `std::string` 연결·할당이 **아예 실행되지 않는다.** ✅

> 반면 상한 초과 로그(`logError`)는 **비정상 상황에서만** 발생하므로 무조건 출력해도 무해하며, W1 우회 시도를 **관측 가능**하게 만드는 유일한 신호다.

---

## 6. 감사 요약

| 영역 | 판정 | 근거 |
|------|------|------|
| **알고리즘 복잡도 / CPU DoS** | ✅ **All Clear** | O(N²)이지만 N≤256 하드 상한으로 **상수 천장**(최악 ~65K쌍, 프레임 예산의 2~6%) <br> 상한 초과 시 O(1) 즉시 반환이라 계산량 증폭 불가 |
| **OOM / 메모리 할당** | ✅ **All Clear** | `std::bitset<256>`은 할당자 없는 스택 타입(32B), `DetectedObject`에 힙 멤버 없어 move가 memcpy, 축소 `resize`는 재할당 없음 → **완전 무할당 확인** |
| **스택 오버플로** | ✅ **All Clear** (구조적 면역) | 재귀 없음, 지역 저장소 O(1)(입력과 무관), 큰 지역 배열·VLA 없음 |
| **기하학적 예외** | ✅ **All Clear** | 역전·0면적·음수·범위 밖 좌표 모두 방어 <br> **4개 나눗셈 전부 분모 가드 보유** → 0 나눗셈 경로 없음 <br> 임계값은 W2 패치로 조립 시점 검증됨 |

### 지적 사항 목록

| # | 항목 | 등급 | 실패 방향 | 상태 |
|---|------|------|-----------|------|
| **W2** | 임계값 미검증 (음수 허용) | Warning (최우선) | 🔴 위험(과소 검출) | ✅ **패치 완료** — 생성자에서 `[0,1]` 검증 후 `std::invalid_argument` |
| **W1** | Fail-open 우회 (상한 불일치 사각지대) | Warning | 🟢 안전(과잉 검출) | ✅ **패치 완료** — `kMaxObjectsPerFrame` 128 → **256**으로 파서와 정렬  |
| **W3** | 내부 루프 중복 계산 (`area`/`intersectionArea`) | ℹ️ Info | — | ⬜ 미조치 — 최악 부하에서 ~2배 절감 여지 (현재 예산의 2~6%라 긴급도 낮음) |
| **I1** | 무할당이 호출부 `std::move`에 의존 | ℹ️ Info | — | ⬜ 미조치 — lvalue로 바뀌면 **프레임당 복사+할당이 조용히 부활** (경고 없음) |
| **I2** | NaN 좌표 방어가 상류 파서에 의존 | ℹ️ Info | 🟢 안전 | ⬜ 미조치 — 현재는 파서 `isfinite`가 차단 |
| **I3** | NaN **임계값**은 생성자 검증을 통과함 | ℹ️ Info | 🟢 안전 | ⬜ 미조치 — `NaN<0`·`NaN>1` 모두 false <br> 거동은 fail-open (아무것도 안 지움) `!std::isfinite` 추가로 해소 가능 |

**총평**: Sanitizer 계층은 **메모리·스택 안전성과 기하학적 예외 처리가 견고**하다. `std::bitset` + in-place 압축의 **완전 무할당 설계**와 **모든 나눗셈의 분모 가드**는 설계 의도대로 정확히 동작하며, O(N²)도 하드 상한 덕분에 CPU DoS로 발전하지 않는다. Critical 결함은 없었다.

감사에서 식별된 두 Warning은 **모두 패치되었다**:
- **W2** — 유일하게 위험 방향으로 실패하던 경로가 제거되어, 잘못된 임계값은 이제 **기동 시점에 프로세스를 중단**시킨다. (조용한 경보 누락 → 시끄러운 즉시 실패) 이로써 compute-server의 모든 계산 구현체가 동일한 fail-fast 규약을 갖는다.
- **W1** — 파서와 Sanitizer의 상한을 **256으로 정렬**해 팬텀 필터 우회 사각지대를 없앴다. 대가로 최악 CPU가 약 4배 늘었으나 프레임 예산의 한 자릿수 퍼센트에 머문다.

잔여 항목(W3/I1/I2/I3)은 모두 정보성이거나 안전 방향 실패이므로 선택적 개선 사항이다.