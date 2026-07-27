# Sanitizer 모듈 레퍼런스

> **대상 파일**
> - 인터페이스: `include/interfaces/IObjectSanitizer.h`
> - 구현체: `src/sanitize/ContainmentSanitizer.h`, `.cpp`

파서가 만든 `ChannelFrame`에서 **팬텀(중복) 객체를 제거**하는 단계다. 같은 물리적 대상을 CCTV가 두 번 내보내는 경우(예: 같은 머리를 `Head`로도 `Human`으로도, 또는 큰 `Human` 안에 작은 `Human`이 겹쳐 나오는 경우)를 잡아, 하류의 위험 판정과 지면점 추출이 **하나의 실체를 두 번 세는** 오류를 막는다.

---

| Date | Version | Writer | Summary |
| --- | --- | --- | ---|
| 2026.07.27 | v1.0.0 | Mangjun |  |

---

## 1. 인터페이스 명세 (Interface Contract)

```cpp
class IObjectSanitizer {
public:
    virtual ~IObjectSanitizer() = default;
    virtual domain::ChannelFrame sanitize(domain::ChannelFrame frame) = 0;
};
```

- **값 전달·값 반환**: `frame`을 값으로 받아 필터링 후 반환한다. 구현체가 내부에서 **in-place 압축**을 하도록(=별도 벡터를 새로 만들지 않도록) 의도된 시그니처다.
- 계약은 "유효하지 않은 팬텀 객체를 제거한 프레임"만 요구하고, *어떤 규칙으로* 판정하는지는 구현체 자유다.

---

## 2. 구현체 분석 (Implementation Details)

### 2.1 팬텀 판정 3규칙

`ContainmentSanitizer(double iouThresh = 0.5, double containThresh = 0.9)`

| 규칙 | 조건 | 근거 |
|------|------|------|
| **A** | RISK 후보(Human/Vehicle)가 **부위 객체(Head/LicensePlate)** 와 `IoU > iouThresh_` | CCTV가 같은 신체 부위를 Head로도 Human으로도 동시 출력 (실측: ObjectId 3024 ↔ Head 3022, IoU≈0.69) |
| **B** | 더 작은 RISK 후보가 **같은 클래스**의 더 큰 RISK 객체 안에 거의 완전히 포함(`IoMin > containThresh_`) | 부위 객체가 함께 안 나오는 중복에 대한 안전망 |
| **C** | `likelihood`로는 **필터링하지 않음** | 실측에서 진짜 사람(0.46)이 중복(0.56)보다 likelihood가 낮게 나온 사례 존재 |

> **규칙 B가 "같은 클래스"에만 적용되는 이유**: 클래스가 다르면(예: Human이 Vehicle 안에 겹침) 서로 다른 실제 객체일 수 있으므로 제거하면 안 된다.

기하 헬퍼: `area`, `intersectionArea`, `iou`(=inter/union), `ioMin`(=inter/min(면적)) **IoMin**은 크기 차가 큰 두 bbox에서 "작은 쪽이 큰 쪽 안에 거의 다 들어있는지"를 IoU보다 정확히 반영하므로 포함 판정(규칙 B)에 쓴다.

### 2.2 스택 `std::bitset` drop-mask (무할당 hot path)

이 모듈의 성능 핵심이다.

```cpp
constexpr std::size_t kMaxObjectsPerFrame = 128;
...
std::bitset<kMaxObjectsPerFrame> drop;   // 스택. 프레임마다 힙 할당 0
```

- 제거 대상 표시를 `std::vector<bool>`(프레임마다 힙 할당)이 아니라 **컴파일타임 크기의 스택 `std::bitset`** 으로 둔다.
- `kMaxObjectsPerFrame = 128`의 근거: compute-server는 채널당 1프로세스이고, 엣지 AI의 NMS 출력은 보통 프레임당 50~100개로 제한되므로 128이면 충분한 여유

### 2.3 Fail-Open 상한 처리

```cpp
if (n > kMaxObjectsPerFrame) {
    logError(...);   // 비정상 폭주
    return frame;    // sanitize 스킵 (원본 그대로 통과)
}
```

객체 수가 상한을 넘으면(비정상 폭주) sanitize를 **스킵하고 원본을 그대로 반환**한다. 이것이 **fail-open**의 안전 논리다. — 팬텀 제거만 못 할 뿐, **위험 객체를 실수로 지우지 않는 쪽**이 안전 측면에서 낫다.

### 2.4 2단계 알고리즘 (판정 → 압축)

**1단계(판정 전용)**: `i`의 판정은 다른 모든 `j`의 **원본** bbox를 참조하므로, 이 단계가 끝나기 전에는 `frame.objects`를 **절대 변형하면 안 된다**. 규칙 A/B에 걸리면 `drop[i]=true`만 세우고 넘어간다. (정상 동작이라 제거 로그는 `Debug` 레벨 + 레벨 가드로 문자열 조립까지 억제)

**2단계(in-place 압축)**: 판정이 모두 끝난 뒤에만 통과 원소를 앞으로 당기고(`std::move`) `resize(writeIdx)`로 잘라낸다. **`resize`로 줄이는 것은 재할당을 유발하지 않으므로** 이 단계도 힙 할당이 없다.

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

> 판정과 변형을 섞으면 "이미 지워 앞으로 당겨진 원소"를 다른 판정이 참조해 결과가 달라진다. **판정 완료 후 압축**이 정확성의 핵심

---

## 3. Edge-Worker 원칙

- **채널 단일성**: `frame.channelId`는 로그 태그로만 쓰이고, 다른 채널이나 전역 상태를 참조하지 않는다.
- **무할당 원칙 준수**: 스택 `std::bitset` + in-place `resize` 압축으로 프레임당 힙 할당 0 <br> 채널당 1프로세스라는 엣지 워커 구조가 `kMaxObjectsPerFrame` 상한을 타당하게 만든다.
- **안전 우선 fail-open**: 상한 초과 시 위험 객체를 지우기보다 sanitize를 건너뛴다. — 엣지에서 잘못된 삭제가 곧 미탐지(위험 누락)로 직결되기 때문