# Main_Project 보안 진단 보고서

## 1. 요약

- 분석 대상: `VEDA-TEAM3/Main_Project`
- 기준 브랜치: `develop`
- 기준 커밋: `24abcb6`
- 분석 방식: 소스·설정·CI/CD·배포 파일 정적 검토
- 수행하지 않은 항목: 실제 장비 침투 테스트, broker 설정/ACL 확인, RTSP 카메라 설정 확인,
  바이너리 fuzzing, 운영 서버 권한 확인, 의존성 바이너리 버전별 CVE 대조

현재 저장소에서 확인한 결과는 다음과 같다.

| 등급 | 개수 | 핵심 내용 |
|---|---:|---|
| Critical | 1 | 신뢰할 수 없는 PR 코드가 self-hosted runner에서 실행될 수 있음 |
| High | 2 | Control MQTT 평문·무인증 기본 경로, RTSP 평문 Digest/고정 cnonce |
| Medium | 5 | MQTT payload byte 상한 부재, config fail-open, secret 파일 보호 정책 부족, 빌드 hardening 부족, GitHub Action 미고정 |
| Low | 2 | 내부 broker IP 하드코딩, 로그 파일 symlink 방어 부재 |

가장 먼저 처리할 항목은 CI runner 격리다. 공개 또는 외부 기여자가 PR을 만들 수 있는
저장소라면 현재 설정은 self-hosted runner와 같은 네트워크의 자산까지 위험하게 만들 수 있다.
그다음 Control MQTT를 TLS와 broker 인증/ACL이 필수인 구조로 바꾸고, MQTT 수신 payload에
메시지별 byte/object 상한을 적용해야 한다.

---

## 2. 평가 기준

- **Critical**: 저장소 또는 내부 인프라 장악으로 이어질 수 있으며 공격 난이도가 낮음
- **High**: 인증 우회, credential 탈취, 통신 변조 또는 원격 서비스 장애 가능
- **Medium**: 특정 전제에서 서비스 장애·정보 노출·공급망 위험으로 확대 가능
- **Low**: 직접 악용 가능성은 제한적이지만 방어 심층 또는 정보 노출 측면에서 개선 필요

위험도는 코드만이 아니라 배포 전제를 포함한다. 예를 들어 broker가 완전히 격리된 전용
네트워크와 강한 ACL을 사용한다면 MQTT 관련 위험은 일부 감소하지만, 애플리케이션 자체에
방어가 없다는 사실은 변하지 않는다.

---

## 3. 발견사항

## SEC-01. Pull Request를 self-hosted runner에서 실행

- 심각도: **Critical**
- CWE: CWE-94, CWE-250
- 위치:
  - `.github/workflows/clang-ci.yml:3-10`
  - `.github/workflows/clang-ci.yml:15`
  - `.github/workflows/clang-ci.yml:55`
  - `.github/workflows/clang-ci.yml:69-75`
  - `.github/workflows/clang-ci.yml:139`
  - `.github/workflows/clang-ci.yml:153-162`

### 근거

Workflow는 `pull_request` 이벤트에서 시작되고 세 job 모두 `runs-on: self-hosted`를 사용한다.
PR에 포함된 `CMakeLists.txt`를 `cmake`로 configure하고 build한다. CMake configure 단계는
임의 프로세스 실행과 파일·네트워크 접근이 가능한 코드 실행 단계다. Runner에서는
`sudo apt-get`도 사용하므로 해당 runner 계정이 비밀번호 없는 sudo 권한을 가졌을 가능성이
높다.

### 공격 시나리오

공격자가 악성 CMake 명령을 포함한 PR을 만들면 workflow가 이를 self-hosted runner에서
실행할 수 있다. 공격 코드는 다음 자산을 노릴 수 있다.

- runner 호스트의 파일과 credential
- Docker socket, SSH key, cloud credential 등 runner에 남아 있는 자산
- runner가 접근 가능한 사내망·카메라·broker·개발 서버
- 같은 runner에서 이전 job이 남긴 workspace나 cache
- job의 `GITHUB_TOKEN` 및 workflow 실행 컨텍스트

### 해결 방안

1. 외부 또는 신뢰되지 않은 PR은 `ubuntu-latest` 같은 GitHub-hosted runner에서 실행한다.
2. self-hosted runner는 승인된 branch의 `push` 또는 수동 승인 이후 job에만 사용한다.
3. 꼭 self-hosted가 필요하면 다음 조건을 모두 적용한다.
   - PR author/organization membership 확인 후 environment approval
   - job마다 새 VM/container를 만드는 ephemeral runner
   - 내부망 egress/ingress 차단
   - passwordless sudo 제거
   - Docker socket, SSH key, cloud metadata 접근 차단
   - job 종료 후 디스크 폐기
4. format/tidy/build 중 가능한 작업은 GitHub-hosted runner로 이전한다.
5. `pull_request_target`으로 바꾸어 PR 코드를 checkout하는 방식은 사용하지 않는다. Base
   secret 권한으로 공격자 코드를 실행하는 더 위험한 결과가 될 수 있다.

### 검증

- fork PR에서 self-hosted runner label을 가진 job이 절대 예약되지 않는지 확인
- runner에서 내부 IP, metadata endpoint, Docker socket에 접근할 수 없는지 테스트
- workflow 정책 테스트 또는 GitHub organization runner group 제한 적용

---

## SEC-02. Control MQTT가 평문 연결을 기본값으로 사용하고 client 인증 설정이 없음

- 심각도: **High**
- CWE: CWE-319, CWE-306
- 위치:
  - `control-server/include/core/AppConfig.h:220-236`
  - `control-server/include/core/AppConfig.h:330-340`
  - `control-server/src/sink/MqttTransport.cpp:57-78`
  - `control-server/src/sink/MqttTransport.cpp:125-139`
  - `control-server/src/sink/MqttTransport.cpp:350-375`

### 근거

Control Server의 기본 broker URL은 `tcp://localhost:1883`이다. `tcp://`와 `mqtt://`는
명시적으로 TLS를 비활성화한다. `MqttTransport` 내부에는 username/password 및 client
certificate 필드가 있으나 `AppConfig`에서 값을 읽어 transport에 전달하는 공개 설정 경로가
없다. 결과적으로 현재 코드 기준으로 broker 인증은 client ID 외에는 구성할 수 없다.
Client ID는 인증 수단이 아니며 공격자가 복제할 수 있다.

### 영향

- 동일 네트워크의 공격자가 TopView/Alive 메시지를 도청하거나 변조할 수 있음
- 공격자가 `veda/ch/+/topview` 또는 Alive 토픽에 위조 메시지를 publish할 수 있음
- 공격자가 같은 client ID로 접속해 정상 Control 연결을 끊을 수 있음
- 위험도·카메라 상태·하드웨어 상태가 잘못 계산되거나 UI에 잘못 표시될 수 있음

### 해결 방안

1. 운영 profile에서는 `mqtts://` 또는 `ssl://`만 허용하고 평문 scheme을 거부한다.
2. broker server certificate와 hostname을 검증한다.
3. 다음 중 하나 이상을 애플리케이션 설정에 추가한다.
   - per-service username/password
   - mutual TLS client certificate/private key
   - 짧은 수명의 token 기반 인증
4. broker ACL을 최소 권한으로 구성한다.
   - Compute channel N: `veda/ch/N/*` publish만 허용
   - Control: `veda/ch/+/topview`, `veda/ch/+/alive` subscribe 및
     `veda/risk`, `veda/hw/ch/+/status` publish만 허용
   - UI: 필요한 output topic subscribe만 허용
5. credential은 `config.json`에 평문 저장하지 말고 systemd credential, root-owned secret
   file, tmpfs 또는 secret manager로 주입한다.
6. broker anonymous access를 비활성화한다.

### 검증

- 평문 URL로 실행하면 startup이 실패하는 테스트
- 잘못된 CA/hostname/client cert에서 연결이 거부되는 테스트
- 권한 없는 client의 publish/subscribe가 broker ACL에서 거부되는 통합 테스트

---

## SEC-03. RTSP 제어와 metadata가 평문이며 Digest cnonce가 고정값

- 심각도: **High**
- CWE: CWE-319, CWE-330, CWE-327
- 위치:
  - `compute-server/include/core/AppConfig.h:24-33`
  - `compute-server/include/core/AppConfig.h:275-280`
  - `compute-server/src/network/RtspClientV2.cpp:88-135`
  - `compute-server/src/network/RtspClientV2.cpp:368-399`

### 근거

RTSP client는 일반 TCP socket으로 카메라에 연결하며 TLS를 사용하지 않는다. Digest 인증은
OpenSSL MD5를 사용하고 `cnonce`가 항상 `"0a4f113b"`이다. 카메라가 제공한 realm/nonce의
진위도 TLS나 별도 trust anchor로 검증할 수 없다.

MD5 Digest는 password 자체를 평문으로 보내지는 않지만, 네트워크 공격자가 handshake와
응답을 수집해 offline password guessing을 수행하거나, 악성 nonce를 제공하고 응답을
재사용·분석할 수 있다. 고정 cnonce는 요청별 entropy를 제거한다. RTP/ONVIF metadata도
평문이므로 위치·객체 정보를 도청 또는 변조할 수 있다.

### 해결 방안

1. 카메라가 지원하면 RTSPS/RTP over TLS 또는 HTTPS 기반 ONVIF event transport를 사용한다.
2. 지원하지 않으면 카메라망을 별도 VLAN으로 격리하고 Control/Compute host만 ACL로 허용한다.
3. Digest 구현은 server가 광고한 algorithm을 검증하고, 가능하면 SHA-256을 사용한다.
4. `cnonce`는 CSPRNG(`RAND_bytes`)로 요청/session마다 새로 생성한다.
5. nonce-count overflow와 stale nonce 재협상을 처리한다.
6. 카메라별 고유하고 충분히 긴 password를 사용하고 정기 회전한다.
7. RTSP credential을 일반 config 파일에 저장하지 말고 제한된 secret 파일로 분리한다.

### 검증

- 패킷 캡처에서 Authorization 및 metadata가 평문 네트워크에 노출되지 않는지 확인
- 동일 cnonce가 반복되지 않는 단위 테스트
- MITM 인증서/비인가 카메라 endpoint 연결 거부 테스트

---

## SEC-04. MQTT 메시지의 byte 크기와 JSON object 수 상한 부재

- 심각도: **Medium**
- CWE: CWE-400, CWE-770
- 위치:
  - `control-server/src/sink/MqttTransport.cpp:450-469`
  - `control-server/src/receive/MqttChannelReceiver.h:91-101`
  - `control-server/src/receive/MqttChannelReceiver.cpp:142-160`
  - `control-server/src/receive/MqttChannelReceiver.cpp:180-190`
  - `shared/Contract.h`의 `decode<T>()`

### 근거

Receiver queue는 메시지 개수를 4096개로 제한하지만 각 payload의 byte 크기는 제한하지 않는다.
Mosquitto callback은 broker가 전달한 payload 전체를 `std::string`으로 복사한다. Worker는
nlohmann JSON DOM을 생성하고 `objects` vector를 payload 크기에 따라 할당한다.

따라서 broker에 publish할 권한을 얻은 공격자는 매우 큰 JSON 메시지를 연속 발행해 다음을
유발할 수 있다.

- network thread의 대용량 메모리 복사
- queue의 큰 메모리 점유
- JSON parse CPU 사용 증가
- object vector 대량 할당과 process OOM

메시지 개수 제한만으로는 총 byte 수를 제한하지 못한다.

### 해결 방안

1. callback 진입 직후 topic별 최대 payload byte를 검사하고 복사 전에 거부한다.
2. queue에 `maxQueuedBytes`를 추가하여 메시지 수와 총 byte를 모두 제한한다.
3. `TopViewFrame.objects`와 기타 배열의 최대 원소 수를 contract에 정의한다.
4. nlohmann parser의 깊이/입력 크기 제한 또는 SAX/streaming validation을 검토한다.
5. broker의 `message_size_limit`, client별 publish rate limit, inflight limit도 함께 설정한다.
6. oversized/rate-limit drop을 별도 metric과 rate-limited log로 기록한다.

권장 초기값은 실제 측정 후 결정해야 하지만, 예를 들어 TopView 256 KiB, object 1,000개,
queue 총 16~32 MiB 같은 보수적 상한에서 시작해 운영 데이터로 조정할 수 있다.

### 검증

- 최대 크기 직전/직후 payload 테스트
- 4096개의 대형 payload burst에서 RSS가 설정 상한 내인지 확인
- 깊게 중첩된 JSON 및 매우 큰 array fuzz test

---

## SEC-05. 설정 파일 오류 시 보안에 민감한 기본값으로 계속 실행

- 심각도: **Medium**
- CWE: CWE-636
- 위치:
  - `compute-server/include/core/AppConfig.h:245-265`
  - `control-server/include/core/AppConfig.h:238-259`
  - `control-server/include/core/AppConfig.h:220-236`

### 근거

두 AppConfig loader는 설정 파일이 없거나 JSON parse에 실패해도 기본값으로 계속 진행한다.
Control의 기본 MQTT 값은 평문 `tcp://localhost:1883`이다. 운영 배포에서 설정 mount나
권한이 잘못되면 의도한 TLS endpoint 대신 localhost 평문 broker에 연결을 시도할 수 있다.
Compute는 필수 값이 비어 연결에 실패할 가능성이 높지만, 구성 오류가 명확한 startup
failure가 아니라 여러 fallback과 재시도로 나타날 수 있다.

### 해결 방안

1. `development`와 `production` profile을 구분한다.
2. production에서는 config 누락·parse 실패·필수 보안값 누락 시 즉시 종료한다.
3. 다음 값을 startup validation 필수 항목으로 둔다.
   - TLS broker URL과 CA
   - broker 인증 credential 또는 client certificate
   - RTSP endpoint와 credential source
   - channel/client ID uniqueness
4. startup log에는 secret을 제외한 config fingerprint와 보안 mode를 기록한다.
5. systemd의 `ConditionPathIsReadable`, `LoadCredential`, 파일 권한 검사와 연동한다.

---

## SEC-06. RTSP/MQTT credential 저장 파일에 대한 보호 계약이 부족

- 심각도: **Medium**
- CWE: CWE-256, CWE-732
- 위치:
  - `compute-server/include/core/AppConfig.h:28-33`
  - `compute-server/include/core/AppConfig.h:275-280`
  - `.gitignore:4-5`

### 근거

`.env`만 ignore하며 `config.json`, `*.pem`, `*.key`, `*.crt`에 대한 저장소 차원의 보호 규칙이
없다. 현재 tracked tree에서 실제 password/private key는 발견되지 않았지만, Compute 설정은
`rtspUser`와 `rtspPass`를 JSON에서 직접 읽도록 설계되어 운영자가 실수로 credential이 포함된
`config.json`을 commit할 가능성이 있다.

### 해결 방안

1. `.gitignore`에 운영 config, certificate/private key 패턴을 추가한다.
2. secret이 없는 `config.example.json`만 추적한다.
3. pre-commit/CI에 Gitleaks 또는 TruffleHog를 추가한다.
4. config 파일은 서비스 계정 전용 `0600`, 상위 디렉터리는 `0700/0750`으로 설정한다.
5. systemd credentials 또는 secret manager에서 password를 주입한다.
6. 실수로 commit한 경우 파일 삭제만 하지 말고 credential rotation과 Git history 정리를 함께
   수행한다.

---

## SEC-07. 기본 release 빌드에 명시적 exploit mitigation이 없음

- 심각도: **Medium**
- CWE: CWE-693
- 위치:
  - `compute-server/CMakeLists.txt:45`
  - `control-server/CMakeLists.txt:35`

### 근거

두 실행 파일의 compile option은 `-O2`뿐이다. 배포 toolchain의 distro 기본값에 따라 일부
보호가 들어갈 수 있지만 저장소는 stack protector, FORTIFY, PIE, RELRO/NOW를 명시적으로
보장하지 않는다. 이 애플리케이션은 RTSP/RTP, MQTT, UART 등 공격자가 영향을 줄 수 있는
binary input을 처리하므로 memory corruption 결함이 생겼을 때 exploit 난이도를 높이는
방어가 필요하다.

### 해결 방안

Release hardening option을 toolchain 또는 CMake preset에 추가한다.

```cmake
target_compile_options(target PRIVATE
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=3
    -fPIE
    -fno-omit-frame-pointer
)
target_link_options(target PRIVATE
    -pie
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
)
```

개발/CI에는 ASan, UBSan을 적용하고, 동시성 경로에는 별도 TSan job을 운영한다. Toolchain
호환성을 먼저 검증하고 중복되거나 지원되지 않는 flag는 조건부 적용한다.

### 검증

- `checksec --file=<binary>`
- ASan/UBSan test target 실행
- `readelf`로 PIE, GNU_RELRO, NX stack 확인

---

## SEC-08. GitHub Actions를 mutable tag로 참조

- 심각도: **Medium**
- CWE: CWE-829
- 위치:
  - `.github/workflows/clang-ci.yml:17`
  - `.github/workflows/clang-ci.yml:45`
  - `.github/workflows/clang-ci.yml:59`
  - `.github/workflows/clang-ci.yml:129`
  - `.github/workflows/clang-ci.yml:145`
  - `.github/workflows/clang-ci.yml:166`

### 근거

`actions/checkout@v4`와 `8398a7/action-slack@v3`는 major tag를 사용한다. Tag 소유자가 tag를
이동하거나 upstream 계정/배포가 침해되면 workflow가 다른 코드를 실행할 수 있다. 특히
Slack action은 webhook secret을 환경변수로 받는다.

### 해결 방안

1. 모든 third-party action을 검증된 full commit SHA로 고정한다.
2. Dependabot/Renovate로 SHA 업데이트 PR을 자동 생성한다.
3. GitHub organization Actions 정책에서 허용 action을 제한한다.
4. job별 `permissions`를 명시해 기본 `GITHUB_TOKEN` 권한을 최소화한다.
5. Slack 알림은 가능하면 별도 최소 권한 job으로 격리한다.

---

## SEC-09. 내부 broker IP가 source default로 노출

- 심각도: **Low**
- CWE: CWE-200
- 위치:
  - `control-server/src/sink/MqttTransport.cpp:51-55`
  - `control-server/src/sink/MqttTransport.h:68`

### 근거

내부 RFC1918 broker IP가 parser fallback과 class 기본값에 하드코딩되어 있다. 직접 침해를
만들지는 않지만 네트워크 구조 정보를 공개하고, 잘못된 broker URL 입력 시 의도치 않은 내부
host로 연결할 가능성이 있다.

### 해결 방안

- host 기본값을 빈 문자열로 두고 parsing/validation 실패 시 startup을 중단한다.
- example 값은 문서용 reserved hostname(`broker.example.invalid`)을 사용한다.
- source에는 환경별 IP를 넣지 않는다.

---

## SEC-10. 로그 파일 open 시 symlink·소유권 검증이 없음

- 심각도: **Low** (서비스가 root이거나 로그 디렉터리가 공격자에게 writable하면 Medium 이상)
- CWE: CWE-59
- 위치:
  - `shared/Logger.h:270-284`
  - `deploy/logrotate/veda-compute:29-36`
  - `deploy/logrotate/veda-control:18-23`

### 근거

Logger는 설정된 경로를 `std::ofstream`으로 열며 symlink 여부와 소유권을 검사하지 않는다.
서비스가 높은 권한으로 실행되고 공격자가 작업 디렉터리 또는 log path에 symlink를 만들 수
있다면 임의 파일 append 또는 logrotate와의 경합을 유발할 수 있다.

### 해결 방안

1. Compute/Control을 전용 비-root systemd 계정으로 실행한다.
2. 로그 디렉터리를 해당 계정 소유로 만들고 다른 사용자의 write를 금지한다.
3. 운영에서는 임의 `logFileName` 대신 고정된 절대 경로/허용 디렉터리만 사용한다.
4. 필요하면 `open(O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0640)` 후 `fstat()`으로 regular
   file과 owner를 검증한다.
5. logrotate 템플릿의 placeholder 계정을 실제 서비스 계정으로 확정한다.

---

## 4. 긍정적으로 확인된 방어

다음 항목은 현재 코드에 이미 구현되어 있다.

- Compute MQTT는 CA 파일을 요구하고 `tls_insecure=false`로 설정한다.
- MQTT topic에서 추출한 channel과 payload channel의 일치를 확인한다.
- TopView schema, timestamp, channel, class, finite 좌표를 검증한다.
- RTSP header 크기를 64 KiB로 제한한다.
- RTP payload와 metadata frame 크기에 상한이 있다.
- MQTT receive queue와 Compute Sink queue에 개수 제한 및 drop-oldest가 있다.
- UART packet은 고정 크기 framing과 CRC 검증 경로를 사용한다.
- callback 경계에서 예외가 C API 밖으로 전파되지 않도록 차단한다.
- 현재 tracked tree에서 실제 password, private key, token 값은 발견되지 않았다.
- 로그 CSV field의 quote escaping을 수행한다.

이 항목들은 전체 시스템의 안전성을 높이지만 broker ACL, 운영 파일 권한, runner 격리 같은
배포 보안까지 대신하지는 않는다.

---

## 5. 우선순위별 개선 계획

### 즉시: 0~2일

1. PR job을 GitHub-hosted runner로 이동하거나 self-hosted job에 수동 승인 gate 적용
2. self-hosted runner의 credential·내부망 접근 여부 조사 및 필요 시 rotation
3. 운영 broker에서 anonymous access 차단 및 topic ACL 확인
4. Control 운영 config가 `mqtts://`를 사용하는지 확인
5. repository secret scanning 활성화

### 단기: 1~2주

1. Control AppConfig에 broker 인증/mTLS 설정 추가
2. production에서 plaintext MQTT와 config fallback 거부
3. MQTT payload byte, queue total byte, object count 상한 구현
4. RTSP cnonce를 CSPRNG로 교체하고 카메라 VLAN ACL 적용
5. GitHub Action을 SHA pinning하고 job permissions 최소화
6. config secret을 systemd credential 또는 전용 secret file로 분리

### 중기: 2~6주

1. RTSPS/보안 ONVIF transport 지원 여부 검증 및 전환
2. broker restart, ACL, TLS rejection, oversized payload 통합 테스트
3. ASan/UBSan/TSan 및 fuzzing CI 추가
4. release hardening flag와 `checksec` gate 도입
5. SBOM 생성 및 OpenSSL/libmosquitto/nlohmann-json 버전별 CVE 검사
6. queue depth, oversized drop, reconnect, auth failure security metric 추가

---

## 6. 권장 보안 테스트

| 테스트 | 기대 결과 |
|---|---|
| 평문 `tcp://`로 production 실행 | startup 실패 |
| 잘못된 CA 또는 hostname | MQTT 연결 거부 |
| 권한 없는 client의 TopView publish | broker ACL 거부 |
| 1 MiB 이상 TopView payload | 복사/parse 전 drop |
| object 수 상한 초과 JSON | validation drop |
| broker 재시작 | 제한된 backoff로 재연결·재구독 |
| Compute 강제 종료 | retained LWT `"0"` 수신 |
| 악성/깊은 JSON fuzzing | crash/OOM 없음 |
| 잘못된 UART CRC/길이 | packet 무시 |
| fork PR | self-hosted runner 미할당 |
| release binary `checksec` | PIE, RELRO, NOW, NX, stack protector 활성 |

---

## 7. 한계와 추가 확인 필요 항목

이 보고서는 repository 정적 분석 결과다. 다음은 운영 환경 없이는 확정할 수 없다.

- Mosquitto broker의 anonymous 설정, password/mTLS, ACL, max packet size
- 실제 RTSP 카메라의 RTSPS 및 SHA-256 Digest 지원 여부
- config/certificate/private key의 파일 권한
- systemd service의 실행 계정, capabilities, sandbox 옵션
- self-hosted runner의 sudo, 내부망, Docker socket, cloud credential 접근
- 설치된 OpenSSL, libmosquitto, nlohmann-json, compiler의 정확한 버전과 CVE
- 방화벽, VLAN, broker HA, 로그 수집 서버의 보안 설정

운영 점검 시 이 목록을 증적과 함께 보완해야 최종 보안 승인이 가능하다.
