# `Pipeline` 레퍼런스 매뉴얼 (compute-server)

> **대상**: compute-server 의 패킷 처리 흐름을 이해·수정하려는 개발자
> **원본**: Pipeline.h, Pipeline.cpp
> **관련 스테이지**: OnvifParser, ContainmentSanitizer, ParentBasedRouter, BottomCenterExtractor, HomographyTransform, AffineImageCoordinateMapper, MqttTopViewSink, MqttBlurSink

---

## 0. 한 줄 요약

`Pipeline`은 **단일 스레드 메타데이터 처리 루프**다. `RawPacket` 하나를 받아 파싱 → 검증 →
분기(risk/blur) → 좌표변환 → 발행까지 **고정된 순서**로 흘려보낸다. 순서는 절대 바뀌지 않고, 각 단계의 **구현체만 `AppContext`가 주입**한다.

---

## 1. 역할과 경계

- **고정된 뼈대, 교체 가능한 살**: `Pipeline`은 인터페이스만 안다. 동작을 바꾸려면 구현체를 갈아끼우되, `Pipeline`의 시퀀스는 손대지 않는다.
- **Network/Source 제외**: CCTV에서 메타데이터를 받아오는 단계는 파이프라인 밖이다. `main`이 소스에서 `next()`로 패킷을 당겨 `onPacket()`에 넣는다.
- **출력은 카메라 로컬 좌표**: risk 결과는 카메라 로컬 지면 좌표(m)다. 도면(월드) 좌표로 옮기는 것은 **control-server의 몫**이다.

### 공개 인터페이스

| 멤버 | 용도 |
| --- | --- |
| `Pipeline(parser, imageMapper, sanitizer, router, ground, transform, riskSink, blurSink, options)` | 스테이지 구현체 + 정책 주입 |
| `void onPacket(const domain::RawPacket&)` | 패킷 1개를 전체 시퀀스에 흘려보냄 |

---

## 2. 처리 흐름 (The Chain)

```
Parser -> Sanitizer -> Router --+-- risk -> Ground -> Transform -> RiskSink
                                +-- blur -> ImageMapper ---------> BlurSink
```

한 패킷의 정확한 진행:

1. **`parser_->parse(raw)`** → `ChannelFrame`
   ONVIF XML을 파싱해 `utcTime`, `channelId`, `DetectedObject` 목록으로 만든다.
   문자열 `find`/`from_chars` 기반이라 **어떤 손상 입력에도 예외 없이 빈 프레임**을 돌려준다.
2. **`sanitizer_->sanitize(std::move(frame))`** → 팬텀 객체 제거
   `ContainmentSanitizer`가 **스택 `std::bitset<128>` drop 마스크**로 판정한다 — 프레임마다
   `std::vector<bool>` 을 새로 할당하지 않는 hot-path 무할당 설계
   규칙 A(risk ↔ blur IoU 초과) / 규칙 B(같은 클래스 포함)로 중복을 걸러 in-place 압축
   객체 수가 상한(128)을 넘으면 **sanitize 를 건너뛴다.** — 팬텀을 못 지울 뿐, 위험 객체를 지우지 않는 쪽이 안전
1. **`router_->route(frame)`** → `RouteResult{ risk, blur }`
   객체를 두 갈래로 분류한다: **risk**(Human/Vehicle — 위험 판정 대상) / **blur**(Head/LicensePlate —
   앱에서 가려야 할 대상)
   이후 두 경로는 서로 독립적으로 처리·발행된다.

### 2-A. risk 경로 (안전 크리티컬 — **먼저** 처리)

각 risk 객체마다:
- **잘림 정책 검사** `isEdgeRejected(o, edgePolicy)` — 정책에 걸리면 폐기하고 `edgeDropCount_` 증가
- **`ground_->extract(o.box)`** — bbox 아래변 중앙(발 위치)을 지면 접촉점으로 뽑는다
  (`BottomCenterExtractor`).
- **`transform_->toLocal(groundPoint)`** — 호모그래피로 이미지 지면점 → **카메라 로컬 좌표(m)**
  결과가 `nullopt`(지평선 위/너머, 로컬 범위 밖)면 폐기하고 `transformFailCount_` 증가
- 살아남은 객체를 `TopViewObject`로 담아 `riskFrame`에 push
- **`riskSink_->send(riskFrame)`** (비동기 큐잉)

> **risk 경로는 `imageMapper`를 거치지 않는다.** 호모그래피는 파서가 만든 **메타데이터 이미지 평면**에서 캘리브레이션되므로, 
> blur 정합용 `imageMapScale/Offset`을 이 경로에 섞으면 월드 좌표가 조용히 틀어진다.

### 2-B. blur 경로

- **`imageMapper_->map(routed.blur, channelId)`** — blur 박스를 **앱 표시 좌표계**로 매핑
  (`AffineImageCoordinateMapper`) 앱이 영상 위에 사각형을 얹어야 하므로 이 경로에서만 수행
- 각 blur 대상을 `BlurTarget`으로 담아 `blurFrame`에 push
- **`blurSink_->send(blurFrame)`** (비동기 큐잉)

> **왜 risk 를 blur 보다 먼저 보내는가**: risk는 안전 크리티컬 실시간 경로라 sink로 나가는 시점을 앞당긴다(추가 비용 0)
> 빈 프레임(위험/블러 객체 0개)도 정상이며 그대로 발행된다 — "그 시각에 대상이 없다"는 유효한 상태이기 때문

---

## 3. 스테이지 요약표

| # | 스테이지 | 구현체 | 하는 일 | 경로 |
| --- | --- | --- | --- | --- |
| 1 | 파서 | `OnvifParser` | ONVIF XML → `DetectedObject` (예외 없음) | 공통 |
| 2 | 새니타이저 | `ContainmentSanitizer` | 팬텀 제거 (스택 bitset, fail-open) | 공통 |
| 3 | 라우터 | `ParentBasedRouter` | risk / blur 분류 | 공통 |
| 4a | 지면점 | `BottomCenterExtractor` | bbox 아래변 중앙 추출 | risk |
| 5a | 좌표변환 | `HomographyTransform` | 이미지 지면점 → 로컬 좌표(m), 실패 시 drop | risk |
| 6a | risk 발행 | `MqttTopViewSink` | `TopViewFrame` 비동기 발행 | risk |
| 4b | 이미지 매퍼 | `AffineImageCoordinateMapper` | 앱 표시 좌표 매핑 | blur |
| 5b | blur 발행 | `MqttBlurSink` | `BlurFrame` 비동기 발행 | blur |

---

## 4. 예외 경계 (Exception Boundary)

**위치**: 파이프라인 구동 지점인 `main.cpp`의 메인 루프가
`onPacket()` 호출을 `try/catch` 로 감싼다.

```cpp
while (context->source().next(raw)) {
    ++packetCount;
    try {
        context->pipeline().onPacket(raw);
    } catch (const std::exception& error) {
        logError(kIface, "패킷 처리 중 예외 - 이 패킷을 건너뜁니다: " + std::string(error.what()));
    } catch (...) {
        logError(kIface, "패킷 처리 중 알 수 없는 예외 - 이 패킷을 건너뜁니다");
    }
}
```

- **무엇을 막는가**: 한 패킷 처리 도중의 예외가 **루프(=프로세스) 전체를 끝장내는 것**을 막는다.
  파서는 이미 예외를 던지지 않도록 설계됐지만, 예상 못한 `throw` — 대표적으로 메모리 압박
  하의 **`std::bad_alloc`**가 발생해도 **그 패킷 하나만 버리고
  다음 패킷으로 계속 돈다**.
- **왜 엣지 워커에 중요한가**: 없으면 예외가 `main`을 탈출해 `std::terminate`로 즉사한다.
  그러면 종료 시퀀스가 생략되어 **MQTT dead 신호도 못 보내고** 갑자기 죽는다.
  경계를 두면 일시적 이상(손상 패킷/순간 메모리 부족)에도 워커가 살아남아 계속 서비스한다.
- **경계의 위치가 main 인 이유**: `onPacket`을 부르는 유일한 지점이 main 루프라, 여기서 한 번만 감싸면 파이프라인 내부 어느 스테이지의 예외든 모두 포착된다. (스테이지마다 중복 `try/catch` 불필요)

---

## 5. 스레드 안전성

**`Pipeline`은 내부 락이 전혀 없다 — 필요가 없기 때문이다.**

- **단일 스레드 실행**: `onPacket()`은 오직 main 루프에서 **순차적으로** 호출된다. 두 패킷이 동시에 파이프라인을 지나는 일이 없다.
- **경계에서 이미 스레드 안전**: 소스(생산자 워커 스레드)와 파이프라인(소비자 = main 스레드)은
  **`RtspOnvifSourceV2`의 SPSC 링버퍼**로 분리돼 있다. 크로스 스레드 핸드오프는 그 링버퍼가 mutex 로 처리하고, `next()`는 **버퍼 소유권을 스왑(zero-copy)**해 main 스레드에 넘긴다 →
  파이프라인이 만지는 `RawPacket`은 이미 main 스레드 전용이라 스테이지가 락을 걸 이유가 없다.
- **비동기 발행**: `send()`는 프레임을 sink 큐에 넣고 즉시 반환한다. (논블로킹) 실제 MQTT publish는 **각 sink의 워커 스레드**에서 일어나므로, 파이프라인 루프가 네트워크에 막히지 않는다.
- **rate-limit 카운터**: `edgeDropCount_`/`transformFailCount_`는 로그 도배 방지용 누적 카운터인데, 단일 스레드 호출 전제라 **atomic이 아니어도 안전**하다.

---

## 6. 폐기(drop) 지점 한눈에 보기

| 지점 | 사유 | 집계 |
| --- | --- | --- |
| 새니타이저 | 팬텀(중복) 객체 | (Debug 로그) |
| risk: 잘림 정책 | bbox 잘림으로 지면점 신뢰 불가 | `edgeDropCount_` (rate-limit) |
| risk: 좌표변환 | 지평선 위/너머·로컬 범위 밖 | `transformFailCount_` (rate-limit) |
| sink 큐 | 큐가 가득 참(`drop-oldest`) / 유효성 실패 | sink `droppedCount()` |

---

## 7. 수정 가이드

- **동작을 바꾸려면**: 새 스테이지 구현체를 만들어 **`AppContext`에서 주입**을 교체한다.
  `Pipeline`의 시퀀스 코드는 건드리지 않는다.
- **순서(뼈대)는 고정**이다: Parser → Sanitizer → Router → (`risk`: Ground → Transform → RiskSink) / (`blur`: ImageMapper → BlurSink) 특히 **ImageMapper는 blur 분기에만** 두어야 하고, **risk를 blur보다 먼저** 보내는 순서를 유지한다.
- **`onPacket` 은 계속 단일 스레드 전제**로 둔다. 병렬화하려면 스테이지의 무락(lock-free) 가정과 rate-limit 카운터부터 다시 설계해야 한다.
