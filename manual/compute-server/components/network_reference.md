# Network 모듈 레퍼런스 (저수준 I/O & 프로토콜)

> **대상 파일**
> - 인터페이스: `include/interfaces/INetwork.h`
> - 구현체: `src/network/RtspClientV2.h`, `.cpp`

CCTV와 **직접 TCP 소켓을 맺고 RTSP 프로토콜로 대화하는 최하위 계층**이다. 이 계층의 책임은 딱 하나 — **"단일 연결 시도의 생명주기"** (connect → setup → play → run)를 수행하고, 재조합된 메타데이터 payload를 콜백으로 밀어올리는 것

**재연결·백오프·버퍼링 정책은 이 계층의 책임이 아니다.** 그것은 상위 Source 어댑터가 담당한다. `run()`은 연결이 끊기면 **그냥 리턴**하며, 스스로 재시도하지 않는다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | INetwork 인터페이스 및 RtspClientV2 구현체 분석 (소켓 I/O, RTSP 프로토콜 통신 및 연결 수명주기 명세) |

---

## 1. 인터페이스 명세 (Interface Contract)

```cpp
class INetwork {
public:
    virtual ~INetwork() = default;

    virtual bool connect() = 0;   ///< CCTV와 TCP 연결 수립
    virtual bool setup() = 0;     ///< RTSP SETUP (Digest 인증 포함)
    virtual void play() = 0;      ///< RTSP PLAY 로 스트리밍 시작 + keepalive 구동
    virtual void run() = 0;       ///< 인터리브 스트림 블로킹 수신 루프 (재연결 없음)

    /// @brief Metadata payload 하나가 재조합될 때마다 호출되는 콜백
    std::function<void(std::string_view)> onPayloadReceived;
};
```

| 항목 | 계약 내용 |
|------|-----------|
| **역할 분담** | `IMetadataSource`가 Pipeline이 보는 **pull** 인터페이스라면, `INetwork`는 RTSP 프로토콜 자체(Digest 인증, TCP 인터리브 프레이밍)를 감춘 **push** 인터페이스 |
| **생명주기 범위** | *단일 연결 시도*만 책임진다. `run()`은 오류/끊김 시 리턴하며 **재연결하지 않음** |
| **콜백 스레드** | `onPayloadReceived`는 `run()`을 부르는 스레드에서 **동기 호출**된다. 콜백에서 오래 걸리면 **다음 RTP 패킷 수신이 지연**되므로, 구현 측은 버퍼 복사 정도만 해야 함 |
| **`@todo` (설계 부채)** | 현재 인터페이스가 RTSP에 지나치게 치중되어 있음 <br> MQTT 등 다른 프로토콜로 교체 시 재설계가 필요할 수 있다고 헤더에 명시되어 있음 |

---

## 2. 구현체 분석: `RtspClientV2`

### 2.1 TCP 연결 수명주기 (`connect()`)

소켓 생성부터 블로킹 모드 복원까지 한 함수에서 처리한다. 핵심은 **명시적 타임아웃**이다.

```cpp
sock_ = socket(AF_INET, SOCK_STREAM, 0);
cancelFd_.store(sock_, std::memory_order_release);   // 취소 스레드에 fd 공개

setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, ...);        // recv 타임아웃 (기본 5초)
setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &noDelay, ...);  // Nagle 지연 제거
setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &socketRecvBufBytes_, ...);
```

**① 주소 검증** — `inet_pton`의 반환값을 반드시 검사한다. 검사하지 않으면 `sin_addr`가 0인 채 **`0.0.0.0`에 접속을 시도**해 원인 모를 실패로 이어진다. 점 표기 IPv4만 지원 (호스트명 미지원)

**② 논블로킹 connect + `select()`** — 이 계층에서 `select()`가 쓰이는 **유일한 지점**이다.

```cpp
const int origFlags = fcntl(sock_, F_GETFL, 0);
fcntl(sock_, F_SETFL, origFlags | O_NONBLOCK);       // 논블로킹 전환

const int connectResult = ::connect(...);
if (connectResult < 0 && errno != EINPROGRESS) { ... 실패 ... }

if (connectResult != 0) {
    fd_set writeSet; FD_ZERO(&writeSet); FD_SET(sock_, &writeSet);
    struct timeval connectTimeout { connectTimeoutSec_, 0 };
    const int selectResult = select(sock_ + 1, nullptr, &writeSet, nullptr, &connectTimeout);
    if (selectResult <= 0) { ... 타임아웃 ... }

    int sockErr = 0; socklen_t len = sizeof(sockErr);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, &sockErr, &len);   // 실제 연결 결과 확인
    if (sockErr != 0) { ... 실패 ... }
}
fcntl(sock_, F_SETFL, origFlags);   // 이후 경로는 블로킹 전제 -> 원복
```

> **왜 `select()`가 필요한가**: `SO_RCVTIMEO`는 **connect에 적용되지 않는다.** 이 명시적 제한이 없으면 카메라 무응답이나 방화벽 SYN drop 시 **커널 기본 타임아웃(1~2분)까지 아무 로그 없이 블로킹**된다. `select()` 성공 후에도 `SO_ERROR`를 확인해야 실제 연결 성공 여부를 알 수 있다.
>
> **폴링이 아니다**: `select()`는 connect 단계에서 **단발성**으로만 쓰인다. 이후 스트림 수신은 블로킹 `recv()`이므로 매 패킷 폴링하는 CPU 낭비가 없다.

**③ fd 즉시 반납 (`closeSocket()`)** — `connect()`의 **모든 실패 경로**에서 호출된다.

```cpp
void RtspClientV2::closeSocket() noexcept {
    cancelFd_.store(-1, std::memory_order_release);  // close '전에' 무효화
    if (sock_ != -1) { close(sock_); sock_ = -1; }
}
```

소멸자에만 맡기면 **같은 인스턴스로 `connect()`를 재시도하는 순간 이전 fd가 새어나간다.** 멱등이며, `cancelFd_`를 먼저 -1로 만들어야 `cancel()`이 이미 닫힌(또는 번호가 재사용된) fd에 `shutdown`을 걸지 않는다.

### 2.2 취소 경로 — `shutdown()`이 반드시 필요한 이유

```cpp
void RtspClientV2::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
    const int fd = cancelFd_.load(std::memory_order_acquire);
    if (fd != -1) ::shutdown(fd, SHUT_RDWR);   // 블로킹 recv()를 즉시 깨움
}
```

**스트림이 건강한 동안에는 `recv()`가 계속 성공해 타임아웃이 나지 않는다.** 따라서 `cancelled_` 플래그만 세우면 `run()`이 영영 반환하지 않고, 상위 워커의 `join`이 무한 대기한다.

> **`close()`가 아니라 `shutdown()`인 이유**: fd의 소유와 close는 소유자 스레드(워커/소멸자)의 몫이다. 다른 스레드가 `close`하면 **fd 번호 재사용 경합**이 생긴다. `shutdown()`은 fd를 닫지 않고 연결만 끊으므로, 블로킹 중인 `recv()`가 즉시 0/-1로 반환된다.

### 2.3 RTSP / HTTP 헤더 파싱

#### `recvHeaders()` — 저빈도 제어 경로 전용

```cpp
bool RtspClientV2::recvHeaders(std::string& out) {
    out.clear();
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos) {
        const int n = recv(sock_, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 65536) return false;   // 악성 무한 헤더 방어 (64 KiB 하드 상한)
    }
    return true;
}
```

헤더 종료(`\r\n\r\n`)까지 누적하며, **64 KiB 상한**으로 악의적 무한 헤더를 차단한다. SETUP/PLAY 같은 저빈도 경로에만 쓰이므로 별도 버퍼 최적화를 하지 않는다.

#### 헤더 값 추출

`find` + `substr` 조합으로 필요한 값만 뽑는다. `substr`은 길이를 남은 크기로 **클램프**하므로, 닫는 따옴표나 구분자가 없어도 OOB가 발생하지 않는다.

| 값 | 추출 방식 |
|----|-----------|
| `realm` | `res1.find("realm=\"")` → 다음 `"`까지 |
| `nonce` | `res1.find("nonce=\"")` → 다음 `"`까지. **비어 있으면 즉시 실패** |
| `Session` | `res2.find("Session: ")` → `find_first_of(";\r\n")`까지 |
| 상태 줄 | `statusLine()` 헬퍼가 첫 `\r\n` 앞부분만 잘라냄 (진단용) |

**실패 시 진단 품질**: 세션 ID를 못 얻으면 상태 줄을 함께 남겨 원인을 구분한다 — `401`=인증 거부(계정/비밀번호), `454/455/461`=URI/Transport 문제. 이 클라이언트는 **DESCRIBE 없이 곧바로 SETUP**하므로 `rtspSetupUri`가 정확해야 한다.

### 2.4 Digest 인증 (RFC 2617)

`setup()`은 **2단계 핸드셰이크**다: 1차 SETUP(무인증) → `401`로 받은 `realm`/`nonce`로 Authorization 헤더 생성 → 2차 SETUP.

```cpp
const std::string ha1 = md5Hex(user + ":" + realm_ + ":" + pass);
const std::string ha2 = md5Hex(method + ":" + uri);
const std::string response = md5Hex(ha1 + ":" + nonce_ + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
```

**세션마다 무작위 `cnonce`** — 생성자에서 `std::random_device` + `std::mt19937`로 생성한다. 하드코딩된 고정 cnonce는 Digest challenge-response를 **재전송 공격(replay)** 에 취약하게 만든다.

```cpp
std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<std::uint32_t> dist;
snprintf(cnonceBuf, sizeof(cnonceBuf), "%08x", dist(gen));
cnonce_ = cnonceBuf;
```

`nonceCount_`(nc)는 요청마다 증가하며 `%08x`로 포맷된다.

> **⚠️ 평문 전송 주의**: RTSP 구간에는 TLS가 적용되지 않는다. (TLS는 MQTT 계층 전용) Digest가 비밀번호 자체는 해시로 보호하지만 nonce/response는 도청 가능하므로, **RTSP 구간은 신뢰된 LAN/VLAN 격리**를 전제로 한다.

### 2.5 Payload 버퍼링 (성능 핵심)

#### 사용자 공간 버퍼링 recv

`recv()` syscall을 패킷마다 부르지 않고, **64 KiB 단위(`readBufBytes_`)로 한 번에 채워** 사용자 공간에서 잘라 쓴다.

```cpp
bool fillReadBuffer();                              // recv() syscall 1회로 sockBuf_ 를 채움
bool readByte(std::uint8_t& out);                   // 버퍼에서 1바이트 (비면 fill)
bool readBytes(std::uint8_t* dest, std::size_t len);// 정확히 len 바이트 (resize 없이)
```

`sockBufLen_`(유효 길이)과 `sockBufPos_`(읽기 커서)로 상태를 관리하며, **동적 할당은 연결당 1회 `resize`뿐**이다. `readBytes`는 미리 확보된 버퍼에 `memcpy`하므로 **매 패킷 `vector::resize`(재할당 + zero-init)가 발생하지 않는다.**

#### TCP 인터리브 프레이밍 (`run()`)

RTSP over TCP는 `$ | channel | length(2B) | RTP packet` 형태로 제어 채널에 미디어를 끼워 보낸다.

```cpp
while (!cancelled_.load(std::memory_order_acquire)) {   // 매 반복 취소 확인
    std::uint8_t sync = 0;
    if (!readByte(sync)) break;
    if (sync != '$') continue;                          // 동기 바이트 검증

    std::uint8_t header[3];
    if (!readBytes(header, 3)) break;
    const int channel    = header[0];
    const int payloadLen = (header[1] << 8) | header[2];

    if (static_cast<std::size_t>(payloadLen) > kMaxRtpPayloadSize) break;  // 프레이밍 손상
    if (!readBytes(rtpPacket_.data(), payloadLen)) break;
    if (channel != 0) continue;                         // 메타데이터 채널만
    if (payloadLen < kRtpHeaderSize) continue;          // RTP 헤더도 안 되는 크기

    const bool marker = (rtpPacket_[1] & 0x80) != 0;    // RTP marker bit = 프레임 끝
    metadataBuffer.append(rtpPacket_.data() + kRtpHeaderSize, payloadLen - kRtpHeaderSize);

    if (metadataBuffer.size() > maxMetadataFrameSize_) { ... 폐기 + break ... }

    if (marker) {
        if (onPayloadReceived) onPayloadReceived(std::string_view(metadataBuffer));
        metadataBuffer.clear();                          // capacity 유지
    }
}
```

- **취소 플래그를 매 반복 확인** — 이것이 없으면 무한 대기가 발생한다.
- **`rtpPacket_`은 고정 버퍼**(생성 시 `kMaxRtpPayloadSize`로 1회 `resize`) → 패킷당 할당 0.
- **`metadataBuffer`는 `reserve(8192)` 후 `clear()` 재사용** → 프레임당 재할당 없음.
- **marker bit**가 프레임 경계다. RTP payload를 marker가 설 때까지 이어붙여 하나의 ONVIF XML 문서를 재조합한 뒤 콜백으로 올린다.

#### 프로토콜 불변 상수 vs 튜닝 노브

| 구분 | 값 | 설정 노출 |
|------|-----|-----------|
| `kRtpHeaderSize` = 12 | RTP 헤더 크기 | ❌ 프로토콜 불변 (바꾸면 파싱이 깨짐) |
| `kMaxRtpPayloadSize` = 65535 | 인터리브 2바이트 length 필드의 최댓값 | ❌ 프로토콜 불변 |
| `readBufBytes_` (기본 64 KiB) | recv 단위 → 프레임당 syscall 수 좌우 | ✅ `AppConfig` |
| `maxMetadataFrameSize_` (기본 1 MiB) | marker 누락/손상 시 무한 누적 방지 상한 | ✅ `AppConfig` |
| `connectTimeoutSec_` / `recvTimeoutSec_` | 연결/수신 타임아웃 | ✅ `AppConfig` |
| `socketRecvBufBytes_` | `SO_RCVBUF` | ✅ `AppConfig` |
| `keepAliveIntervalSec_` | GET_PARAMETER 주기 | ✅ `AppConfig` |

> 튜닝 노브의 기본값은 `performance/compute-server.md`에 측정된 값 그대로다. 건드리면 문서의 지표를 다시 측정해야 한다.

**프레임 상한 초과 시 동작**: `maxMetadataFrameSize_`를 넘으면 marker 누락이나 스트림 손상으로 간주하고, 버퍼를 버린 뒤 `run()`을 종료한다. → 상위 Source의 재연결 백오프에 위임

### 2.6 `play()` — PLAY 응답 검증이 필수인 이유

```cpp
const std::string status = statusLine(res);
if (status.find(" 200") == std::string::npos) {
    logError(kIface, "PLAY 실패 - 응답: [" + status + "] ... PLAY는 트랙이 아니라 "
                     "세션 aggregate URL(끝의 trackID/슬래시 없이)로 보내야 함");
    return;                        // playOk_ 는 false 로 남음
}
playOk_ = true;                    // 여기까지 와야 스트리밍이 실제로 시작됨
keepRunning_ = true;
keepaliveThread_ = std::thread(&RtspClientV2::keepAliveLoop, this);
```

`playSucceeded()`가 `false`면 상위 워커는 `run()`에 **진입하지 않는다.** 예전에는 PLAY 실패에도 `run()`을 불렀고, 카메라가 곧바로 연결을 끊어 즉시 리턴 → **백오프가 매번 1초로 리셋되는 재접속 폭풍**이 발생했다.

### 2.7 Keep-alive

```cpp
void RtspClientV2::keepAliveLoop() {
    while (keepRunning_) {
        // 1초씩 쪼개서 자는 이유: 종료 요청에 최대 1초 안에 반응하기 위함
        for (int i = 0; i < keepAliveIntervalSec_ && keepRunning_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!keepRunning_) break;

        ... GET_PARAMETER 전송 (실패 시 break) ...
    }
}
```

기본 30초 주기로 `GET_PARAMETER`를 보내 RTSP 세션을 유지한다. **긴 sleep을 1초 단위로 쪼개** 종료 요청에 빠르게 반응한다(스핀이 아니라 sleep이므로 CPU 낭비 없음). 소멸자에서 `keepRunning_ = false` 후 반드시 `join`된다.

### 2.8 SIGPIPE 방어 — 모든 송신 경로

카메라가 이미 연결을 끊은 상태에서 `send()`를 부르면 기본 동작상 **SIGPIPE로 프로세스가 죽는다.** 이 계층의 **네 개 송신 경로 전부**가 `MSG_NOSIGNAL`을 쓰고 반환값을 검사한다.

| 송신 경로 | 보호 |
|-----------|------|
| `setup()` 1차 SETUP | `send(..., MSG_NOSIGNAL)` + 반환 검사 |
| `setup()` 2차(인증) SETUP | `send(..., MSG_NOSIGNAL)` + 반환 검사 |
| `play()` PLAY | `send(..., MSG_NOSIGNAL)` + 반환 검사 |
| `keepAliveLoop()` GET_PARAMETER | `send(..., MSG_NOSIGNAL)` + 반환 검사 |

`main.cpp`의 전역 `std::signal(SIGPIPE, SIG_IGN)`가 이중 안전망으로 깔려 있다.

### 2.9 성능 지표

`reportMetricsIfDue()`가 주기적으로 **평균 프레임 조립 시간 / 프레임당 `recv()` 호출 수 / fps / 처리량(KB/s)** 을 로그로 남긴다. 특히 **프레임당 recv 호출 수**는 버퍼링 recv가 실제로 syscall을 줄이고 있는지 확인하는 지표다.

### 2.10 종료와 자원 회수 (RAII)

```cpp
RtspClientV2::~RtspClientV2() {
    keepRunning_ = false;
    if (keepaliveThread_.joinable()) keepaliveThread_.join();
    const bool wasOpen = (sock_ != -1);
    closeSocket();
    if (wasOpen) logSuccess(kIface, "소켓 정상 종료");
}
```

keepalive 스레드를 먼저 join한 뒤 소켓을 닫는다. 상위 Source가 세션마다 **지역 인스턴스**로 이 클래스를 쓰므로, 재연결 루프가 돌아도 fd·스레드 누수가 없다.

---

## 3. Edge-Worker 원칙

- **채널 단일성**: `RtspClientV2`는 자기 채널의 접속 정보(`AppConfig`의 `rtspIp`/`rtspPort`/`rtspSetupUri`/`rtspPlayUri`/계정) **하나만** 안다. `channelCount`나 다른 카메라의 존재를 전혀 모른다.
- **원본 바이트만 운반**: 이 계층이 나르는 것은 카메라가 보낸 **ONVIF 메타데이터 원본 바이트**다. (디코딩된 영상이 아님) 내용 해석은 파서의 몫이고, 좌표의 의미 부여는 그보다 더 하류의 몫이다.
- **graceful shutdown의 출발점**: `cancel()`의 `shutdown(SHUT_RDWR)` 경로가 있어야 SIGINT/SIGTERM에 즉시 응답하고 **MQTT 종료 신호(dead)** 를 정상 발행할 수 있다. 이게 없으면 systemd가 SIGKILL로 강제 종료해, control-server가 채널의 생사를 구분하지 못한다.