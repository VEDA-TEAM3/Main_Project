# Source 계층 보안·성능 권고 (Security & Performance Advisory)

> **대상 모듈**
> - 인터페이스: `include/interfaces/IMetadataSource.h`
> - 구현체: `src/source/RtspOnvifSourceV2.h`, `.cpp`
>
> **관련 문서**: 모듈 상세 [../components/source_reference.md](../components/source_reference.md) · 하위 계층 감사 [network_security.md](network_security.md)

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | Source 계층(파이프라인 핸드오프 및 링버퍼) 스레드 안전성, OOM 방어 및 성능/보안 감사 명세 |

---

## 1. 위협 모델 (Threat Model) — 내부 자원 고갈 & 스레드 교착

이 계층은 **외부 공격자와 직접 마주하지 않는다.** 신뢰 경계는 하위 네트워크 계층([network_security.md](network_security.md))이 이미 통과했고, 페이로드 *내용*의 위협은 상위 파서([parser_security.md](parser_security.md))가 다룬다.

따라서 Source 계층의 위협 모델은 **외부 침입이 아니라 내부 자원·동시성 실패**에 맞춰진다.

| 위협 범주 | 구체적 시나리오 | 영향 |
|-----------|-----------------|------|
| **자원 고갈 (내부)** | 컨슈머(Pipeline)가 느려지거나 멈췄는데 프로듀서(카메라)는 계속 프레임을 밀어넣음 | 큐 무한 증가 → OOM → 프로세스 강제 종료 |
| **스레드 교착/유실 기상** | 종료 신호가 대기 중인 스레드에 전달되지 않음 (lost wakeup) | `next()` 영구 블로킹 → **종료 불가** → systemd SIGKILL → MQTT 종료 신호(dead) 미발행 |
| **수명주기 위반 (UAF)** | 워커 스레드가 살아있는 상태에서 클라이언트/소스 객체가 파괴됨 | use-after-free → 크래시 또는 조용한 메모리 오염 |
| **자원 누수** | 재연결 루프가 소켓·스레드를 회수하지 않고 반복 | fd 고갈 → 장기 가동 중 연결 불가 |
| **지연 누적** | 오래된 프레임이 큐에 쌓여 실시간성 상실 | 위험 판정이 과거 상태 기준으로 이루어짐 (안전 문제) |

### 왜 "종료 불가"가 보안 사안인가

CLAUDE.md가 명시하듯 **graceful shutdown은 이 시스템에서 load-bearing 속성**이다. `next()`가 풀리지 않으면 `main` 루프가 빠져나오지 못하고 → `AppContext` 소멸이 일어나지 않고 → **MQTT `alive="0"` 종료 신호가 발행되지 않는다.** 그 결과 control-server는 해당 채널이 *죽었는지 단지 조용한 것인지* 구분하지 못하고, 최대 `1.5 × keepalive` 동안 **죽은 채널을 살아있다고 오인**한다. 위험 판정 시스템에서 이는 안전 문제로 직결된다.

---

## 2. OOM & 메모리 상한 방어 (Ring Buffer Logic)

### 2.1 고정 용량 링버퍼 — 무한 증가 구조가 존재하지 않음

```cpp
ring_.resize(ringCapacity_);   // 생성자에서 1회. 이후 크기 변경 없음
std::size_t head_  = 0;
std::size_t count_ = 0;        // 불변식: 0 <= count_ <= ringCapacity_
```

`std::queue<RawPacket>`(deque)를 **의도적으로 폐기**하고 고정 크기 `std::vector`로 대체했다. 슬롯 개수가 컴파일 시점이 아닌 런타임 값이지만, **생성 이후에는 절대 늘어나지 않는다.** `push_back`도, `resize`도, `emplace`도 hot path에 없다.

### 2.2 drop-oldest가 메모리 상한을 보장하는 방식 ✅

**컨슈머(Pipeline)가 완전히 멈춰도 메모리 사용량은 증가하지 않는다.** 이것이 이 설계의 핵심 안전 속성이다.

```cpp
if (count_ == ringCapacity_) {
    // 링이 가득 참: 새 슬롯을 '할당'하지 않고, 가장 오래된 슬롯을 그대로 재사용
    writeIdx = head_;
    head_ = (head_ + 1) % ringCapacity_;
    ++metrics_.droppedCount;      // 드랍을 계측 (은폐하지 않음)
} else {
    writeIdx = (head_ + count_) % ringCapacity_;
    ++count_;
}
```

포화 상태에서 프로듀서는 **`head_`를 한 칸 밀고 그 슬롯에 덮어쓴다.** `count_`는 `ringCapacity_`에서 더 이상 증가하지 않는다. 즉:

> **컨슈머 stall 시 동작**: 큐 길이 = `ringCapacity_`로 고정 → **메모리 상한 보장**. 가장 오래된 프레임이 소리 없이 사라지는 대신, `droppedCount`로 계측되어 로그에 드러난다.

이는 실시간 좌표 시스템에 올바른 선택이다 — **오래된 위치 정보는 가치가 없고**, 지연이 무한정 누적되면 위험 판정이 과거 상태를 기준으로 이루어진다. 메모리를 지키는 동시에 실시간성도 지킨다.

### 2.3 메모리 사용량의 이론적 상한

| 요소 | 상한 |
|------|------|
| 슬롯 개수 | `sourceRingCapacity` (기본 **8**) |
| 슬롯당 `bytes` 최대 크기 | `rtspMaxMetadataFrameBytes` (기본 **1 MiB**) — 하위 네트워크 계층이 강제 |
| **링 전체 최악 상한** | **8 × 1 MiB = 8 MiB** |
| 추가 | `main`의 `raw` 버퍼 1개 (스왑 파트너) |

실측 페이로드는 수 KB 수준이므로 정상 운용 시 수십 KB에 그친다.

> **⚠️ 고수위(high-water mark) 특성**: 각 슬롯의 `bytes` capacity는 **한 번 커지면 줄어들지 않는다**(`assign`/`swap`만 쓰고 `shrink_to_fit`을 하지 않음). 이는 "프레임당 힙 할당 0"을 위한 **의도된 트레이드오프**다. 비정상적으로 큰 프레임이 한 번 들어오면 그 capacity가 상주하지만, §2.3의 상한(8 MiB) 안에 갇혀 있으므로 **OOM으로 발전하지 않는다.**

### 2.4 `% ringCapacity_` 0 나눗셈 방어 — 설정 계층 의존

`ringCapacity_ == 0`이면 `(head_ + 1) % ringCapacity_`가 **0 나눗셈(UB/크래시)** 이 된다. 현재 이는 `AppConfig`가 막는다.

```cpp
// AppConfig.h
clampPositive(cfg.sourceRingCapacity, 8, "sourceRingCapacity");

static inline void clampPositive(int& value, int fallback, const char* name) {
    if (value <= 0) { /* 경고 출력 */ value = fallback; }
}
```

- `config.json`으로 `0`이나 음수를 넣어도 **경고와 함께 기본값 8로 교정**된다. → 정상 경로에서 안전 ✅
- 구조체 기본값도 `int sourceRingCapacity = 8`이므로 기본 생성된 `AppConfig`도 안전 ✅
- **ℹ️ 잔여 사항**: `RtspOnvifSourceV2` 자체는 이 값을 **재검증하지 않는다.** `AppConfig::load()`를 우회해 손으로 `config.sourceRingCapacity = 0`을 설정한 뒤 생성자에 넘기면(예: 테스트 하네스) 0 나눗셈이 가능하다. 방어 심층화 관점에서 생성자에 `if (ringCapacity_ == 0) ringCapacity_ = 1;` 수준의 가드를 두면 계층 간 의존이 끊긴다.

---

## 3. 수명주기 & 스레드 안전성 (Lifecycle & Thread Safety)

### 3.1 스레드 구성과 공유 상태

| 스레드 | 역할 | 접근하는 상태 |
|--------|------|----------------|
| **워커 스레드** (Source 소유) | 재연결 루프 + `client.run()` 구동. **콜백도 이 스레드에서 실행** | `ring_`, `head_`, `count_`, `metrics_`(쓰기) — 전부 `mtx_` 하에서 |
| **메인 스레드** (`next()`) | 링에서 꺼내 Pipeline으로 전달 | 동일 — 전부 `mtx_` 하에서 |
| **시그널 스레드** (`stop()`) | 종료 요청 | `stopping_`(atomic), `activeClient_`(`clientMutex_` 하) |

**두 개의 뮤텍스가 서로 다른 것을 지킨다**: `mtx_`는 링버퍼/지표를, `clientMutex_`는 `activeClient_` 포인터를 보호한다. 두 락을 **중첩해서 잡는 지점이 없으므로 락 순서 역전(교착)이 발생하지 않는다.** ✅

### 3.2 ✅ `sessionProductive` — 경합처럼 보이지만 안전함

```cpp
bool sessionProductive = false;                              // 평범한 bool, 워커 스택
client.onPayloadReceived = [this, &sessionProductive](...) {
    sessionProductive = true;                                // 동기화 없이 쓰기
    ...
};
```

리뷰 시 **가장 먼저 의심받는 지점**이지만 실제로는 안전하다. `onPayloadReceived`는 `client.run()` 내부에서 **동기적으로** 호출되고, `run()`은 워커 스레드가 직접 부른다. 즉 **읽기·쓰기가 모두 동일한 워커 스레드**에서만 일어난다. (keepalive 스레드는 `GET_PARAMETER` 전송만 하며 이 콜백을 부르지 않는다.)

선언 순서도 의도적이다 — `sessionProductive`를 `client`보다 **먼저** 선언해, 역순 파괴로 `client`가 먼저 사라지게 한다. 콜백이 캡처한 참조가 죽은 변수를 가리키는 일이 없다. ✅

### 3.3 ✅ UAF 방지 — `activeClient_` 널 처리 계약

```cpp
{ std::lock_guard<std::mutex> lk(clientMutex_);
  if (stopping_) break;                    // stop()이 이미 왔으면 등록조차 안 함
  activeClient_ = &client; }               // 취소 대상으로 등록

... connect / setup / play / run ...

{ std::lock_guard<std::mutex> lk(clientMutex_);
  activeClient_ = nullptr; }               // ★ client 파괴 '전에' 반드시 해제
```

`client`는 루프 반복마다 파괴되는 **지역 객체**다. 이 포인터를 파괴 전에 지우지 않으면 `stop()`이 죽은 객체의 `cancel()`을 호출한다. 등록/해제/사용이 **모두 `clientMutex_` 하에서** 일어나므로 경합이 없다. ✅

`if (stopping_) break;`를 등록과 **같은 임계 구역 안**에 둔 것도 중요하다 — 검사와 등록 사이에 `stop()`이 끼어들어 "취소 신호를 놓친 채 새 세션을 시작"하는 창을 닫는다. ✅

### 3.4 ✅ 워커 join 보장과 소멸 순서

```cpp
RtspOnvifSourceV2::~RtspOnvifSourceV2() {
    stop();                                   // 취소 신호 + 소켓 shutdown
    if (worker_.joinable()) { worker_.join(); }
}
```

- 소멸자 **본문**에서 join이 끝난 뒤에야 멤버(`ring_`, `mtx_`, `cv_`)가 파괴된다 → 워커가 이미 죽은 멤버를 만지는 일이 없다. ✅
- 콜백 람다가 캡처한 `this`도 join 이후에는 아무도 쓰지 않는다. ✅
- 세션마다 **지역 `RtspClientV2` 인스턴스**를 쓰므로 재연결 루프가 수천 번 돌아도 RAII로 소켓·keepalive 스레드가 회수된다 → **fd/스레드 누수 없음.** ✅
- `main.cpp`는 `signalThread.join()`을 **`context.reset()`보다 먼저** 수행하므로, 소멸자의 `stop()`과 시그널 스레드의 `stop()`이 동시에 실행되지 않는다. ✅

### 3.5 ✅ W1 — `stop()`의 유실 기상(lost wakeup) 창 — **패치 완료**

> **상태: 조치 완료 (2026-07-27).** 감사에서 최우선 지적 사항으로 식별되었으며, 아래 패치로 해소되었다.

#### 문제 (패치 이전)

```cpp
void RtspOnvifSourceV2::stop() noexcept {
    stopping_.store(true);            // ← mtx_ 를 잡지 않고 변경

    { std::lock_guard<std::mutex> lk(clientMutex_);   // ← 다른 뮤텍스
      if (activeClient_ != nullptr) { activeClient_->cancel(); } }

    cv_.notify_all();                 // ← mtx_ 를 한 번도 잡지 않은 채 notify (문제 지점)
}
```

컨슈머의 대기 술어는 **`mtx_`가 지키는 `count_`** 와 **`mtx_` 밖의 atomic `stopping_`** 을 함께 읽는다.

```cpp
std::unique_lock<std::mutex> lk(mtx_);
cv_.wait(lk, [this] { return count_ > 0 || stopping_.load(); });
```

**문제의 인터리빙**:

1. 컨슈머가 `mtx_`를 잡은 채 술어를 평가 → `count_==0`, `stopping_==false` → **false**
2. 아직 조건변수 대기열에 등록되기 **전**, 시그널 스레드가 `stop()`을 전부 실행 (`stopping_=true` → `notify_all()`). `stop()`은 `mtx_`를 잡지 않으므로 **컨슈머가 `mtx_`를 쥐고 있어도 진행 가능하다**
3. `notify_all()` 시점에 대기자가 없음 → **알림 소실**
4. 컨슈머가 대기열에 등록되고 블로킹 → 이후 워커도 종료해 **아무도 다시 notify하지 않음** → `next()` **영구 블로킹**

**영향**: `main` 루프가 빠져나오지 못함 → `context.reset()` 미실행 → **MQTT 종료 신호(dead) 미발행** → systemd 타임아웃 후 SIGKILL. §1의 "종료 불가" 시나리오가 그대로 실현된다.

#### 적용된 패치

`cv_.notify_all()` **직전에 컨슈머와 동일한 뮤텍스(`mtx_`)를 획득**하도록 수정했다.

```cpp
// [W1] notify 전에 mtx_ 를 잡는다: 그냥 notify 만 하면 컨슈머가 "술어를 false 로 평가한 뒤
// 조건변수 대기열에 등록되기 전" 구간에 알림이 끼어들어 유실된다 (lost wakeup).
// 술어가 보는 stopping_ 은 mtx_ 밖의 atomic 이라, stop() 이 mtx_ 를 전혀 잡지 않으면
// 컨슈머가 mtx_ 를 쥔 채로도 stop() 이 끝까지 진행할 수 있어 그 창이 실제로 열린다.
// 알림을 놓치면 워커도 곧 종료해 다시 notify 할 주체가 없으므로 next() 가 영구 블로킹되고,
// main 루프가 빠져나오지 못해 MQTT 종료 신호("0")를 발행하지 못한 채 SIGKILL 된다.
// (MqttFrameSink::start / MqttTransport::stop 이 쓰는 것과 같은 방어 패턴)
std::lock_guard<std::mutex> lock(mtx_);
cv_.notify_all();
```

#### 왜 이것이 창을 닫는가 — 결정론적 기상 보장

핵심은 **알림을 보내는 쪽이 컨슈머가 술어를 평가할 때 쓰는 것과 같은 뮤텍스를 거치도록 강제**하는 데 있다. 이로써 §3.5의 문제 인터리빙에서 2단계가 **성립 불가능**해진다.

- 컨슈머가 `mtx_`를 쥐고 있는 동안(= 술어 평가 중이거나, 대기열 등록을 진행 중인 동안)에는 `stop()`이 `mtx_` 획득에서 **블로킹**된다. 따라서 `notify_all()`은 그 위험 구간에서 절대 실행될 수 없다.
- `stop()`이 `mtx_`를 얻었다는 것은 컨슈머가 **이미 대기열 등록을 마치고 `mtx_`를 놓았다**(즉 `cv_.wait` 내부에서 대기 중)는 뜻이거나, **아직 `mtx_`를 잡기 전**이라는 뜻이다.
  - 전자라면 → `notify_all()`이 확실히 그 대기자에게 전달된다.
  - 후자라면 → 컨슈머는 이후 `mtx_`를 잡고 술어를 평가하는데, 그 시점엔 `stopping_ == true`이므로 **애초에 대기에 들어가지 않고** 즉시 통과한다.

두 경우 모두 **컨슈머가 반드시 깨어난다.** 즉 알림 유실 경로가 사라지고 **결정론적 기상(deterministic wakeup)** 이 보장된다.

그 결과:

| 보장 | 내용 |
|------|------|
| **`next()` 무한 대기 제거** | 종료 요청은 항상 컨슈머에게 도달한다 |
| **`main` 루프 탈출 보장** | `next()`가 잔여 프레임 배출 후 `false` 반환 |
| **graceful shutdown 보장** | `context.reset()`이 실행되어 **MQTT 종료 신호(`alive="0"`)가 정상 발행**된다 → control-server가 채널의 죽음을 즉시 인지 |
| **SIGKILL 회피** | systemd 종료 타임아웃에 걸리지 않음 |

> **락 유지 시간**: `lock_guard`가 `stop()` 함수 끝까지 `mtx_`를 잡고 있으나, 그 구간은 `notify_all()` 호출 한 번뿐이라 수 명령어 수준이다. 깨어난 컨슈머가 `mtx_`를 재획득하기까지의 지연도 무시할 수준이며, `stop()`은 락을 쥔 채 블로킹 작업을 하지 않으므로 **교착 위험이 없다.**
>
> **코드베이스 일관성**: `MqttFrameSink::start()`와 `MqttTransport::stop()`이 동일한 문제를 막기 위해 이미 같은 방어 패턴(뮤텍스 경유 후 notify)을 쓰고 있었다. 이번 패치로 **Source 계층까지 세 곳의 처리가 일관**되게 되었다.

> **참고 — 워커 측은 원래도 자가 치유되었다**: 워커의 백오프 대기는 `cv_.wait_for(lk, backoffSec, pred)`로 **타임아웃이 있어** 알림을 놓쳐도 최대 `backoffSec`(≤30초) 후 깨어나 `stopping_`을 확인하고 종료한다. 반면 컨슈머의 `cv_.wait`은 **타임아웃이 없어** 스스로 회복하지 못했다. 그래서 실제 위험은 `next()` 쪽에 집중되어 있었고, 이번 패치가 정확히 그 경로를 막는다.

### 3.6 ⚠️ W2 — 취소 플래그가 핸드셰이크 단계에서 확인되지 않음

`cancel()`은 `cancelled_ = true`를 세우고 소켓에 `shutdown()`을 건다. 그러나 **`cancelled_`를 검사하는 곳은 `run()`의 루프 상단뿐**이며, `connect()`/`setup()`/`play()`는 이를 확인하지 않는다.

핸드셰이크 도중(특히 `activeClient_` 등록 직후, `connect()` 호출 직전)에 `stop()`이 도착하면:

- `cancel()` 시점에 `cancelFd_`가 아직 `-1`이라 소켓 차단 효과가 없다
- `connect()`가 **새 소켓**을 만들고 핸드셰이크를 끝까지 진행한다
- **최악의 경우 지연** ≈ `connectTimeoutSec`(5s) + `recvTimeoutSec` × 3회(SETUP 2회 + PLAY 1회, 각 5s) ≈ **약 20초**
- 그제서야 `run()`이 진입 즉시 `cancelled_`를 보고 break

**영향**: 교착이 아니라 **종료 지연**이다. systemd 기본 `TimeoutStopSec`(90초) 안에는 들어오므로 SIGKILL로 이어지지는 않지만, 종료가 최대 20초 늘어질 수 있다. `connect()`/`setup()`/`play()` 진입부에 `cancelled_` 조기 반환을 넣으면 즉시 종료된다.

### 3.7 ✅ 종료 시맨틱 — 잔여 프레임 배출과 계약 준수

`next()`의 술어가 `count_ > 0 || stopping_`이므로, `stop()` 이후에도 **링에 남은 프레임은 모두 배출**된 뒤 `count_ == 0`에서 `false`를 반환한다. 이는 `IMetadataSource`의 계약("호출 이후 `next()`는 남은 버퍼를 모두 내보낸 뒤 `false`를 반환")과 정확히 일치한다. ✅

계약상 금지된 "`false` 이후 재호출"이 실제로 일어나도 `stopping_`이 참이라 술어가 즉시 만족되고 `count_ == 0`으로 다시 `false`를 반환한다 — **UB나 블로킹이 아니다.** ✅

---

## 4. 성능 최적화 (Zero-copy 검증)

### 4.1 ✅ `std::swap` 핸드오프의 실효성 검증

```cpp
domain::RawPacket& slot = ring_[head_];
out.channelId = slot.channelId;
std::swap(out.bytes, slot.bytes);   // O(1) 포인터 3개 교환 (data/size/capacity)
out.recvTime  = slot.recvTime;
```

`std::vector`의 swap은 **내부 포인터 교환**이므로 페이로드 크기와 무관하게 O(1)이다. 검증된 이득:

| 항목 | 효과 |
|------|------|
| **per-frame memcpy 제거** | 수 KB 복사 → 포인터 3개 교환 |
| **capacity 왕복 재사용** | 소비 완료된 `out`의 버퍼가 슬롯으로 돌아가 **다음 write의 capacity가 됨** |
| **정상 상태 힙 할당** | warmup 이후 **0** (양쪽 버퍼가 고수위 capacity에 수렴) |

**전제 조건이 실제로 지켜지는지 확인함** — `main.cpp`가 `domain::RawPacket raw;`를 **루프 밖**에 선언하고 재사용한다. 매 반복 새로 만들면 스왑으로 돌려받은 capacity가 즉시 버려져 이 최적화가 무효화된다.

```cpp
domain::RawPacket raw;                        // ★ 루프 밖 — 최적화의 전제
while (context->source().next(raw)) { ... }
```

> CLAUDE.md의 "capture→pipeline handoff는 `std::swap`" 원칙이 이 지점을 가리킨다. `assign`/복사로 되돌리면 이 두 이득이 동시에 사라진다.

### 4.2 ℹ️ 남아 있는 복사 1회 — 프로듀서 경계 (의도적)

```cpp
slot.bytes.assign(payload.begin(), payload.end());   // ← 유일한 복사
```

- **왜 제거할 수 없는가**: 콜백이 받는 `payload`는 `RtspClientV2`가 소유한 `metadataBuffer`(`std::string`)를 가리키는 `string_view`이며, 클라이언트는 콜백 반환 직후 `clear()`로 **즉시 재사용**한다. 소유권이 없으므로 swap이 불가능하고, 타입도 다르다(`std::string` ↔ `std::vector<uint8_t>`).
- **비용**: 페이로드 크기만큼의 memcpy(실측 수 KB → 수 µs 미만).
- **할당은 없다**: `assign`은 슬롯의 기존 capacity를 재사용하므로 **콜백당 힙 할당 0**이다(고수위 도달 후).

### 4.3 ⚠️ W3 — 프로듀서가 락을 쥔 채 복사함

위 `assign`은 **`mtx_`를 잡은 상태에서** 수행된다. 즉 복사 시간만큼 컨슈머가 대기한다.

- **현재 영향**: 무시할 수준. 5fps × 수 KB → 락 점유 시간 수 µs, 경합률 사실상 0.
- **이론적 최악**: 손상된 스트림이 1 MiB 프레임을 만들면 그 memcpy 동안 락이 잡힌다(그래도 ms 단위).
- **개선 방향(선택)**: 락 밖에서 스테이징 버퍼에 복사한 뒤 락 안에서 슬롯과 swap하면 임계 구역이 O(1)로 줄어든다. 다만 스테이징 버퍼가 하나 더 필요하고 현재 측정상 이득이 없어 **현 설계가 합리적**이다.

### 4.4 ✅ CPU 스핀 없음 / 락 유지 시간 최소화

- **컨슈머는 스핀하지 않는다**: lock-free 대신 `mtx_` + `cv_`를 **의도적으로** 선택해, 유휴 시 컨슈머가 **잠들게** 했다. 라즈베리파이의 제한된 CPU에서 바쁜 대기는 명백한 낭비다.
- **로그를 락 밖에서 출력한다**: `buildMetricsReportIfDue()`는 락 안에서 **문자열만 만들어 반환**하고, 실제 `logSuccess()` 호출은 락 해제 후 수행된다. ✅

```cpp
{   std::unique_lock<std::mutex> lk(mtx_);
    ...
    report = buildMetricsReportIfDue();      // 문자열 생성만
}
if (!report.empty()) { logSuccess(kIface, report); }   // I/O는 락 밖
```

- **`% ringCapacity_` 런타임 나눗셈**: `ringCapacity_`가 런타임 값이 되며 컴파일타임 비트마스크로 접히지 않지만, 콜백/`next()`당 각 1회뿐이라 측정 지표에 변화가 없었다.

### 4.5 ✅ 관측 가능성 — 병목이 지표로 드러남

| 지표 | 의미 |
|------|------|
| `droppedCount` **증가** | 컨슈머(Pipeline)가 프레임 레이트를 못 따라감 → `sourceRingCapacity` 조정 또는 하류 최적화 필요 |
| `totalQueueLatency` / `consumedCount` | 네트워크 도착 → Pipeline 인출까지의 평균 큐 지연 |
| `producedCount` vs `consumedCount` | 생산/소비 균형 |

드랍이 **은폐되지 않고 계측·로깅**된다는 점이 중요하다 — 무언의 프레임 손실은 안전 시스템에서 가장 위험한 실패 양상이다.

---

## 5. 감사 요약

| 영역 | 판정 | 근거 |
|------|------|------|
| **스레드 안전성 / 경합** | ✅ **All Clear** (W1 패치 완료) | 링버퍼·지표는 `mtx_`로 완전 보호, 두 뮤텍스 중첩 없어 교착 없음, `sessionProductive`는 단일 스레드라 안전. **`stop()`의 유실 기상 창은 패치로 제거됨(§3.5)** |
| **OOM 위험** | ✅ **All Clear** | 고정 용량 링 + drop-oldest로 **컨슈머 stall 시에도 메모리 상한 보장**(최악 8 MiB). 0 나눗셈은 `clampPositive`가 차단 |
| **자원 누수 / UAF** | ✅ **All Clear** | 워커 join 보장, 소멸자 본문에서 join 후 멤버 파괴, `activeClient_` 널 처리 계약, 세션별 RAII로 fd/스레드 누수 없음 |
| **I/O 병목 / 락 경합** | ✅ **All Clear** (W3 경미) | 스핀 없음(CV 수면), 로그는 락 밖, 임계 구역 수 µs. 프로듀서가 락을 쥔 채 복사하나 현 부하에서 무시 가능 |
| **성능 최적화 (zero-copy)** | ✅ **검증 완료** | `std::swap` O(1) 교환 + capacity 왕복 재사용, `main`의 `raw` 루프 밖 선언으로 전제 조건 충족, 정상 상태 힙 할당 0 |

### 지적 사항 목록

| # | 항목 | 등급 | 영향 | 상태 |
|---|------|------|------|------|
| **W1** | `stop()`의 유실 기상 창 (`notify_all` 전 `mtx_` 미획득) | Warning (최우선) | 낮은 확률로 `next()` 영구 블로킹 → 종료 불가 → MQTT dead 신호 미발행 | ✅ **패치 완료** — `notify_all()` 직전 `mtx_` 획득 (§3.5) |
| **W2** | `cancelled_`가 connect/setup/play에서 미확인 | Warning | 종료 지연 최대 ~20초 (교착 아님) | ⬜ 미조치 — 각 진입부 조기 반환 권고 |
| **W3** | 프로듀서가 `mtx_`를 쥔 채 페이로드 복사 | Info | 현 부하에서 무시 가능 | ⬜ 미조치 — 현재 불필요 (측정상 이득 없음) |
| **I1** | Source가 `ringCapacity_`를 재검증하지 않음 (`AppConfig` 의존) | Info | 정상 경로 안전. 설정 계층 우회 시 0 나눗셈 가능 | ⬜ 미조치 — 생성자 가드 1줄로 해소 가능 |

**총평**: Source 계층의 **메모리 상한 보장과 zero-copy 핸드오프는 설계대로 정확히 동작**하며, UAF·누수·교착에 대한 방어도 견고하다. Critical 결함은 없었고, 최우선 지적 사항이던 **W1(유실 기상)은 패치로 해소**되어 `stop()` → `next()` 기상이 결정론적으로 보장된다. 이로써 조건변수 알림 방어 패턴이 `MqttFrameSink`·`MqttTransport`·`RtspOnvifSourceV2` **세 곳에서 일관**되게 적용되었다. 잔여 항목(W2/W3/I1)은 모두 정보성 또는 선택적 개선 사항이다.
