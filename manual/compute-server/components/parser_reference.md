# Parser 모듈 레퍼런스

> **대상 파일**
> - 인터페이스: `include/interfaces/IMetadataParser.h`
> - 구현체: `src/parser/OnvifParser.h`, `.cpp`

ONVIF CCTV가 RTSP 인터리브로 실어보내는 **원본 XML 메타데이터**(`domain::RawPacket`)를 파이프라인 내부 표현(`domain::ChannelFrame`)으로 바꾸는 파이프라인의 **첫 단계**다. `std::string_view` 위에서 태그 위치만 찾아 잘라내는 **경량 수동 파서**로 구현되어 있다.

---

| Date | Version | Writer | Summary |
| --- | --- | --- | ---|
| 2026.07.27 | v1.0.0 | Mangjun | IMetadataParser 인터페이스 및 OnvifParser 구현체 동작 원리(무할당 파싱, 좌표 정규화) 문서화 |

---

## 1. 인터페이스 명세 (Interface Contract)

```cpp
class IMetadataParser {
public:
    virtual ~IMetadataParser() = default;
    virtual domain::ChannelFrame parse(const domain::RawPacket& raw) = 0;
};
```

계약이 요구하는 것은 단 하나, `parse(raw)`다. 그러나 헤더 주석이 못박은 **정책**이 계약의 본체다.

| 항목 | 계약 내용 |
|------|-----------|
| **파싱 대상** | `<tt:Class><tt:Type Likelihood="...">` 만 읽음 |
| **프레임별 재적용** | `<tt:Transformation>`(Scale/Translate)은 값이 매 프레임 바뀔 수 있으므로 프레임마다 새로 읽어 적용 |
| **예외 금지** | **실패 시 예외를 던지지 않는다.** 파싱 불가능한 패킷은 *빈* `ChannelFrame` 반환 |
| **왜 예외 금지인가** | 파이프라인 스레드가 잘못된 패킷 하나 때문에 죽으면 안 됨 (한 채널의 손상 프레임이 프로세스를 내리지 않도록) |

즉 반환 계약은 **"항상 유효한 `ChannelFrame` 하나"** 이며, 실패는 `objects`가 비었거나 `utcTime==0`인 프레임으로 표현된다.

### 출력 타입 `domain::ChannelFrame`

```cpp
struct ChannelFrame {
    veda::TimestampMs utcTime = 0;        // 프레임 UtcTime (CCTV 기준 epoch ms)
    veda::ChannelId   channelId = 0;      // CCTV 채널 ID
    std::vector<DetectedObject> objects;  // 감지된 내부용 객체 목록
};
```

`DetectedObject`는 `id`, `parentId(optional)`, `cls`, 정규화 bbox(`NormBox`), 그리고 두 개의 경계 플래그 `touchesBorder` / `bottomTruncated`를 가진다. **이 경계 플래그를 채우는 것이 파서의 핵심 책임 중 하나**다.

---

## 2. 구현체 분석 (Implementation Details)

### 2.1 생성자와 `edgeEpsilon_`

```cpp
explicit OnvifParser(double edgeEpsilon = 0.002);
```

`edgeEpsilon`은 "bbox가 프레임 경계에 닿았다"고 볼 정규화 좌표 오차율이다. **경계 판정은 이 파서에서만 수행**한다 — 예전에는 매퍼에도 같은 상수가 중복 정의되어 값이 갈라질 여지가 있었으나, 매퍼가 blur 전용이 되면서 판정 책임을 파서 한 곳으로 모았다.

### 2.2 무할당 지향 파싱 기법

파서 전체가 `std::string_view` 위에서 동작하며, 프레임당 수십 회 호출되는 지점에서 임시 문자열 생성을 피한다.

- **`extractQuoted(s, key)`** — `key="value"`에서 `value`를 뽑는다. `key + "=\""`를 이어붙인 임시 `std::string`을 만들어 검색하던 방식 대신, `key`만 먼저 찾고 바로 뒤가 `="`인지 직접 확인 → **임시 문자열 생성(및 SSO 초과 시 힙 할당)을 제거**
- **`parseNumber<T>(sv)`** — `std::from_chars` 래퍼 (실패는 `std::nullopt`)
- **`parseUtcTimeMs`** — `UtcTime="YYYY-MM-DDTHH:MM:SS.sssZ"` 고정 포맷을 **위치 기반**으로 잘라 `timegm`으로 epoch ms 환산

### 2.3 좌표계 변환 (ONVIF → NormBox)

ONVIF 정규화 좌표는 **원점 중앙 `[-1,1]`, y축 위쪽**이다. 이를 파이프라인 내부의 **좌상단 원점 `[0,1]`** 좌표로 옮긴다.

```cpp
double normX(px, t) { return (t.scaleX * px + t.translateX + 1.0) * 0.5; }
double normY(py, t) { return (1.0 - (t.scaleY * py + t.translateY)) * 0.5; }
```

> `Scale_y`가 음수인 것이 ONVIF 표준상 정상이다. **손으로 부호를 뒤집지 않고 스트림 값을 그대로 적용**한다. (설치 방향과 무관한 표준 좌표계)

### 2.4 `parse()`의 방어적 스킵 정책

`parse()`는 신뢰할 수 없는 데이터를 만나면 **로그를 남기고 빈/부분 프레임을 반환**한다. 절대 throw하지 않는다.

1. `<tt:Frame>` 시작/종료 태그 없음 → 프레임 스킵
2. `UtcTime` 속성 없음 / 형식 파싱 실패 → 프레임 스킵
3. **`Transformation` 파싱 실패 → 프레임 스킵** (좌표 자체를 신뢰할 수 없으므로 객체를 하나도 내보내지 않음)
4. 객체 루프 내부: `ObjectId` 없음 / bbox 없음 / bbox 숫자 파싱 실패 → **해당 객체만** `continue`

페이로드당 `<tt:Frame>`은 하나라고 가정하며, 여러 개면 첫 번째만 처리한다.

### 2.5 경계 플래그 계산 (risk 경로 정확도의 핵심)

```cpp
det.bottomTruncated = det.box.b >= 1.0 - edgeEpsilon_;
det.touchesBorder   = det.bottomTruncated
                    || det.box.l <= edgeEpsilon_
                    || det.box.r >= 1.0 - edgeEpsilon_
                    || det.box.t <= edgeEpsilon_;
```

`bottomTruncated`를 `touchesBorder`와 **따로 두는 이유**가 중요하다. 지면점(bbox 아래변 중앙)의 신뢰도는 "어느 변이 잘렸는가"에 따라 완전히 다르다.

- 좌/우/위 변이 잘림 → 발이 여전히 보이므로 지면점 **유효**
- **아래변이 잘림** → 발 위치를 모르는 채 잘린 지점을 지면으로 오인 → 호모그래피가 실제보다 수 미터 먼 곳으로 사상

이 플래그는 뒤의 `Pipeline`이 `RiskEdgePolicy`(기본 `DropBottomTruncated`)로 소비한다.

### 2.6 `<tt:Type>` 분류와 진단 로그

`ClassCandidate` 블록을 건너뛴 뒤 실제 `<tt:Type>` 텍스트를 찾아 `veda::objectClassFromString()`으로 매핑한다. 결과가 `Unknown`이면 — 이는 파싱 실패가 아니라 **"정상 인식된 미지원 값"** 이므로 — `g_unknownTypeCount`로 **rate-limit(1건째, 이후 100건마다)** 진단 로그를 남긴다. 이 로그가 없으면 blur가 조용히 걸러지는 원인을 추적할 수 없다.

> `g_unknownTypeCount`가 락 없는 전역인 이유: 파서는 **채널당 단일 스레드**에서만 호출되므로 경합이 없다.

---

## 3. Edge-Worker 원칙

- **파서는 채널을 하나만 안다.** 출력 `ChannelFrame.channelId`는 입력 `raw.channelId`를 그대로 복사할 뿐, 다른 채널의 존재나 `channelCount`를 전혀 모른다.
- **좌표는 전부 카메라 로컬(정규화 이미지 평면)이다.** 파서가 만드는 것은 `[0,1]` 이미지 좌표까지이며, 미터·월드 좌표로의 변환은 이후 단계(`Transform`)의 몫이다. 파서는 도면을 전혀 알지 못한다.
- **판정은 Metadata 좌표계에서 단 한 번.** 경계 판정(`touchesBorder`/`bottomTruncated`)을 여기서 확정하므로, 매퍼는 이를 재계산하지 않는다. (중복 상수로 값이 갈라지는 잠복 결함 제거)