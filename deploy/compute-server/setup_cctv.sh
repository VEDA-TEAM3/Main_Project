#!/usr/bin/env bash
set -euo pipefail

ID=${1:-}
[[ $ID =~ ^[0-9]{2}$ ]] || { echo "usage: $0 <CCTV_ID: 01..99>" >&2; exit 1; }

IMAGE=${IMAGE:-veda/compute-server:1.0.0}
CHANNELS=${CHANNELS:-4}
ROOT=${ROOT:-/opt/veda}
LOGROOT=${LOGROOT:-/var/log/veda}
CA=${CA:-/etc/veda/certs/ca.crt}
LRDIR=${LRDIR:-/etc/logrotate.d}

BASE=$ROOT/cctv_${ID}
LOGDIR=$LOGROOT/cctv_${ID}
LRCONF=$LRDIR/veda-cctv-${ID}

[[ $ROOT != /opt/veda || $EUID -eq 0 ]] || { echo "root 권한 필요 (sudo)" >&2; exit 1; }

mkdir -p "$LOGDIR"
for ((i = 0; i < CHANNELS; i++)); do mkdir -p "$BASE/ch$i"; done

TMP=$(mktemp "$BASE/.compose.XXXXXX")
trap 'rm -f "$TMP"' EXIT

cat > "$TMP" <<YAML
# 자동 생성: setup_cctv.sh ${ID} -- 직접 수정하지 말 것 (재실행 시 덮어써짐)
name: veda-cctv-${ID}

x-compute-common: &compute-common
  image: ${IMAGE}
  restart: always
  working_dir: /opt/veda
  stop_grace_period: 15s
  # RTSP(카메라)/MQTT(브로커) 모두 아웃바운드 전용이라 열 포트가 없다 -> host 네트워크로
  # NAT 홉을 제거해 지연을 줄인다. network_mode 를 쓰면 networks: 키는 함께 쓸 수 없다.
  network_mode: "host"
  deploy:
    resources:
      limits:
        cpus: "0.75"
        memory: 256M
  healthcheck:
    test: ["CMD-SHELL", "pgrep compute-server || exit 1"]
    interval: 30s
    timeout: 5s
    retries: 3
    start_period: 60s

services:
YAML

for ((i = 0; i < CHANNELS; i++)); do
    cat >> "$TMP" <<YAML
  compute-ch${i}:
    <<: *compute-common
    container_name: cctv_${ID}_ch${i}
    volumes:
      - ./ch${i}/config.json:/opt/veda/config.json:ro
      - ${CA}:${CA}:ro
      - ${LOGDIR}:/opt/veda/logs

YAML
done

mv "$TMP" "$BASE/docker-compose.yml"
trap - EXIT
chmod 644 "$BASE/docker-compose.yml"

if [[ $EUID -eq 0 ]]; then
    chown -R 1000:1000 "$LOGDIR"
    chmod 755 "$LOGDIR"
else
    echo "경고: 비루트 실행 -- chown 1000:1000 $LOGDIR 를 건너뜀" >&2
fi

if command -v docker > /dev/null; then docker compose -f "$BASE/docker-compose.yml" config -q; fi

if [[ $EUID -eq 0 || $LRDIR != /etc/logrotate.d ]]; then
    mkdir -p "$LRDIR"
    TMPLR=$(mktemp "$LRDIR/.veda-cctv.XXXXXX")
    trap 'rm -f "$TMPLR"' EXIT
    cat > "$TMPLR" <<CONF
# 자동 생성: setup_cctv.sh ${ID} -- 직접 수정하지 말 것 (재실행 시 덮어써짐)
${LOGDIR}/*.csv
${LOGDIR}/*.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
    copytruncate
}
CONF
    mv "$TMPLR" "$LRCONF"
    trap - EXIT
    chmod 644 "$LRCONF"
    if command -v logrotate > /dev/null; then
        logrotate -d "$LRCONF" > /dev/null 2>&1 ||
            { echo "logrotate 설정 검증 실패: $LRCONF" >&2; exit 1; }
    fi
else
    echo "경고: 비루트 실행 -- $LRCONF 생성을 건너뜀" >&2
fi

# 글롭이 겹치면 logrotate 는 'duplicate log entry' 로 해당 실행 전체를 실패시킨다.
for legacy in "$LRDIR/compute-server" "$LRDIR/veda-compute"; do
    if [[ -f $legacy ]]; then
        echo "경고: $legacy 가 $LOGDIR 와 겹칠 수 있음 -- 제거 후 'logrotate -d' 재확인" >&2
    fi
done

echo "OK: $BASE (${CHANNELS}ch, ${IMAGE})"
echo "     logrotate: $LRCONF (daily, rotate 7, copytruncate)"
for ((i = 0; i < CHANNELS; i++)); do
    [[ -f $BASE/ch$i/config.json ]] ||
        echo "  TODO: $BASE/ch$i/config.json  (channelId=$i, logFileName=logs/veda_ch$i.csv)"
done
[[ -f $CA ]] || echo "  TODO: $CA"