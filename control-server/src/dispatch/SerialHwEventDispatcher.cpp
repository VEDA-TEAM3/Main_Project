#include "dispatch/SerialHwEventDispatcher.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

SerialHwEventDispatcher::SerialHwEventDispatcher(std::string devicePath, uint32_t heartbeatIntervalMs,
                                                 uint32_t missedBeatsForTimeout)
    : devicePath_(std::move(devicePath)),
      heartbeatIntervalMs_(heartbeatIntervalMs),
      missedBeatsForTimeout_(missedBeatsForTimeout) {
    openPort();

    running_ = true;
    readerThread_ = std::thread(&SerialHwEventDispatcher::readerLoop, this);
    watchdogThread_ = std::thread(&SerialHwEventDispatcher::watchdogLoop, this);
}

SerialHwEventDispatcher::~SerialHwEventDispatcher() {
    running_ = false;
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

/**
 * @details 포트를 못 열어도 예외를 던지지 않는다(AppConfig::load와 동일한 원칙).
 *          fd_ == -1로 남기고, dispatch()/readerLoop()가 이를 감지해 조용히 스킵한다.
 *          TODO: 재연결 로직은 없음 — 지금은 서버 재시작으로만 복구 가능
 */
void SerialHwEventDispatcher::openPort() {
    fd_ = open(devicePath_.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        std::cerr << "[SerialHwEventDispatcher] 포트 열기 실패: " << devicePath_ << " (" << strerror(errno) << ")"
                  << " — STM32로 전송/수신이 비활성화됩니다.\n";
        return;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    tcgetattr(fd_, &tty);

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_iflag = 0;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;  // read() 1바이트당 최대 1초 대기 (종료 시 readerLoop가 빨리 빠져나오도록)

    tcsetattr(fd_, TCSANOW, &tty);

    std::cout << "[SerialHwEventDispatcher] " << devicePath_ << " 연결됨 (115200 8N1)\n";
}

/**
 * @details ConsoleDispatcher와 동일하게 이전에 실제로 전송 성공한 값과 비교해 변경분만 보낸다.
 *          IHwEventDispatcher.h의 @note대로, 비교 기준은 "마지막 전송 성공 값"이어야 유실 시
 *          재전송 누락이 안 생긴다 — write()가 실패하면 lastSentLevel_을 갱신하지 않는다.
 */
void SerialHwEventDispatcher::dispatch(const domain::RiskEvaluation& eval) {
    if (fd_ < 0) {
        return;
    }

    for (const auto& zone : eval.zoneLevels) {
        std::cout << "[Dispatcher] UART 이벤트 통지 → 채널 " << zone.zoneId << "\n";

        auto it = lastSentLevel_.find(zone.zoneId);
        const bool changed = (it == lastSentLevel_.end()) || (it->second != zone.level);
        if (!changed) {
            continue;
        }

        veda_risk_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.channel_id = static_cast<uint8_t>(zone.zoneId);
        ev.risk_level = static_cast<uint8_t>(zone.level);
        ev.timestamp_ms = eval.timestamp;
        ev.dist_mm = (zone.minDist >= 0.0) ? static_cast<uint16_t>(zone.minDist * 1000.0) : VEDA_DIST_MM_NONE;

        veda_downlink_frame_t frame;
        frame.start_byte = VEDA_START_BYTE;
        frame.payload = ev;
        frame.checksum = veda_downlink_checksum(&ev);
        frame.end_byte = VEDA_END_BYTE;

        ssize_t written = write(fd_, &frame, sizeof(frame));
        if (written != static_cast<ssize_t>(sizeof(frame))) {
            std::cerr << "[SerialHwEventDispatcher] 전송 실패: 채널 " << zone.zoneId << " (" << strerror(errno)
                      << ")\n";
            continue;  // lastSentLevel_ 갱신 안 함 -> 다음 프레임에서 재시도됨
        }

        lastSentLevel_[zone.zoneId] = zone.level;
    }
}

/**
 * @details readerLoop()는 콜백 등록 여부와 무관하게 생성 시점부터 계속 heartbeat를 수신해
 *          aliveState_를 갱신해왔다. 여기서 콜백을 뒤늦게 등록하면, 등록 이전에 이미 파악된
 *          채널별 상태는 다음 전환(alive↔dead)이 생길 때까지 통지되지 않으므로,
 *          등록 즉시 현재 aliveState_ 스냅샷을 한 번 통지해 그 공백을 없앤다.
 */
void SerialHwEventDispatcher::setStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    statusCallback_ = std::move(callback);

    if (statusCallback_) {
        for (const auto& [ch, alive] : aliveState_) {
            statusCallback_(ch, alive);
        }
    }
}

/**
 * @details STM32 rx_task와 대칭인 상행 프레임 동기화 상태머신.
 *          START_BYTE를 찾을 때까지 앞의 쓰레기 바이트는 건너뛰고, payload(16B)+checksum+END_BYTE가
 *          모두 맞아야 유효한 프레임으로 처리한다. 체크섬/END가 어긋나면 조용히 버리고 재동기화.
 */
void SerialHwEventDispatcher::readerLoop() {
    enum State { WAIT_START, READ_PAYLOAD, READ_CHECKSUM, WAIT_END };
    State state = WAIT_START;
    uint8_t payloadBuf[sizeof(veda_uplink_packet_t)];
    size_t payloadIdx = 0;
    uint8_t rxChecksum = 0;

    while (running_) {
        if (fd_ < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        uint8_t byte;
        ssize_t n = read(fd_, &byte, 1);
        if (n <= 0) {
            continue;  // 타임아웃(VTIME=10) 또는 일시적 에러 -> running_ 체크 후 계속
        }

        switch (state) {
            case WAIT_START:
                if (byte == VEDA_START_BYTE) {
                    payloadIdx = 0;
                    state = READ_PAYLOAD;
                }
                break;

            case READ_PAYLOAD:
                payloadBuf[payloadIdx++] = byte;
                if (payloadIdx == sizeof(payloadBuf)) {
                    state = READ_CHECKSUM;
                }
                break;

            case READ_CHECKSUM:
                rxChecksum = byte;
                state = WAIT_END;
                break;

            case WAIT_END:
                if (byte == VEDA_END_BYTE && veda_checksum(payloadBuf, sizeof(payloadBuf)) == rxChecksum) {
                    veda_uplink_packet_t pkt;
                    memcpy(&pkt, payloadBuf, sizeof(pkt));
                    handleUplinkFrame(pkt);
                }
                state = WAIT_START;
                break;
        }
    }
}

void SerialHwEventDispatcher::handleUplinkFrame(const veda_uplink_packet_t& pkt) {
    if (pkt.reason == VEDA_UPLINK_REASON_HEARTBEAT) {
        markAlive(static_cast<veda::ChannelId>(pkt.channel_id));
    }
    // ACK(reason == VEDA_UPLINK_REASON_ACK)은 지금은 별도 처리 없음.
    // TODO: 명령-상태 불일치 재시도 정책(mismatchRetryCount)을 여기서 검증하려면
    // dispatch()가 마지막으로 보낸 값과 이 ACK의 siren/buzzer/led 값을 비교해야 함.
}

void SerialHwEventDispatcher::markAlive(veda::ChannelId ch) {
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    lastHeartbeatAt_[ch] = std::chrono::steady_clock::now();

    auto it = aliveState_.find(ch);
    const bool wasAlive = (it != aliveState_.end()) && it->second;
    aliveState_[ch] = true;

    if (!wasAlive && statusCallback_) {
        statusCallback_(ch, true);
    }
}

/**
 * @details heartbeatIntervalMs_마다 깨어나서, 마지막 HEARTBEAT 이후
 *          missedBeatsForTimeout_ * heartbeatIntervalMs_를 넘긴 채널을 dead로 판정한다.
 */
void SerialHwEventDispatcher::watchdogLoop() {
    const auto timeoutDuration = std::chrono::milliseconds(static_cast<uint64_t>(heartbeatIntervalMs_) *
                                                           static_cast<uint64_t>(missedBeatsForTimeout_));

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(heartbeatIntervalMs_));

        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        auto now = std::chrono::steady_clock::now();

        for (auto& [ch, alive] : aliveState_) {
            if (!alive) {
                continue;
            }
            auto lastIt = lastHeartbeatAt_.find(ch);
            const bool timedOut = (lastIt == lastHeartbeatAt_.end()) || (now - lastIt->second > timeoutDuration);
            if (timedOut) {
                alive = false;
                if (statusCallback_) {
                    statusCallback_(ch, false);
                }
            }
        }
    }
}
