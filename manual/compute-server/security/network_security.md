# Network 클라이언트 계층 보안 권고 (Security Advisory)

> **대상 모듈**
> - Push 인터페이스: `include/interfaces/INetwork.h`
> - RTSP 클라이언트: `src/network/RtspClientV2.h`, `.cpp`

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-07-27 | 1.0.0 | Mangjun | 네트워크/RTSP 클라이언트 계층 I/O 병목 및 보안 취약점 감사, 성능 최적화 명세 |

---

## 1. 위협 모델 (Threat Model)

이 계층은 **CCTV라는 외부 신뢰 불가 엔드포인트와 직접 소켓을 맺는 최전선**이다. 파서(`OnvifParser`)가 *바이트열의 내용*을 다룬다면, 이 계층은 그 바이트열이 도착하기까지의 **소켓·버퍼·스레드·프로토콜 상태**를 다룬다.

- **공격 표면**: 카메라는 현장 장비라 탈취·스푸핑·펌웨어 변조가 가능하고, 네트워크 경로상 **중간자(MITM)** 도 가정 가능하다. 공격자는 (1) 임의의 RTSP/인터리브 바이트, (2) 악의적 응답 헤더, (3) 연결 지연·부분 전송, (4) 무응답/급단절을 던질 수 있다.
- **자산**: `rtspUser`/`rtspPass`(Digest 자격증명), 워커 스레드의 생존성(멈추면 채널 위험 판정 중단), 라즈베리파이의 제한된 메모리/CPU.
- **설계 목표**: "어떤 소켓 상태·응답에도 **스레드가 멈추지 않고**, **메모리를 무한정 쓰지 않으며**, **프로세스가 죽지 않고**, 취소 시 **즉시·안전하게 정리**된다."

### 신뢰 경계와 책임 분리

`INetwork`(push)는 *단일 연결 시도*(connect → setup → play → run)만 책임지고, **재연결/백오프는 상위 `RtspOnvifSourceV2`(pull 어댑터)** 가 맡는다. 이 분리 덕분에 `run()`은 "끊기면 그냥 리턴"이라는 단순 계약을 지키고, 재시도 폭풍 방지 같은 정책은 한 곳(Source)에 모인다.

> **전송 암호화 주의**: 이 계층의 RTSP 트래픽은 **평문**이다. (TLS는 MQTT 계층에만 적용) Digest 인증(RFC 2617)이 비밀번호 자체는 해시로 보호하지만, nonce/response는 도청 가능하다. 따라서 **RTSP 구간은 신뢰된 로컬 네트워크(LAN/VLAN 격리)** 를 전제로 하며, 이는 배포상의 요구사항이다. `config.json`의 자격증명은 평문으로 저장되므로 파일 권한 관리가 필요하다.

---

## 2. OOM(메모리 고갈) & 스택 오버플로 방어

### 2.1 모든 수신 버퍼가 고정·상한 (Unbounded 없음)

이 계층에는 **무한정 자라는 버퍼가 존재하지 않는다.** 모든 저장소가 컴파일타임 고정이거나 명시적 상한을 가진다.

| 버퍼 | 크기/상한 | 근거 |
|------|-----------|------|
| `sockBuf_` (사용자 공간 recv 버퍼) | 고정 `rtspReadBufBytes_` (기본 64 KiB) | 연결당 1회 `resize`, 이후 재사용 |
| `rtpPacket_` (RTP payload 재조합) | 고정 `kMaxRtpPayloadSize` = 65535 | 인터리브 2바이트 length 필드의 최댓값 |
| `recvHeaders` 누적 문자열 | **64 KiB 하드 상한** (`out.size() > 65536 → return false`) | 악성 무한 헤더 방어 |
| `metadataBuffer` (프레임 조립) | **`maxMetadataFrameSize_` 상한** (기본 1 MiB) 초과 시 폐기+연결 재수립 | marker 누락/스트림 손상으로 인한 무한 누적 차단 |
| `ring_` (Source 링버퍼) | 고정 `sourceRingCapacity` | drop-oldest로 지연·메모리 무한 누적 방지 |

> **핵심**: RTP 인터리브 프레임 크기는 프로토콜상 2바이트(≤65535)로 제한되고, 조립 프레임은 1 MiB에서 끊긴다. 공격자가 "거대한 프레임"으로 메모리를 고갈시키려 해도 **1 MiB에서 버퍼를 버리고 연결을 재수립**한다. (상위 Source의 재연결 백오프에 위임)

### 2.2 연결 드롭 시 자원 회수 (누수 없음)

- **`closeSocket()`은 멱등**이며 `connect()`의 **모든 실패 경로**에서 즉시 호출된다. 소멸자에만 의존하지 않으므로, 같은 인스턴스로 재시도해도 이전 fd가 새지 않는다.
- `RtspOnvifSourceV2::workerLoop`는 세션마다 **지역 `RtspClientV2` 인스턴스**를 쓰므로, 루프 반복 시 RAII로 소켓·스레드가 정리된다.
- `keepaliveThread_`는 소멸자에서 `keepRunning_=false` 후 반드시 `join`된다.
- **결론**: 반복적인 연결/드롭 사이클에서도 fd·스레드·힙 누수가 없다.

### 2.3 스택 오버플로 — 구조적 면역

- 이 계층에는 **재귀가 전혀 없다.** 스트림 읽기(`run`, `readBytes`, `fillReadBuffer`)와 헤더 파싱(`recvHeaders`)은 모두 **평면 루프**다.
- 스택 사용은 소량의 고정 지역 버퍼뿐이다: `buf[4096]`(헤더 수신), `header[3]`(인터리브 헤더), `cnonceBuf[9]`, `output[33]`(MD5 hex), `header[1024]`(Digest 헤더), `ncBuf[9]`. 입력 크기에 따라 스택이 깊어지는 지점이 없다.
- **결론**: 아무리 크거나 악의적인 스트림에도 스택 고갈이 발생하지 않는다.

---

## 3. I/O 성능 & 병목 분석

### 3.1 블로킹 호출에 반드시 타임아웃 (스레드 무한 대기 방지)

| 지점 | 타임아웃 메커니즘 |
|------|-------------------|
| `connect()` | **논블로킹 connect + `select()`** 에 `connectTimeoutSec_` 명시 <br> `SO_RCVTIMEO`는 connect에 적용되지 않으므로, 이게 없으면 카메라 무응답/방화벽 SYN drop 시 커널 기본(1~2분)까지 무한 대기 |
| `recv()` (헤더/스트림) | 소켓에 `SO_RCVTIMEO`(`recvTimeoutSec_`, 기본 5초) 설정 |
| `next()` (consumer) | 조건변수 `cv_.wait` — 패킷 도착 또는 `stopping_` 시 깨어남 |
| 취소 | `cancel()`의 `shutdown(SHUT_RDWR)`로 블로킹 `recv()`를 **즉시** 깨움 |

### 3.2 폴링/스핀 없음 (CPU 낭비 방지)

- `select()`는 **connect 단계에서 단발성**으로만 쓰인다(단일 fd). 패킷 수신은 블로킹 `recv`라 매 패킷 폴링하지 않는다.
- **consumer는 스핀하지 않는다**: `next()`는 CV로 잠들었다가 알림에만 깨어난다. (라즈베리파이에서 바쁜 대기 회피)
- **keepalive는 스핀하지 않는다**: `keepAliveIntervalSec_`(기본 30초)를 **1초 단위로 쪼개 sleep**하며 종료 요청에 최대 1초 내 반응

### 3.3 취소 지연 없음 (graceful shutdown)

**스트림이 건강한 동안 `recv()`는 계속 성공해 타임아웃이 나지 않는다.** 따라서 취소 플래그(`cancelled_`)만으로는 `run()`이 영영 반환하지 않는다. `cancel()`이 **소켓 `shutdown(SHUT_RDWR)`** 을 함께 걸어 blocking `recv()`를 즉시 풀어, 워커 `join`이 `recv` 타임아웃(5초)만큼 지연되지 않는다. → SIGINT/SIGTERM에 즉시 응답하고 MQTT 종료 신호를 정상 발행

### 3.4 병목 완화 기법

- **사용자 공간 버퍼링 recv**(`sockBuf_` 64 KiB): 패킷당 syscall 수를 크게 줄임
- **`TCP_NODELAY`**: Nagle 지연 제거. **`SO_RCVBUF`**: 커널 수신 버퍼 확대
- **고정 `rtpPacket_` 재사용**: 매 패킷 `vector::resize`(재할당+zero-init) 제거
- 성능 기준값은 `performance/compute-server.md`에 측정된 값이며, 위 튜닝 노브의 기본값이 그 측정 조건과 일치한다.

---

## 4. 이 계층에 적용된 보안 방어 (Security Defenses)

### 4.1 SIGPIPE 방어 — 계층 내 모든 `send()` 경로 확인 완료

카메라가 이미 연결을 끊은 상태에서 `send()`를 호출하면 기본 동작상 **SIGPIPE로 프로세스가 죽는다.** 이 계층의 **모든 송신 경로**가 `MSG_NOSIGNAL`로 보호되며 반환값도 검사한다.

| 송신 경로 | 보호 |
|-----------|------|
| `setup()` 1차 SETUP 요청 | `send(..., MSG_NOSIGNAL)` + 반환 검사 |
| `setup()` 2차(인증) SETUP 요청 | `send(..., MSG_NOSIGNAL)` + 반환 검사 |
| `play()` PLAY 요청 | `send(..., MSG_NOSIGNAL)` + 반환 검사 |
| `keepAliveLoop()` GET_PARAMETER | `send(..., MSG_NOSIGNAL)` + 반환 검사 |

> 감사 결과 이 계층에 `MSG_NOSIGNAL` 없이 쓰기를 수행하는 **다른 send/write 경로는 없다.** 또한 `main.cpp`가 전역 `signal(SIGPIPE, SIG_IGN)`를 이중 안전망으로 설치해, 향후 누락된 경로가 생겨도 프로세스가 죽지 않도록 방어 심층화되어 있다.

### 4.2 소켓 취소의 스레드 안전성 (cross-thread `close` 경합 차단)

- 취소는 `close()`가 아니라 **`shutdown()`**을 쓴다. fd의 소유·close는 소유자 스레드(소멸자/워커)의 몫이며, 다른 스레드가 `close`하면 **fd 번호 재사용 경합**이 생긴다.
- `cancelFd_`(atomic)로 fd를 공유하되, `closeSocket()`이 **close 직전에 `cancelFd_ = -1`** 로 무효화해, 이미 닫힌(또는 번호가 재사용된) fd에 `shutdown`이 걸리지 않도록 한다.
- Source 측 UAF 방어: `stop()`은 `clientMutex_` 아래서 `activeClient_->cancel()`을 부르고, 워커는 클라이언트 파괴 **전에 반드시 `activeClient_ = nullptr`** 로 지운다.

### 4.3 목적지 주소 검증 (`inet_pton`)

`inet_pton`의 반환값을 검사한다. 검사하지 않으면 `sin_addr`가 0인 채 **`0.0.0.0`에 접속을 시도**해 원인 모를 실패로 이어진다. 잘못된 `rtspIp`(호스트명/오타)는 명시적 로그와 함께 즉시 실패 처리(점 표기 IPv4만 지원).

### 4.4 Digest 인증 재전송 공격 방어 (무작위 `cnonce`)

세션마다 `cnonce_`를 `std::mt19937`로 **무작위 생성**한다. 하드코딩된 고정 cnonce는 Digest challenge-response를 **재전송 공격**에 취약하게 만든다.

### 4.5 악성/절단 RTSP 응답의 안전한 처리

- **PLAY 응답을 반드시 검증**: 상태 줄에 ` 200`이 없으면 스트리밍이 시작되지 않으므로 `run()`에 진입하지 않는다(`playSucceeded()`). 예전엔 실패해도 `run()`을 불러 즉시 종료→백오프 리셋되는 재접속 폭풍이 있었다.
- **인터리브 프레이밍 검증**: sync 바이트 `$` 확인, 채널 필터(`channel != 0` 스킵), `payloadLen > kMaxRtpPayloadSize` 시 연결 종료, `payloadLen < kRtpHeaderSize(12)` 스킵
- **헤더 파싱 안전성**: `realm`/`nonce`/`Session` 추출은 `find`+`substr` 기반이며, 닫는 따옴표/구분자가 없으면 `substr`이 길이를 남은 크기로 **클램프**하므로 OOB가 없다. 세션 ID를 못 얻으면(401 등) 상태 줄을 분류해 로그를 남기고 실패 처리한다.

---

## 5. 잔여 위험 & 권고 (Residual Risks)

구조적으로 견고하나, 방어 심층화 관점에서 **선택적으로** 강화할 여지가 있는 항목(현재 크래시/RCE로 이어지지 않음):

| # | 항목 | 성격 | 권고 |
|---|------|------|------|
| R1 | **RTSP 평문 전송** | 설계 전제 | RTSP 구간을 격리된 LAN/VLAN으로 한정 |
| R2 | **Slowloris식 느린 헤더 드립** | I/O (저위험) | `recvHeaders`는 64 KiB 크기 상한으로 결국 종료되나 **총 소요시간 상한은 없음** <br> 헤더 수신 전체에 deadline을 두면 더 견고 |
| R3 | **응답 헤더 값의 후행 garbage 흡수** | 견고성 (저위험) | 닫는 따옴표 누락 시 `realm_`/`nonce_`가 응답 말미까지 흡수될 수 있음. (메모리 안전엔 무해) <br> 구분자 검증 추가 여지 |
| R4 | **MD5 (OpenSSL) deprecated API** | 유지보수 | Digest 인증이 RFC상 MD5를 요구하므로 알고리즘 자체는 정당 <br> deprecated 컴파일 경고 해소를 위해 `EVP` 인터페이스로 이관 여지 |
| R5 | **자격증명 평문 저장** | 배포 | `config.json`이 평문 <br> 파일 권한/시크릿 관리(별도 배포 정책) |

---

## 6. 감사 요약 (5개 영역)

| 영역 | 판정 | 핵심 근거 |
|------|------|-----------|
| **보안 취약점** | ✅ All Clear (경미한 강화 여지 R3/R4) | 전 send 경로 `MSG_NOSIGNAL`, cross-thread `shutdown`(not close) + `cancelFd_` 무효화, `inet_pton` 검증, 무작위 cnonce, PLAY/프레이밍 검증 |
| **I/O 병목** | ✅ All Clear | 모든 블로킹 호출에 타임아웃(connect=select, recv=SO_RCVTIMEO), 폴링/스핀 없음, 취소 즉시성 확보 |
| **OOM 위험** | ✅ All Clear | 모든 버퍼 고정/상한(64 KiB·65535·1 MiB·링), 실패 경로 즉시 `closeSocket`, RAII 세션, 누수 없음 |
| **스택 오버플로** | ✅ All Clear (구조적 면역) | 재귀 없음, 고정 지역 버퍼만 사용 |
| **성능 최적화** | ✅ 양호 (경미한 잔여 복사 1회) | 사용자공간 버퍼링 recv, TCP_NODELAY/SO_RCVBUF, 고정 `rtpPacket_` 재사용, consumer는 `std::swap` zero-copy. producer→링 슬롯 복사 1회는 스레드 경계상 불가피 |

**⚠️ Warning 등급**: R2(slowloris 총시간 상한 부재), R3(헤더 값 후행 garbage) <br> 둘 다 메모리 안전·가용성에 즉각적 위협은 아니며, 방어 심층화 차원의 선택적 개선 항목이다.

**총평**: 네트워크/RTSP 계층은 **타임아웃·버퍼 상한·안전한 취소·SIGPIPE 방어**가 체계적으로 갖춰져 있어, 외부 신뢰 불가 엔드포인트를 마주하는 경계로서 견고하다. Critical 취약점은 발견되지 않았고, 잔여 항목(R1~R5)은 대부분 배포·유지보수 성격이다.