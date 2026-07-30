# Risk 레퍼런스

> **대상 파일**
> - 판정 포트: `include/interfaces/IRiskPolicy.h`
> - 현재 구현: `src/risk/ThresholdRiskPolicy.h`, `.cpp`
> - 거리 계산: `include/interfaces/IDistanceMetric.h`
> - 결과 타입: `include/domain/RiskEvaluation.h`, `include/domain/WorldObject.h`

Risk는 Control Server 파이프라인의 **판정 계층(decision layer)** 이다. 융합·존 배정이 끝난
월드 프레임을 받아 "지금 이 순간 어느 채널이 얼마나 위험한가"를 확정한다.

이 계층의 출력은 **하드웨어를 직접 움직인다.** `RiskEvaluation::zoneLevels` 가 그대로
STM32로 나가 사이렌·부저·LED를 켠다. 따라서 이 계층의 조용한 실패는 곧 **경보 미발생**이다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-30 | 1.0.0 | Mangjun | Risk를 out-parameter 무할당 판정 포트로 재정의하고 비유한 좌표 사전 차단·인덱스 기반 최근접 계약 문서화 |

---

## 1. 판정 포트 계약

```cpp
class IRiskPolicy {
public:
    virtual ~IRiskPolicy() = default;
    virtual void evaluate(domain::WorldFrame& frame, domain::RiskEvaluation& out) = 0;
};
```

| 계약 | 내용 |
|------|------|
| 입력 | `WorldFrame` (월드 좌표, `zoneId` 배정 완료) |
| 출력 ① | `out` — 채널별 위험도 (`zoneLevels`) |
| 출력 ② | `frame` — 각 객체의 `riskLevel`/`nearestObj`/`nearestDist`, 그리고 `frame.level` |
| 사전 조건 | `IZoneMapper` 가 `zoneId` 를 이미 배정했을 것 |
| 스레드 안전 | **아님** — `processPipeline` 단일 스레드 전용 (rate-limit 카운터가 가변) |
| 할당 | 프레임당 힙 할당 0 |

### 1.1 ⚠️ 반환이 아니라 out-parameter

```cpp
// 이전
virtual domain::RiskEvaluation evaluate(domain::WorldFrame& frame) = 0;
```

값 반환이면 `zoneLevels` 벡터가 **매 프레임 새로 할당**된다. (초당 10회) out-parameter로 바꾸면 호출자가 같은 버퍼를 재사용하므로 `resize()`가 no-op이 되어 할당이 사라진다.

> **구현체는 `out.zoneLevels` 를 `resize` 로 맞추기만 하고 새로 만들지 않아야 한다.**
> 값 반환으로 되돌리면 이 성질이 조용히 사라진다. —
> `IObjectRouter::route` / `ILocalToWorldTransform::transform` 과 동일한 규약이다.

호출자(`Controller`)는 `riskEval_` 를 멤버로 들고 있다.

## 2. 위험 판정 5원칙

이 5개가 이 계층의 사양 전부다. 하나라도 어기면 UI와 HW가 서로 다른 값을 보게 된다.

| # | 원칙 | 구현 위치 |
|---|------|-----------|
| 1 | **차량이 없으면 위험도 없음** — 사람만 있으면 전부 `None` | 외부 루프가 `cls != Vehicle` 을 skip |
| 2 | 거리는 **차량 기준으로만** 잰다. (사람↔사람은 계산 안 함) | 외부 루프가 차량, 내부 루프가 전체 |
| 3 | 판정된 레벨을 **차량과 최근접 객체 양쪽**에 부여 (올리기만) | `if (vehicle.riskLevel > nearest.riskLevel)` |
| 4 | **UI == HW 단일 진실 공급원** — `frame.level = max(zoneLevels)` | 함수 말미 |
| 5 | 채널 위험도 = 그 채널 **차량들의** 레벨 max (전파된 사람 레벨 제외) | zone 집계가 차량 루프 안에만 존재 |

### 2.1 원칙 4가 깨지는 방식

`MqttTransport::fillRiskFrame`은 `frame.level`을 **그대로 복사**하며 재계산하지 않는다.
만약 sink가 객체들을 순회해 max를 다시 구하면, 원칙 5에 의해 zone 집계에서 제외된
"사람에게 전파된 레벨"까지 섞여 **UI가 HW보다 높은 위험도를 표시**하게 된다.
컴파일 오류는 나지 않는다.

## 3. 판정 알고리즘

```text
[1단계] O(N) 초기화 · 유한성 검사 · 좌표 SoA 구축
   for each object:
       riskLevel/nearestObj/nearestDist 리셋
       좌표가 유한하지 않으면 → 판정에서 제외 (candIdx_ 에 넣지 않음)
       유한하면 → candPos_ / candIdx_ 에 적재

[2단계] V x N 최근접 탐색
   for each candidate k (차량만):
       for each candidate m (m != k):
           dist = metric_->calculate(...)      ← DI 유지
           최소값 갱신 (cmov 형태)
       임계 분류 → Danger / Warning / None
       Rule 3: nearest 에 레벨 전파 (인덱스 직접 접근)
       Rule 5: zone 집계

[3단계] Rule 4
   frame.level = max(zoneLevels)
```

### 3.1 유한성 검사를 1단계에 둔 이유

NaN은 `<` 와 `<=` **모두에서 false** 다. 이중 루프 안으로 들어오면:

- `dist < minDist` 가 false → 최근접으로 뽑히지 않음
- `minDist <= dangerousDistance_` 가 false → Danger 판정 안 됨

즉 그 객체와 얽힌 **차량이 판정에서 통째로 빠지고 경보가 조용히 사라진다.**

검사를 이중 루프 안에 넣으면 비용이 O(N²)가 된다. 1단계의 기존 초기화 루프에 얹으면
순회 횟수가 그대로 N이므로, **같은 방어를 N배 싸게** 산다.

### 3.2 좌표 SoA (`candPos_`)

`WorldObject` 는 gid·cls·riskLevel·nearestObj·nearestDist·zoneId·sourceChannels까지 안고 있어
약 80 B다. 최근접 탐색이 실제로 읽는 것은 `pos`(16 B)뿐인데, 객체 배열을 그대로 훑으면
캐시 라인마다 쓸모없는 64 B를 함께 끌어온다.

`candIdx_[k]`는 `candPos_[k]`가 `frame.objects`의 몇 번째인지를 돌려주는 역인덱스다.

> **인덱스만 두고 `frame.objects[candIdx_[m]].pos` 로 접근하는 변형도 측정했으나 더
> 느렸다.** (N=20에서 0.74x vs 0.92x)
> 간접 참조 비용이 좌표 복사 비용보다 컸다.

### 3.3 최근접 대상은 인덱스로 식별한다

```cpp
static constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;
```

이전 구현은 `veda::GlobalId nearestGid = 0` 을 "못 찾음" 센티널로 썼다. **`gid == 0` 인 객체가 실제로 존재**하므로, 그 객체가 최근접이면 못 찾은 것으로 오인되어 **경보가 사라졌다.**

인덱스는 부수 효과도 있다. Rule 3이 `gid` 로 전체를 재탐색하던 O(N) 루프가 사라졌다.

```cpp
// 이전: 위험 차량마다 O(N) 재탐색
for (auto& other : frame.objects)
    if (other.gid == nearestGid && vehicle.riskLevel > other.riskLevel) ...

// 현재: 직접 접근
if (vehicle.riskLevel > nearest.riskLevel) nearest.riskLevel = vehicle.riskLevel;
```

### 3.4 거리 계산은 `IDistanceMetric` 을 통한다

내부 루프에서 유클리드 거리를 인라인하면 5~10%를 얻을 수 있으나 **하지 않는다.**
거리 정의를 DI로 교체할 수 있다는 것이 이 계층의 계약이고, 인라인하면 그 seam이 죽는다.
측정상 가상 디스패치 비용은 N이 커질수록 사라진다.

## 4. 생성자 검증

```cpp
ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, const RiskConfig& risk, int channelCount);
```

| 인자 | 제약 | 위반 시 |
|------|------|---------|
| `metric` | non-null | `std::invalid_argument` |
| `channelCount` | `[1, 256]` | `std::invalid_argument` |
| `warningDistance` / `dangerousDistance` | 유한, 0 이상 | `std::invalid_argument` |
| 두 임계값 순서 | `dangerousDistance <= warningDistance` | `std::invalid_argument` |

**임계값 역전을 거부하는 이유**: `dangerous > warning` 이면 `minDist <= dangerous` 가 먼저
걸려 **Warning이 영영 도달 불가능**해진다. 과다 경보라 안전 방향이긴 하나, 운영자가 의도한
2단계 판정이 조용히 1단계가 된다.

**비유한 임계값을 거부하는 이유**: `minDist <= NaN` 은 항상 false다. Danger도 Warning도
성립하지 않아 **경보가 완전히 죽는다.**

## 5. 관측성

| 로그 | 조건 | rate-limit |
|------|------|------------|
| 위험 판정 (`logSuccess`) | Warning/Danger 확정 | 1회차 + 50회마다, `isLogEnabled` 가드 |
| 비유한 좌표 제외 (`logError`) | 1단계에서 폐기 발생 | 1회차 + 100회마다 |
| `zoneId` 범위 밖 (`logError`) | zone 집계 제외 | 없음 (설정 오류라 드묾) |

문자열 조립은 **rate-limit 을 통과한 뒤에** 한다. 인자로 넘기면 억제되는 경우에도
조립 비용을 전부 낸다 — `HomographyTransform` 과 같은 규약이다.

## 6. 테스트 하네스

```cpp
auto metric = std::make_shared<EuclideanMetric>();
RiskConfig rc; rc.warningDistance = 5.0; rc.dangerousDistance = 2.0;
ThresholdRiskPolicy policy(metric, rc, 12);

domain::RiskEvaluation out;
domain::WorldFrame frame;
// 차량 1대 + 1m 거리의 사람 → Danger
policy.evaluate(frame, out);
```

검증할 항목:

- 차량이 없으면 모든 레벨이 `None` (원칙 1)
- 사람↔사람 거리가 판정에 영향을 주지 않는지 (원칙 2)
- 최근접 객체에 레벨이 전파되되 **낮추지는 않는지** (원칙 3)
- `frame.level == max(zoneLevels)` (원칙 4)
- 전파된 사람 레벨이 `zoneLevels` 에 섞이지 않는지 (원칙 5)
- **경계**: `minDist` 가 임계값과 정확히 같을 때 (`<=` 이므로 포함)
- **NaN 좌표가 섞여도 나머지 객체 판정이 유지되는지**
- **최근접 객체의 `gid` 가 0이어도 판정되는지**
- 생성자가 null/범위 밖/비유한/역전 인자를 거부하는지
- 정상상태에서 프레임당 힙 할당이 0인지 (전역 `operator new` 계수)

## 7. 확장 체크리스트

- [ ] `IRiskPolicy` 만 구현하고 `Controller`를 수정하지 않았는가
- [ ] `out` 을 **재사용**하는가 (새로 만들지 않는가)
- [ ] 5원칙을 모두 지키는가 — 특히 원칙 4 (UI == HW)
- [ ] 비유한 좌표를 O(N) 단계에서 거르는가
- [ ] 핫패스 스크래치 버퍼가 멤버이고 `clear()`로 재사용되는가
- [ ] 생성자가 잘못된 임계값·채널 수를 거부하는가
- [ ] 로그 문자열을 rate-limit 통과 후에 조립하는가

## 8. 현재 구조의 개선 과제

| 항목 | 현재 상태 | 개선 방향 |
|------|-----------|-----------|
| 최근접 탐색 | V × N 브루트포스 | 객체 수가 늘면 공간 분할 도입 (`GridFuser` 와 동일한 접근) |
| 궤적 예측 | **없음** — 현재 위치만 본다 | 속도 벡터 기반 TTC(Time-To-Collision) 판정 검토 |
| 스레드 안전 | 아님 (rate-limit 카운터 가변) | `processPipeline` 단일 스레드 가정에 의존 |
| 임계값 결합 | 거리 정의 교체 시 재조정 필요 | 강제 수단 없음 — 문서 의존 |
| 테스트 | `tests/` 미등록 | control-server 테스트 범위 포함 시 이관 |

> **궤적 예측이 없다는 점은 사양이지 결함이 아니다.** 현재 판정은 "지금 이 순간의 거리"만
> 본다. 정지한 차량 옆 1 m의 보행자와 시속 40 km로 접근 중인 차량 앞 1 m의 보행자가 같은 `Danger`로 판정된다.
> TTC를 도입하려면 `WorldObject`에 속도 벡터가 필요하고,
> 그것은 융합 계층의 트래킹(`trackMaxDistance`)에서 파생되어야 한다.
