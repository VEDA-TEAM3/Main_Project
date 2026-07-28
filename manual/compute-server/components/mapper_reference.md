# Mapper 모듈 레퍼런스 (Blur 경로 이미지 좌표 매핑)

> **대상 파일**
> - 인터페이스: `include/interfaces/IImageCoordinateMapper.h`
> - 구현체: `src/mapper/AffineImageCoordinateMapper.h`, `.cpp`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-28 | 1.0.0 | Mangjun | IImageCoordinateMapper 인터페이스 및 Affine 이미지 좌표 매핑(blur 경로) 명세 |

---

Router 가 갈라놓은 **blur 분기의 좌표계를 앱 출력 좌표계로 맞추는 단계**다. 이 문서는 **blur 경로만** 다룬다. risk 경로의 호모그래피 변환(`ICoordinateTransform` / `HomographyTransform`)은 목적지도 수학도 완전히 다른 별개 계층이며 **[transform_reference.md](transform_reference.md)** 에서 다룬다.

| 구분 | **Mapper (이 문서)** | Transform (`transform_reference.md`) |
|------|----------------------|--------------------------------------|
| 경로 | **blur 전용** | **risk 전용** |
| 입력 | `DetectedObject` 목록의 bbox | 지면점 `ImagePoint` (정규화 `[0,1]`) |
| 출력 | 매핑된 bbox (**정규화 `[0,1]`**) | `veda::LocalPoint` (**미터**, 카메라 원점) |
| 수학 | 4개 스칼라 아핀 (`x·s + o`) | 3×3 원근 변환 + **원근 나눗셈** |
| 나눗셈 | **없음** | 있음 (0 분모 위험) |
| 실패 표현 | 원소 제거 (목록 축소) | `std::optional` → `nullopt` |
| 호출 빈도 | **프레임당 1회** (내부에서 객체 순회) | **객체당 1회** |

> **⚠️ 두 변환을 섞으면 안 된다.** 아핀 매핑의 목적지는 **앱이 영상 위에 블러 사각형을 그리는 좌표계**이고, 호모그래피는 **카메라 메타데이터 이미지 평면**에서 캘리브레이션된다. 아핀 매핑을 지면점 추출 *이전에* 적용하면 월드 좌표가 통째로 틀어진다. 기본값(`scale=1, offset=0`)이 항등이라 **증상이 보이지 않는 잠복 결함**이었고, 그래서 `Pipeline` 이 아핀 매핑을 **Router 뒤 blur 분기**로 옮겼다.

---

## 1. 존재 이유 — 정규화만으로는 해소되지 않는 화각 불일치

메타데이터 좌표계와 실제 송출되는 RTSP 프레임의 좌표계는 **같은 카메라에서 나오더라도** 서로 다른 화각(FOV)/crop/종횡비를 가질 수 있다.

두 좌표계 모두 `[0,1]` 로 정규화되어 있지만, **정규화는 "같은 화면을 본다"를 보장하지 않는다.** 예를 들어 메타데이터는 전체 센서 화각 기준인데 송출 스트림은 중앙을 crop 한 경우, 같은 `(0.5, 0.5)` 가 서로 다른 물리적 지점을 가리킨다. 이 어긋남을 보정하는 것이 이 계층의 유일한 책임이다.

**보정하지 않으면** 앱이 실제 얼굴/번호판에서 빗나간 위치에 블러 사각형을 그린다 — 개인정보가 그대로 노출된다.

---

## 2. 인터페이스 명세

```cpp
class IImageCoordinateMapper {
public:
    virtual ~IImageCoordinateMapper() = default;

    virtual void map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const = 0;
};
```

| 요소 | 계약 |
|------|------|
| `objects` | **in-out 파라미터.** 결과가 화면 밖이면 제거되므로 **크기가 줄 수 있다** |
| `channelId` | **진단 로그 전용.** 매핑 수식에는 관여하지 않는다 |
| 반환값 | **없음.** 새 벡터를 만들지 않는다 |
| `const` | 구현체는 **가변 상태를 갖지 않는다** (스레드 안전) |

### 2.1 in-place 변형을 택한 이유

반환값 대신 목록을 직접 고치고 잘라내는 형태이므로 **호출당 힙 할당이 0**이다. 이는 compute-server 의 per-frame 무할당 원칙(CLAUDE.md)의 일부이며, `IObjectRouter::route` 의 out-parameter 계약과 같은 동기에서 나왔다.

### 2.2 경계 플래그를 건드리지 않는 이유

`touchesBorder` / `bottomTruncated` 는 **risk 경로가 쓰는 값**이고 **메타데이터 좌표계 기준으로만** 의미가 있다. 파서가 한 번만 판정하며, 이 계층은 읽지도 쓰지도 않는다.

clamp 이후에 재판정하면 "앱 화면 기준 경계"라는 **다른 의미**가 같은 필드에 섞여 들어간다. blur 경로의 소비자인 `veda::BlurTarget` 에는 경계 필드 자체가 없으므로 재판정할 이유도 없다.

---

## 3. `AffineImageCoordinateMapper` — 구현

### 3.1 수식

```
l' = l·scaleX + offsetX      r' = r·scaleX + offsetX
t' = t·scaleY + offsetY      b' = b·scaleY + offsetY
```

**나눗셈이 없다.** 0 분모, 발산, 지평선 같은 원근 변환 고유의 위험이 **구조적으로 존재하지 않는다.**

### 3.2 생성자 — fail-fast 검증

```cpp
AffineImageCoordinateMapper::AffineImageCoordinateMapper(double scaleX, double scaleY,
                                                         double offsetX, double offsetY)
    : scaleX_(scaleX), scaleY_(scaleY), offsetX_(offsetX), offsetY_(offsetY) {
    if (!isFinite(scaleX_) || !isFinite(scaleY_) || !isFinite(offsetX_) || !isFinite(offsetY_) ||
        scaleX_ <= 0.0 || scaleY_ <= 0.0) {
        throw std::invalid_argument("image coordinate mapper requires finite positive scales");
    }
}
```

| 검사 | 이유 |
|------|------|
| 4개 값의 **유한성** | `NaN`/`Inf` scale 은 모든 박스를 비유한으로 만들어 blur 를 전량 폐기시킨다 |
| scale > 0 (**0·음수 거부**) | 음수 scale 은 좌표계를 뒤집어 `l > r` 을 만든다. `scale = 0` 은 모든 박스를 한 점으로 붕괴시킨다 |

`main` 이 이 예외를 잡아 프로세스를 종료한다 — **설정이 잘못된 채로 조용히 빗나간 블러를 그리느니 즉시 죽는 편이 안전하다**는 프로젝트 규약을 따른다.

### 3.3 in-place 필터링 파이프라인

```cpp
std::size_t writeIdx = 0;
for (std::size_t readIdx = 0; readIdx < objects.size(); ++readIdx) {
    auto& object = objects[readIdx];

    domain::NormBox mapped;                       // ① 아핀 적용
    mapped.l = object.box.l * scaleX_ + offsetX_;
    mapped.r = object.box.r * scaleX_ + offsetX_;
    mapped.t = object.box.t * scaleY_ + offsetY_;
    mapped.b = object.box.b * scaleY_ + offsetY_;

    if (!isFinite(4개) || !isVisible(mapped)) continue;   // ② 유한성 + 가시성

    clampToOutput(mapped);                                // ③ [0,1] 클램프
    if (mapped.r <= mapped.l || mapped.b <= mapped.t) continue;  // ④ 퇴화 박스 제거

    object.box = mapped;
    if (writeIdx != readIdx) objects[writeIdx] = std::move(object);  // ⑤ 앞으로 당기기
    ++writeIdx;
}
objects.resize(writeIdx);                                 // ⑥ 축소 (재할당 없음)
```

| 단계 | 판정식 | 의미 |
|------|--------|------|
| ② 가시성 | `r > 0 && l < 1 && b > 0 && t < 1` | 출력 프레임과 **조금이라도 겹치면 살린다** (관대한 판정) |
| ③ 클램프 | `std::clamp(v, 0.0, 1.0)` | 화면 밖으로 삐져나온 부분을 잘라 앱이 그릴 수 있는 값으로 |
| ④ 퇴화 | `r <= l \|\| b <= t` | 클램프 결과 면적이 0이 된 박스 제거 |

**순서가 중요하다.** ②를 ③보다 먼저 하지 않으면 완전히 화면 밖에 있는 박스가 클램프되어 **가장자리에 달라붙은 가짜 블러**가 된다.

### 3.4 축소가 재할당을 유발하지 않는 이유

출력 개수는 **항상 입력 이하**이므로 새 벡터가 필요 없다. 통과한 원소만 앞으로 당긴 뒤 `resize(writeIdx)` 로 잘라내는데, **`resize` 로 줄이는 것은 재할당을 유발하지 않는다** (capacity 유지). 다음 프레임에서 같은 벡터가 재사용되므로 warmup 이후 이 함수의 힙 할당은 **완전히 0**이다.

### 3.5 전량 필터링 진단

```cpp
if (inputCount > 0 && writeIdx == 0) {
    logError(kIface, "ch=... 입력 blur 객체 N개가 전부 필터링됨 (scale/offset 설정 확인 필요)");
}
```

입력이 있었는데 결과가 0이라는 것은 거의 항상 **scale/offset 오설정**을 뜻한다(예: `offsetX = 5.0` 이면 모든 박스가 화면 오른쪽 밖으로 밀려난다). 이 신호가 없으면 "블러가 하나도 안 걸리는" 증상만 남고 원인을 찾기 어렵다.

> 이 로그는 **프레임당 최대 1회**이고 비정상 상황에서만 발생하므로 rate-limit 을 두지 않았다.

---

## 4. 설정 (`config.json`)

| 키 | 기본값 | 의미 |
|----|--------|------|
| `imageMapScaleX` | `1.0` | 가로 스케일 |
| `imageMapScaleY` | `1.0` | 세로 스케일 |
| `imageMapOffsetX` | `0.0` | 가로 이동 |
| `imageMapOffsetY` | `0.0` | 세로 이동 |

> **⚠️ 기본값은 항등 변환이다.** 이는 "화각 불일치를 보정하지 않은 상태"이지 "보정이 필요 없음이 검증된 상태"가 아니다. 카메라의 메타데이터 화각과 송출 화각이 실제로 같은지 확인하지 않았다면, 블러가 미세하게 빗나가고 있을 수 있다.

`AppConfig::load` 는 이 4개 값에 **clamp 를 적용하지 않는다** — 값의 유효성 판정은 매퍼 생성자가 fail-fast 로 담당한다(§3.2).

---

## 5. Pipeline 통합

```cpp
// Router 가 blur/risk 를 가른 '뒤', blur 분기에만 적용
imageMapper_->map(routeResult_.blur, frame.channelId);
```

- **프레임당 정확히 1회** 호출된다(내부에서 객체를 순회).
- 매핑 이후의 `routeResult_.blur` 가 `MqttBlurSink` 로 넘어가 `veda::BlurTarget` 으로 발행된다.
- 매핑에서 제거된 객체는 **블러 대상에서 빠질 뿐** risk 판정에는 영향이 없다(경로가 이미 갈라진 뒤이므로).

---

## 6. Edge-Worker 원칙

- **blur 경로 격리**: 이 매핑은 blur 전용이다. risk 경로에 적용하면 월드 좌표가 조용히 틀어진다 — 인터페이스 헤더의 `@warning` 이 이를 명시한다.
- **호출당 무할당**: in-place 변형 + `resize` 축소로 힙 할당 0. 반환값 형태로 "단순화" 하지 말 것.
- **채널 단일성**: 자기 채널의 scale/offset 4개만 안다. 다른 카메라도, `channelCount` 도 모른다. `channelId` 는 로그 태그일 뿐이다.
- **조립 시점 fail-fast**: 잘못된 파라미터는 생성자가 throw 하여 프로세스를 종료시킨다. 런타임에 "그럴듯하지만 빗나간" 블러를 그리지 않는다.
- **판정의 단일 소유권**: 경계 플래그는 파서만 판정한다. 좌표계가 다른 계층이 같은 필드를 다시 쓰면 의미가 오염된다.
