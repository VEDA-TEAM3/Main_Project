# Metric 레퍼런스

> **대상 파일**
> - 거리 포트: `include/interfaces/IDistanceMetric.h`
> - 현재 구현: `src/metric/EuclideanMetric.h`, `.cpp`
> - 소비자: `src/fuse/ConcatFuser.cpp`, `src/fuse/GridFuser.cpp`, `src/risk/ThresholdRiskPolicy.cpp`

Metric은 Control Server의 **거리 함수(distance function)** 계층이다. 두 월드 좌표 사이의
물리적 거리(m)를 계산하는 것이 전부다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-30 | 1.0.0 | Mangjun | Metric을 거리 포트로 정의하고 hypot→sqrt 최적화 및 O(N²) 호출 특성 문서화 |

---

## 1. 거리 포트 계약

```cpp
class IDistanceMetric {
public:
    virtual ~IDistanceMetric() = default;
    virtual double calculate(const domain::WorldPoint& p1, const domain::WorldPoint& p2) const = 0;
};
```

인터페이스는 이 한 함수가 전부다.

| 계약 | 내용 |
|------|------|
| 입력 | **월드 좌표** 2개 (`domain::WorldPoint`, 단위 m) |
| 출력 | 물리적 거리(m) (음수 아님) |
| 상태 | **무상태** — `const` 이며 가변 멤버 없음 |
| 스레드 안전 | 락 없이 공유 가능 |
| 할당 | 호출당 힙 할당 0 |
| 예외 | 던지지 않음 |
| 호출 빈도 | **O(N²)** |

### 1.1 ⚠️ 입력은 반드시 월드 좌표

`WorldPoint` 와 `LocalPoint` 는 의도적으로 다른 타입이다. 카메라 로컬 좌표를 넣으면
컴파일이 실패한다 — 서로 다른 카메라의 로컬 좌표 사이 거리는 물리적 의미가 없기 때문이다.
변환 책임은 `ILocalToWorldTransform` 에 있다.

### 1.2 입력 유효성은 검사하지 않는다

`calculate()` 는 좌표의 유한성을 검사하지 **않는다.** O(N²) 경로에 분기를 넣는 대신
상류(O(N))에서 걸러내는 것이 옳기 때문이다. 비유한 좌표가 들어오면 결과도 비유한이 되며, 그 결과는 조용한 판정 실패로 이어진다.

## 2. 현재 구현 — `EuclideanMetric`

```cpp
double EuclideanMetric::calculate(const domain::WorldPoint& p1, const domain::WorldPoint& p2) const {
    const double dx = p1.x - p2.x;
    const double dy = p1.y - p2.y;
    return std::sqrt(dx * dx + dy * dy);
}
```

평면 유클리드 거리다. 고도(z)는 다루지 않는다. — 주차장/교차로 평면 도면 위의 2D 문제다.

### 2.1 ⚠️ `std::hypot` 을 쓰지 않는 이유

`hypot` 은 `dx*dx` 가 `double` 범위를 넘거나 언더플로하는 극단값에서도 정확하도록 스케일링을
거치는 **libm 함수 호출**이다. 그 보호가 이 도메인에서는 불필요하다.

| 항목 | 값 |
|------|-----|
| 좌표 범위 | `worldBounds` 안의 미터 값 — \|dx\| 최대 10⁴ 수준 |
| `dx*dx` 최대 | 약 10⁸ |
| `double` 상한 | 1.8 × 10³⁰⁸ |
| 오버플로 여유 | **300 자릿수** |
| 언더플로 임계 | \|dx\| < 1e-154 — 미터 단위 충돌 임계값에 무의미 |

반면 비용 차이는 크다. `sqrt` 는 하드웨어 명령 하나지만 `hypot` 은 함수 호출이다.

**실측(x86, N=200 = 19,900쌍): `hypot` 216.57 µs → `sqrt` 41.93 µs (5.17배).**

> **⚠️ 회귀 주의**: 정확도가 더 좋다는 이유로 `hypot`으로 되돌리면 O(N²) 경로 전체가 5배 느려진다. 이 도메인에서 두 함수의 결과 차이는 관측되지 않는다.

## 3. 호출 특성 — O(N²)가 이 계층의 전부다

`calculate()` 자체는 나눗셈 없는 산술 몇 개다. 그러나 **모든 호출 지점이 이중 루프 안**이라,
호출당 비용이 그대로 제곱으로 증폭된다.

| 호출 지점 | 루프 | 목적 | 결과 사용법 |
|-----------|------|------|-------------|
| `ConcatFuser.cpp:115` | 후보 전체 쌍 O(N²) | 중복 병합 판정 | `dist > dedupMergeDistance_` |
| `ConcatFuser.cpp:193` | 융합 객체 × 트랙 | 트랙 매칭 | `dist > trackMaxDistance_` |
| `ConcatFuser.cpp:219` | 융합 객체 × 트랙 | 최근접 트랙 | `dist < minDist` |
| `GridFuser.cpp:136` | 공간 해시 근접쌍 O(N·k) | 중복 병합 판정 | `dist > dedupMergeDistance_` |
| `GridFuser.cpp:225/247` | 융합 객체 × 트랙 | 트랙 매칭 | 위와 동일 |
| `ThresholdRiskPolicy.cpp:49` | 차량 × 전체 객체 | 최근접 거리 | `dist < minDist`, 임계 비교 |

`GridFuser` 가 브로드페이즈를 O(N·k) 로 줄이지만, **`calculate()` 호출당 비용을 줄이는 것과는
독립**이다. 두 최적화는 곱해진다.

### 3.1 제곱거리 최적화를 도입하지 않은 이유

호출 지점 6곳 중 4곳이 **임계값 비교만** 한다. (`dist > threshold`) 이론적으로는 `calculateSquared()`를 두고 임계값도 제곱해 비교하면 `sqrt`를 완전히 없앨 수 있다.

**측정해 보니 이득이 없었다.**

| 방식 | N=200 (19,900쌍) |
|------|------------------|
| `sqrt` (가상 호출) | 41.93 µs |
| 제곱거리 (직접 호출, sqrt 없음) | 41.79 µs |
| **차이** | **0.3%** |

`sqrt` 는 최신 CPU에서 파이프라인된 단일 명령이라, 제거해도 루프의 나머지 비용에 묻힌다.
**인터페이스를 둘로 쪼개고 6개 호출 지점의 임계값을 제곱 도메인으로 옮기는 복잡도를
0.3% 와 맞바꿀 이유가 없다.** 임계값 제곱을 빠뜨린 호출 지점 하나가 조용한 오판정이 된다는
위험까지 감안하면 순손실이다.

### 3.2 가상 디스패치를 제거하지 않은 이유

| 방식 | N=50 | N=200 |
|------|------|-------|
| 가상 호출 | 3.27 µs | 41.93 µs |
| 직접 호출 | 2.88 µs | 42.93 µs |
| 차이 | 1.14배 | **차이 없음** |

N이 커질수록 분기 예측기가 단일 구현을 학습해 간접 호출 비용이 사라진다. **DI가 주는
교체 가능성을 포기할 만한 이득이 아니다.**

## 4. 다른 거리 함수로 교체하기

`IDistanceMetric` 을 구현하고 `AppContext` 의 주입 대상만 바꾼다. 소비자는 인터페이스만
알고 있다.

교체 시 **반드시 같이 재검토해야 하는 값**이 있다. 모든 미터 임계값이 이 거리 정의를
전제로 조정되어 있기 때문이다.

- `dedupMergeDistance` (융합 중복 판정)
- `trackMaxDistance` (트랙 연속성)
- `warningDistance` / `dangerousDistance` (위험 판정)

예컨대 맨해튼 거리로 바꾸면 같은 물리적 배치에서 거리가 최대 √2배 커져, **모든 임계값이
사실상 좁아진다.** 컴파일 오류도 런타임 오류도 없이 경보 민감도만 바뀐다.

가능한 구현 예:

- `ManhattanMetric` — 격자형 통로 기준 이동 거리
- `WeightedMetric` — 진행 방향 가중 (전방 위험을 측방보다 크게)
- `EllipticalMetric` — 차량 진행축 기준 타원 거리

## 5. 테스트 하네스

무상태·순수 함수라 하네스가 필요 없다. 값만 검증한다.

```cpp
EuclideanMetric m;
domain::WorldPoint a{0.0, 0.0};
domain::WorldPoint b{3.0, 4.0};
assert(m.calculate(a, b) == 5.0);        // 3-4-5 삼각형
assert(m.calculate(a, a) == 0.0);        // 동일점
assert(m.calculate(a, b) == m.calculate(b, a));  // 대칭성
```

검증할 항목:

- **동일성**: `d(a,a) == 0`
- **대칭성**: `d(a,b) == d(b,a)`
- **삼각부등식**: `d(a,c) <= d(a,b) + d(b,c)`
- **정확도**: 3-4-5 등 정수해가 정확히 나오는지
- **비유한 입력**: NaN 좌표에서 NaN이 나오는지 (계약 확인 — 방어는 상류 책임)

## 6. 확장 체크리스트

- [ ] `IDistanceMetric` 만 구현하고 소비자를 수정하지 않았는가
- [ ] `const` 이고 가변 상태가 없는가 (락 없이 공유 가능한가)
- [ ] 호출당 힙 할당이 0인가
- [ ] O(N²) 경로에 들어갈 만큼 호출 비용이 충분히 낮은가 (실측했는가)
- [ ] 거리 정의 변경에 맞춰 4개 미터 임계값을 재조정했는가
- [ ] 대칭성·삼각부등식을 만족하는가 (융합의 클러스터링이 이를 전제한다)

## 7. 현재 구조의 개선 과제

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| 2D 고정 | 고도 무시 | 다층 주차장 지원 시 `WorldPoint` 확장 필요 |
| 임계값 결합 | 거리 정의와 4개 임계값이 암묵적으로 결합 | 구현 교체 시 임계값 재조정을 강제할 방법 없음 — 문서 의존 |

인터페이스는 작게 유지한다. 제곱거리·배치 계산·SIMD 진입점을 `IDistanceMetric` 에 추가하지 않는다. — 3.1에서 측정했듯 이득이 없고, 호출 지점마다 다른 규약을 만든다.
