# Compute Server 초기 설정 및 신규 CCTV 배포 매뉴얼

## 버전 이력

| 버전 | 일자 | 내용 |
| --- | --- | --- |
| v1.0 | 2026-07-28 | 최초 작성. 디렉터리 생성 · `docker-compose.yml` 작성 · logrotate 설정을 모두 수동으로 진행 |
| v2.0 | 2026-07-29 | `setup_cctv.sh` 도입으로 디렉터리/compose 스캐폴딩과 logrotate 설정을 자동화. 컨테이너 네트워크를 `network_mode: host` 로 전환, 엣지 수용량 측정 결과 반영 |

> **대상 독자**: 현장 배포 엔지니어
> **적용 범위**: 신규 CCTV 1대(4채널)를 `Docker Compose`로 배포하는 전 과정
> **아키텍처**: 채널당 `compute-server` 컨테이너 1개(총 4개)를 단일 `docker-compose.yml`로 기동
> `Auto Deployment` · `restart policy` · `Health Check`를 `Compose`가 담당

### 엣지 수용량

**라즈베리파이 4(4GB) 1대는 CCTV 3대(총 12채널)까지 안정적으로 운용할 수 있다.**

CCTV 1대는 4채널(다방향 센서)이고 채널 1개당 컨테이너 1개이므로, Pi 1대에서 도는 컨테이너는
최대 12개다. 이 값이 상한인 이유는 메모리다 — 채널당 `memory: 256M` 이므로 12채널이면
`12 × 256M = 3GB`, 4GB 중 OS/dockerd 몫으로 약 700MB 만 남는다. CCTV 4대(16채널)는
4GB 를 넘겨 OOMKill 이 발생한다.

`cpus: "0.75"` 는 **상한이지 예약이 아니다.** compute-server 는 디코딩된 영상이 아니라 ONVIF
메타데이터만 다루므로 실사용량이 상한보다 훨씬 낮고, 그래서 `0.75 × 12 = 9.0` 이 4코어를
넘겨도 문제가 되지 않는다.

CCTV 를 4대 이상 붙여야 하면 Pi 를 추가한다. `channelId` 만 전역에서 유일하면
control-server 는 어느 Pi 에서 왔는지 구분할 필요가 없다.

---

## Step 0. 사전 준비 (배포 전 확인)

배포를 시작하기 전에 호스트(라즈베리파이)에서 아래를 확인한다.

| 항목 | 확인 명령 | 기대 결과 |
| --- | --- | --- |
| Docker 설치 | `docker --version` | 20.10 이상 |
| Docker Compose v2 | `docker compose version` | v2.x |
| compute-server 이미지 | `docker image ls` | `veda/compute-server:1.0.0` 태그가 존재 |
| MQTT 브로커 접속 | `ping -c1 <브로커IP>` | 응답 정상 |
| logrotate | `logrotate --version` | 설치되어 있을 것 |
| 여유 메모리 | `free -h` | 추가할 채널 수 × 256M 이상 |

> **이미지 준비**: **본 매뉴얼은 이미지가 이미 배포 호스트에 있다고 가정한다.**
> 개발 장비에서 `docker buildx build --platform linux/arm64 -t veda/compute-server:1.0.0 --load .` 로 빌드하고
> `docker save` → `scp` → `docker load` 로 옮긴다. 태그는 compose 의 `image:` 와 정확히 일치해야 한다.
> 이미지의 작업 디렉터리(`WORKDIR`)는 `/opt/veda` 이며, 컨테이너는 그 디렉터리에서 `config.json`을 읽고 로그를 쓴다.

---

## Step 1. 스캐폴딩 (`setup_cctv.sh`)

디렉터리 생성 · `docker-compose.yml` 작성 · 로그 디렉터리 소유권 · logrotate 설정을
**스크립트 한 번**으로 처리한다. 멱등이므로 몇 번 실행해도 결과가 같다.

```bash
sudo ./deploy/setup_cctv.sh 01
```

인자는 **2자리 CCTV ID** 하나뿐이다(`01`~`99`).

### 1-1. 스크립트가 만드는 것

| 경로 | 내용 |
| --- | --- |
| `/opt/veda/cctv_01/` | 배포 베이스 디렉터리 |
| `/opt/veda/cctv_01/ch0` ~ `ch3` | 채널별 설정 디렉터리 (`config.json` 은 **비워둔다**) |
| `/opt/veda/cctv_01/docker-compose.yml` | 4채널 서비스 정의 (자동 생성 — 직접 수정 금지) |
| `/var/log/veda/cctv_01/` | 호스트 로그 디렉터리, `chown 1000:1000` 적용 |
| `/etc/logrotate.d/veda-cctv-01` | 로그 회전 설정 (Step 5) |

`chown 1000:1000` 은 생략할 수 없다. 컨테이너는 비루트 계정 `veda_user`(uid 1000)로 실행되므로
소유권이 root 로 남아 있으면 **CSV 로거가 파일을 만들지 못하고 조용히 실패한다.**

### 1-2. 재정의 가능한 환경변수

| 변수 | 기본값 | 용도 |
| --- | --- | --- |
| `IMAGE` | `veda/compute-server:1.0.0` | 이미지 태그 고정 |
| `CHANNELS` | `4` | CCTV 1대의 채널 수 |
| `ROOT` / `LOGROOT` | `/opt/veda` / `/var/log/veda` | 배포·로그 루트 (테스트용) |
| `CA` | `/etc/veda/certs/ca.crt` | CA 인증서 경로 |

### 1-3. 생성되는 compose 의 핵심

- **`network_mode: "host"`** — RTSP(카메라)/MQTT(브로커) 모두 아웃바운드 전용이라 열 포트가
  없다. NAT 홉을 제거해 지연을 줄인다. `network_mode` 를 쓰면 `networks:` 키는 함께 쓸 수 없다.
- **자원 제한** — `cpus: "0.75"`, `memory: 256M` (채널당)
- **헬스체크** — `pgrep compute-server` (프로세스 생존). ONVIF 메타데이터 파이프라인이라
  HTTP `/health` 같은 엔드포인트가 없다.
- **`restart: always`** — 프로세스가 죽거나 호스트가 재부팅돼도 다시 띄운다.
- **`stop_grace_period: 15s`** — SIGTERM 후 정상 종료(MQTT 사망 신호 발행)까지 기다린다.

### 1-4. 볼륨 마운트 설계

| 마운트 | 모드 | 목적 |
| --- | --- | --- |
| `./chX/config.json` → `/opt/veda/config.json` | `ro` | 채널별 설정 (컨테이너가 변조 못하게 읽기 전용) |
| `/etc/veda/certs/ca.crt` → 동일 경로 | `ro` | TLS CA (전 컨테이너가 공유) |
| `/var/log/veda/cctv_01` → `/opt/veda/logs` | rw | CSV 로그를 **호스트**에 기록 → 컨테이너가 죽어도 로그 보존 |

네 채널이 **같은 호스트 폴더**에 **서로 다른 파일명**으로 쌓인다 → logrotate 글롭 하나로 처리.

---

## Step 2. 채널별 `config.json` 수동 배치

**스크립트는 `config.json` 을 생성하지 않는다.** 카메라 계정/비밀번호가 들어가므로
현장에서 직접 배치한다. 스크립트 실행 후 출력되는 `TODO:` 목록이 그대로 체크리스트다.

각 채널 디렉터리(`/opt/veda/cctv_01/ch0` ~ `ch3`)에 `config.json` 을 만든다.
**채널마다 반드시 달라져야 하는 값**은 다음 두 가지다.

| 키 | 채널별 값 | 설명 |
| --- | --- | --- |
| `channelId` | `0`, `1`, `2`, `3` | 이 채널의 고유 ID. `control-server`가 이 값으로 구분한다. **브로커 전역에서 절대 중복 금지.**<br>( **정책**: 전역 channelId = cctvId × 4 + 로컬채널(0~3) ) |
| `logFileName` | `logs/veda_ch0.csv` … `logs/veda_ch3.csv` | 작업 디렉터리 기준 상대 경로. `logs/` 는 호스트 로그 디렉터리 마운트 지점. 채널마다 파일명이 달라야 한 폴더에서 안 섞인다. |

채널마다 `rtspSetupUri` / `rtspPlayUri` 도 각자의 카메라(방향)를 가리켜야 한다.
CCTV 1대의 4채널은 **설치 좌표(`cameraPosX/Y`)는 같지만 감시 방향이 다르며**, 각각
control-server 의 서로 다른 `SpatialZone` 하나에 1:1 대응한다.

MQTT 항목(TLS 전용):

```json
{
  "channelId":   0,
  "logFileName": "logs/veda_ch0.csv",
  "mqttHost":    "<브로커 호스트명>",
  "mqttPort":    8883,
  "mqttCaFile":  "/etc/veda/certs/ca.crt"
}
```

> `mqttHost` 는 **인증서 SAN 과 일치해야 하는 값**이다. `MqttTransport` 는 호스트명 검증을
> 끄지 않으므로(`tls_insecure_set(false)`) SAN 이 다르면 핸드셰이크가 실패하고,
> 증상은 `queue full; broker NOT connected` 드랍 로그로만 나타난다.

---

## Step 3. TLS 인증서 배치

CA 인증서(`ca.crt`)는 호스트에 **한 곳**에만 두고, 모든 컨테이너에 **읽기 전용**으로
마운트한다(컨테이너마다 복제하지 않는다).

```bash
sudo mkdir -p /etc/veda/certs && sudo cp ./ca.crt /etc/veda/certs/ca.crt && sudo chmod 644 /etc/veda/certs/ca.crt && sudo chown root:root /etc/veda/certs/ca.crt
```

compute-server 에는 **`ca.crt` 한 장만** 준다 — 클라이언트가 브로커 개인키를 들고 있을 이유가 없다.

---

## Step 4. 실행 및 확인

### 4-1. 기동

```bash
cd /opt/veda/cctv_01 && docker compose up -d && docker compose ps
```

### 4-2. 기동 로그로 설정 반영 확인

각 채널이 **자기 channelId** 와 **로그 경로**로 떴는지 확인한다.

```bash
docker compose logs -f compute-ch0
```

정상 기동이면 다음 두 줄이 보여야 한다.

```
Network       - Success: PLAY 성공, 스트리밍 시작
MqttTransport - Success: 연결 성공 (host=..., port=8883)
```

### 4-3. 상태/헬스 확인

```bash
docker inspect --format '{{.Name}} {{.State.Health.Status}}' cctv_01_ch0 cctv_01_ch1 cctv_01_ch2 cctv_01_ch3
```

```bash
ls -l /var/log/veda/cctv_01/
```

### 4-4. 중지/재시작

```bash
docker compose restart compute-ch2
```

> `stop`/`down` 은 SIGTERM 을 보낸다. 서버는 이를 `sigwait` 스레드로 받아 **MQTT 사망 신호(LWT `"0"`)를
> 직접 발행하고** 종료하므로 control-server 가 채널 다운을 즉시 인지한다. `kill -9` 로 죽이면 이 신호가
> 나가지 않아 브로커 keepalive 만료(최대 1.5배)까지 죽은 줄 모른다.

---

## Step 5. 로그 회전 (자동 설정됨)

`setup_cctv.sh` 가 `/etc/logrotate.d/veda-cctv-01` 을 생성한다. **수동 작성 불필요.**

```
/var/log/veda/cctv_01/*.csv
/var/log/veda/cctv_01/*.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
    copytruncate
}
```

### 5-1. `copytruncate` 를 쓰는 이유

컨테이너는 비루트(uid 1000)로 실행되고 로그 파일은 호스트 볼륨에 있다. `copytruncate` 는
파일을 **rename 하지 않고** 복사 후 원본을 잘라내므로:

- **inode 와 소유권이 유지된다** → 회전 후에도 컨테이너가 같은 파일에 계속 쓴다.
- **`postrotate` 로 SIGHUP 을 보낼 필요가 없다** → `docker kill --signal=HUP` 블록과
  시간별 타이머 오버라이드가 모두 불필요해졌다(v1.0 대비 삭제됨).
- 로거가 `O_APPEND` 로 파일을 열기 때문에 truncate 후 오프셋이 0 으로 돌아가
  **sparse 파일이 생기지 않는다.**

> **트레이드오프**: `copytruncate` 는 복사와 truncate 사이에 기록된 로그를 잃을 수 있다.
> 회전 시점(하루 1회)에 한정된 수 줄 수준이며, 운영 편의와 맞바꾼 값이다.
> 무손실이 필요하면 `create` + `postrotate` SIGHUP 방식으로 되돌려야 한다
> (앱은 여전히 SIGHUP 재오픈을 지원한다).

### 5-2. 회전 동작 검증 (배포 직후 1회)

```bash
sudo logrotate -d /etc/logrotate.d/veda-cctv-01
```

```bash
sudo logrotate -f /etc/logrotate.d/veda-cctv-01 && ls -l /var/log/veda/cctv_01/
```

> **주의**: v1.0 의 전역 설정 `/etc/logrotate.d/compute-server`(글롭 `/var/log/veda/*/veda_ch*.csv`)가
> 남아 있으면 새 per-CCTV 설정과 글롭이 겹쳐 logrotate 가 `duplicate log entry` 로 **해당 실행 전체를 실패시킨다.**
> 스크립트가 이 파일을 감지하면 경고하니, 발견되면 삭제할 것.

---

## Step 6. 배포 체크리스트

- [ ] RPi 1대당 CCTV 3대(12채널)를 넘지 않는지 확인
- [ ] `sudo ./deploy/setup_cctv.sh 01` 실행 완료
- [ ] `/opt/veda/cctv_01/ch0~ch3/config.json` 배치, `channelId` 4개가 전역에서 고유한지 확인
- [ ] 각 config 의 `logFileName` = `logs/veda_ch0.csv` ~ `logs/veda_ch3.csv`
- [ ] 각 채널 `rtspSetupUri`/`rtspPlayUri` 가 서로 다른 방향을 가리키는지 확인
- [ ] `/etc/veda/certs/ca.crt` 배치(644, root), SAN 이 `mqttHost` 와 일치
- [ ] `ls -ld /var/log/veda/cctv_01` 소유자가 `1000:1000`
- [ ] `docker compose up -d` → `docker compose ps` 4개 `healthy`
- [ ] 기동 로그로 channelId·브로커 접속 확인
- [ ] `logrotate -d /etc/logrotate.d/veda-cctv-01` 정상, 구 `compute-server` 설정 없음
- [ ] control-server 의 `channelCount` 가 전체 채널 수를 포괄하는지 확인

---

## Step 7. 트러블슈팅

| 증상 | 원인 후보 | 확인/조치 |
| --- | --- | --- |
| 컨테이너가 계속 재시작 | RTSP/브로커 접속 불가 | `docker compose logs compute-chX` 에서 접속 에러 확인. `rtspSetupUri`/브로커 IP/방화벽 점검 |
| `OOMKilled` 로 종료 | Pi 1대에 CCTV 4대 이상 | `docker inspect --format '{{.State.OOMKilled}}' <컨테이너>` 확인. 12채널을 넘겼으면 Pi 를 추가 |
| `unhealthy` 로 표시 | 파이프라인 정지/RTSP 끊김 | 로그 확인. `restart: always`로 프로세스는 살아있어도 데이터가 안 흐르는 상황일 수 있음 |
| CSV 가 안 생김 | 로그 디렉터리 소유권 또는 `logFileName` 오타 | `ls -ld /var/log/veda/cctv_01` 이 `1000:1000` 인지, config 의 `logFileName` 이 `logs/…` 인지 확인 |
| TLS 접속 실패 | CA 경로/SAN 불일치 | `mqttCaFile` 이 컨테이너 내부 경로 `/etc/veda/certs/ca.crt` 인지, 인증서 SAN 이 `mqttHost` 와 같은지 확인 |
| 채널이 control-server 에서 겹침 | `channelId` 중복 | 전 CCTV·전 Pi 를 통틀어 `channelId` 가 유일한지 재확인 |
| logrotate 가 통째로 실패 | 글롭 중복 | 구 `/etc/logrotate.d/compute-server` 삭제 후 `logrotate -d` 재확인 |
| 회전 후 로그가 안 쌓임 | 소유권 변경 | `copytruncate` 는 inode 를 유지하므로 발생하면 안 된다. `create` 방식 설정이 섞였는지 확인 |
| RTSP 가 ~10초마다 끊김 | 카메라가 통보한 세션 timeout 무시 | 카메라 SETUP 응답의 `Session: …;timeout=N` 확인. 서버는 `min(설정값, N/2)` 로 keep-alive 를 보낸다 |

---

## 부록 A. 2번째 이후 CCTV 추가

```bash
sudo ./deploy/setup_cctv.sh 02
```

스크립트가 `/opt/veda/cctv_02`, `/var/log/veda/cctv_02`, `/etc/logrotate.d/veda-cctv-02` 를
모두 만든다. **수동으로 해야 하는 일은 `config.json` 배치뿐이다.**

주의할 점은 둘이다.

1. **`channelId` 는 브로커 전역에서 유일**해야 한다. 정책은 `전역 channelId = cctvId × 4 + 로컬채널(0~3)`
   이므로 `cctv_01` = 0~3, `cctv_02` = 4~7, `cctv_03` = 8~11 이다. `logFileName` 도
   (`veda_ch4.csv` …) 그에 맞춘다. control-server 의 `channelCount` 가 전체 채널 수를
   포괄하는지도 함께 확인한다.
2. **Pi 1대에는 CCTV 3대(12채널)까지만** 올린다. 4대째는 메모리 한계로 OOMKill 이 발생하므로
   Pi 를 추가하고 그쪽에서 `setup_cctv.sh 04` 를 실행한다 — `channelId` 만 유일하면
   control-server 는 어느 RPi 에서 왔는지 알 필요가 없다.
