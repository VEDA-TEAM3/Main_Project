# Compute Server OWASP 및 Secure Coding 검토 보고서

- 검토일: 2026-08-07
- 범위: `compute-server/`, `shared/`, 관련 단위 테스트와 직접 연결된 빌드·컨테이너 설정
- 기준: OWASP Top 10:2025, OWASP OT Top 10, SEI CERT C/C++ Secure Coding

## 1. 결론

현재 상태는 **부분 준수**다. 네트워크·JSON·UART 신뢰 경계의 메모리 고갈, 헤더 주입,
비정렬 접근, 잘못된 필드 수용 문제는 코드로 보완했다. MQTT 서버 인증서 검증도 활성화되어
있다. 그러나 평문 RTSP와 MD5 Digest, MQTT 클라이언트 인증 부재, XOR뿐인 UART 무결성,
공급망 고정·보안 경보 부재 때문에 엔터프라이즈 운영 승인 기준을 완전히 충족하지 않는다.

OWASP Top 10은 웹 애플리케이션 인식 문서이므로 이 RTSP/MQTT C++ 서비스에 대한 인증서가
아니다. 적용 가능한 신뢰 경계와 CWE를 기준으로 매핑했다.

## 2. 적용한 방어적 리팩터링

| 영역 | 파일 | 적용 내용 | 대응 위험 |
|---|---|---|---|
| 설정 입력 | `compute-server/include/core/AppConfig.h` | 설정 파일 1 MiB, RTSP 버퍼 4 MiB, 소켓 16 MiB, 링 256, 로그 큐 100,000 상한; 포트·시간·비율 범위 검증 | CWE-20, CWE-400, A02/A10 |
| 문자열·경로 | `AppConfig.h` | RTSP/MQTT 제어문자·길이 거부, 로그 절대경로 및 `..` 차단 | CWE-93, CWE-22, A05 |
| RTSP | `compute-server/src/network/RtspClientV2.cpp` | 서버 realm/nonce/session 검증, 로그 제어문자 제거, 부분 `send()`/EINTR 처리, CSPRNG nonce, 고정 인증 헤더 버퍼 제거, 정수 파싱 범위화 | CWE-93, CWE-330, CWE-190, A04/A05/A10 |
| 공유 JSON | `shared/Contract.h` | payload 1 MiB, 중첩 32, 객체 256 상한; 초과 입력은 `v=0`으로 fail-closed | CWE-400, CWE-502, A08/A10 |
| UART | `shared/driver_protocol.h` | packed 필드의 정렬 안전 little-endian 접근, null 방어, enum/bool/reserved 검증, C/C++ ABI 크기 정적 검증 | CWE-20, CWE-704, CWE-843 |
| 빌드 | `compute-server/CMakeLists.txt` | stack protector, FORTIFY, PIE, RELRO, NOW | CWE-121 완화 |
| 컨테이너 | `compute-server/docker-compose.yml` | read-only rootfs, 모든 capability 제거, no-new-privileges, PID·메모리·CPU 제한, noexec tmpfs | A02/A06 |
| 이미지 | `compute-server/Dockerfile` | healthcheck 전용 `procps` 제거 | A03 |

## 3. OWASP Top 10:2025 매핑

| 항목 | 판정 | 코드 근거 |
|---|---|---|
| A01 Broken Access Control | 부분 | compute-server에는 사용자 API가 없다. MQTT는 CA 기반 서버 인증만 수행하며 클라이언트 자격증명/인증서 설정 호출은 없다. 토픽 ACL 준수 여부는 브로커 설정 없이는 확인 불가다. |
| A02 Security Misconfiguration | 부분 | TLS 인증서 검증, 입력 범위, 컨테이너 최소 권한은 확인했다. 반면 `network_mode: host`이며 설정 파일 누락·파싱 실패 시 기본값으로 계속 진행한다. |
| A03 Software Supply Chain Failures | 미충족 | Ubuntu base image가 digest로 고정되지 않았고 APT 패키지 버전도 고정되지 않았다. 루트 CMake의 GoogleTest는 tag 기반 FetchContent다. SBOM/SCA/서명 검증은 확인되지 않았다. |
| A04 Cryptographic Failures | 미충족 | MQTT는 CA와 호스트명을 검증한다. RTSP는 raw TCP이며 Digest 계산은 MD5다. 카메라 비밀번호는 `config.json` 평문 필드다. nonce 생성은 OpenSSL `RAND_bytes`로 개선했다. |
| A05 Injection | 준수 확인 | 셸·SQL 실행 경로는 없다. RTSP 반사 헤더 값과 설정 제어문자를 차단했고 XML 파서는 엔티티 실행 엔진 없이 길이 기반 검색만 사용한다. |
| A06 Insecure Design | 부분 | 프레임·객체·큐 상한과 재연결 backoff가 있다. CCTV–compute 신뢰 구간과 MQTT 주체별 권한 모델은 코드로 강제되지 않는다. |
| A07 Authentication Failures | 미충족 | 카메라는 legacy Digest만 사용한다. MQTT 클라이언트가 브로커에 자신을 증명하는 mTLS 또는 사용자 자격증명 경로가 없다. |
| A08 Software/Data Integrity Failures | 부분 | MQTT TLS는 전송 중 변조를 방지한다. 공유 JSON은 크기·깊이·형식을 제한한다. UART XOR checksum은 오류 검출일 뿐 위조 방지나 replay 방지가 아니다. |
| A09 Logging & Alerting Failures | 부분 | 연결·파싱·드랍 오류는 rate-limit된 로컬 로그로 남는다. 중앙 수집, 변조 방지, 보안 이벤트 규칙 및 호출 가능한 경보 경로는 확인되지 않았다. |
| A10 Mishandling Exceptional Conditions | 부분 | 파싱은 예외를 외부로 내보내지 않고 네트워크·큐는 상한과 timeout을 사용한다. 다만 일부 `setsockopt`/`fcntl` 반환값과 백그라운드 스레드의 모든 예외가 처리되지는 않는다. |

## 4. 잔여 발견사항

### SEC-01 — 평문 RTSP 및 MD5 Digest

- 심각도: **높음**
- 파일: `compute-server/src/network/RtspClientV2.cpp:582`, `compute-server/src/network/RtspClientV2.cpp:132`
- 근거: IPv4 TCP 소켓에 직접 연결하고 `EVP_md5()`로 Digest 응답을 만든다. TLS 계층은 없다.
- 수정 이유: 동일 네트워크의 공격자가 메타데이터를 관찰·변조하거나 인증 응답을 재사용할 수 있다.
- 권장 수정: 카메라가 지원하면 RTSPS(TLS 1.2+)와 SHA-256 Digest를 구현한다. 미지원 장비는
  전용 CCTV VLAN과 compute 노드 사이를 WireGuard/IPsec으로 보호하고 MD5 예외를 자산별로 승인한다.

### SEC-02 — MQTT 클라이언트 인증 경로 부재

- 심각도: **높음**
- 파일: `compute-server/src/mqtt/MqttTransport.cpp:151-181`
- 근거: CA 설정과 서버 인증서 검증은 있으나 `mosquitto_tls_opts_set` 기반 클라이언트 인증서,
  `mosquitto_username_pw_set` 또는 별도 인증 토큰 설정이 없다.
- 수정 이유: 브로커가 익명 접속을 허용하거나 네트워크 경계가 무너지면 임의 주체가 채널 상태를 위조할 수 있다.
- 권장 수정: 채널별 mTLS 인증서를 발급하고 SAN/주체를 `veda/ch/{ch}/#` publish ACL에 매핑한다.
  인증서 개인키는 파일 모드 0600 또는 TPM에 보관하고 자동 회전한다.

### SEC-03 — UART 위조·재전송 방지 없음

- 심각도: **높음**(UART 접근자가 비신뢰 주체일 때), 그 외 **중간**
- 파일: `shared/driver_protocol.h:85-101`
- 근거: 프레임 무결성은 단순 XOR checksum이며 sequence number, MAC, challenge가 없다.
- 수정 이유: 물리/커널 접근자가 유효 checksum을 즉시 다시 계산해 위험 상태 또는 ACK를 위조할 수 있다.
- 권장 수정: UART가 신뢰 경계를 넘으면 프로토콜 v2에 monotonic sequence와 HMAC-SHA-256
  truncated tag를 추가한다. 기존 27/19-byte ABI는 별도 버전으로 유지한다.

### SEC-04 — 공급망 재현성과 검증 부족

- 심각도: **중간**
- 파일: `compute-server/Dockerfile:2,34`, `CMakeLists.txt:26-30`
- 근거: base image digest와 APT 버전이 고정되지 않았고 테스트 의존성은 Git tag를 신뢰한다.
- 수정 이유: 동일 버전명이 다른 산출물을 가리키거나 upstream/registry 침해가 빌드에 유입될 수 있다.
- 권장 수정: 승인된 digest, lock된 패키지 snapshot, commit SHA를 사용하고 CycloneDX/SPDX SBOM,
  SCA, SLSA provenance 및 cosign 서명 검증을 CI 승격 조건으로 둔다.

### SEC-05 — 보안 이벤트 경보 경로 없음

- 심각도: **중간**
- 파일: `shared/Logger.h:172-193,270-303`
- 근거: 로그는 콘솔/CSV에 기록되고 포화 시 오래된 항목을 버린다. SIEM 전달이나 임계치 경보 코드는 없다.
- 수정 이유: 반복 인증 실패, 비정상 payload, reconnect 폭풍이 기록되어도 운영 대응이 시작되지 않는다.
- 권장 수정: journald/Fluent Bit로 중앙 전송하고 인증 실패·JSON 거부·드랍·채널 반복 단절에
  시간창 기반 경보를 설정한다. 로그 디렉터리는 서비스 UID만 쓰고 운영자는 읽기 전용으로 접근한다.

### SEC-06 — 예외 및 시스템 호출 실패의 완전한 격리 부족

- 심각도: **중간**
- 파일: `compute-server/src/network/RtspClientV2.cpp:130-189`,
  `compute-server/src/source/RtspOnvifSourceV2.cpp:58-146`
- 근거: 일부 socket option/`fcntl` 결과가 검사되지 않으며 source worker entry 전체에 catch-all이 없다.
- 수정 이유: 자원 고갈 또는 비정상 런타임에서 timeout/모드 설정 실패가 감춰지거나 예외가
  `std::terminate`로 이어질 수 있다.
- 권장 수정: 생존성에 필요한 시스템 호출은 실패 시 소켓을 닫고 재시도하며, 각 thread entry 최상단에서
  `std::exception`과 catch-all을 처리해 상태를 dead로 발행한 뒤 backoff한다.

## 5. 시스템 설정 필수 조치

### P0 — 운영 전 필수

1. CCTV, compute-server, MQTT broker를 별도 VLAN/방화벽 zone으로 분리한다. compute 컨테이너의
   egress는 등록된 카메라 IP:RTSP와 broker:8883, DNS/NTP만 허용한다.
2. Mosquitto에서 anonymous 접속을 끄고 TLS 1.2+와 채널별 mTLS를 적용한다. compute는 자기
   `topview`, `blur`, `alive` 토픽만 publish하고 subscribe 권한은 갖지 않는다.
3. `config.json`은 root 소유 0640 이하, 컨테이너 read-only mount로 유지한다. 비밀번호를
   이미지·Git·Compose 환경변수에 넣지 말고 secret manager/TPM 또는 최소한 별도 credential file로 분리한다.
4. `network_mode: host` 사용 근거가 없으면 전용 bridge network로 전환하고 nftables 정책을 강제한다.
5. UART 장치는 전용 Unix group과 udev mode 0660으로 제한하고 컨테이너에는 필요한 단일 device만 전달한다.

### P1 — 운영 안정화

1. rootless Docker/Podman, 기본 seccomp, AppArmor/SELinux enforcing을 적용한다.
2. 이미지 digest 고정, SBOM·SCA·secret scan·서명 검증을 PR/배포 gate로 둔다.
3. 중앙 로그, 보존·무결성 정책, NTP 동기화 및 보안 경보 runbook을 구성한다.
4. MQTT/RTSP 연결률, 거부 payload, 큐 drop, 메모리, 재시도 횟수에 rate limit과 경보 임계값을 둔다.
5. CA/클라이언트 인증서 만료 30일 전 경보와 무중단 회전 절차를 운영한다.

## 6. 검증 결과

- Release compute-server 및 모든 등록 테스트 빌드 성공
- `ctest --test-dir /tmp/veda-security-build --output-on-failure`: **208/208 통과**
- `driver_protocol.h`: GCC C11, `-Wall -Wextra -Werror` syntax 검사 통과
- 신규 회귀 검사: RTSP CRLF 주입, JSON 크기/깊이/객체 수, UART endian/ABI/필드 검증
- 수정 파일 `clang-format-18` 적용 및 scoped `git diff --check` 통과

Docker daemon, 실제 CCTV, MQTT broker 설정, 인증서, OS 방화벽, CVE 데이터베이스는 제공되지 않아
동적 침투시험·SCA·실환경 TLS 검증은 수행하지 않았다.

## 7. 기준 문서

- [OWASP Top 10:2025](https://owasp.org/Top10/2025/0x00_2025-Introduction/)
- [OWASP A04 Cryptographic Failures](https://owasp.org/Top10/2025/A04_2025-Cryptographic_Failures/)
- [OWASP A09 Security Logging and Alerting Failures](https://owasp.org/Top10/2025/A09_2025-Security_Logging_and_Alerting_Failures/)
- [OWASP A10 Mishandling of Exceptional Conditions](https://owasp.org/Top10/2025/A10_2025-Mishandling_of_Exceptional_Conditions/)
- [OWASP OT Top 10](https://ot.owasp.org/v/2025/the-top-10/)
- [SEI CERT Top 10 Secure Coding Practices](https://wiki.sei.cmu.edu/confluence/pages/viewpage.action?pageId=97747041)
- [SEI CERT ERR51-CPP](https://wiki.sei.cmu.edu/confluence/display/cplusplus/ERR51-CPP.%2BHandle%2Ball%2Bexceptions)
