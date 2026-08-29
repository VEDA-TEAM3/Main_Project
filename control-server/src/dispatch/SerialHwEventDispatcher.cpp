#include "dispatch/SerialHwEventDispatcher.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

#include "dispatch/SerialEventEncoding.h"

namespace {

veda::RiskLevel reportedRiskLevel(const HwIndicatorState& indicators) {
    if (indicators.ledRed) {
        return veda::RiskLevel::Danger;
    }
    if (indicators.ledYellow) {
        return veda::RiskLevel::Warning;
    }
    return veda::RiskLevel::None;
}

HwIndicatorState decodeIndicators(const veda_uplink_packet_t& packet) {
    return {packet.siren_on != 0, packet.buzzer_on != 0, packet.led_red != 0, packet.led_yellow != 0,
            packet.led_green != 0};
}

}  // namespace

SerialHwEventDispatcher::SerialHwEventDispatcher(std::string devicePath, uint32_t heartbeatIntervalMs,
                                                 uint32_t missedBeatsForTimeout, uint32_t mismatchRetryCount,
                                                 bool mismatchEscalateAfterRetries)
    : devicePath_(std::move(devicePath)),
      heartbeatIntervalMs_(heartbeatIntervalMs),
      missedBeatsForTimeout_(missedBeatsForTimeout),
      mismatchRetryCount_(mismatchRetryCount),
      mismatchEscalateAfterRetries_(mismatchEscalateAfterRetries) {
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
bool SerialHwEventDispatcher::sendRiskEventLocked(veda::ChannelId channel, veda::RiskLevel level,
                                                       veda::TimestampMs timestamp, std::uint16_t distanceMm) {
    if (fd_ < 0 || !serial_event::isValidChannelId(channel)) {
        return false;
    }

    veda_risk_event_t event{};
    event.channel_id = static_cast<std::uint8_t>(channel);
    event.risk_level = static_cast<std::uint8_t>(level);
    veda_write_i64_le(&event.timestamp_ms, timestamp);
    veda_write_u16_le(&event.dist_mm, distanceMm);

    veda_downlink_frame_t frame{};
    frame.start_byte = VEDA_START_BYTE;
    frame.payload = event;
    frame.checksum = veda_downlink_checksum(&event);
    frame.end_byte = VEDA_END_BYTE;

    const ssize_t written = write(fd_, &frame, sizeof(frame));
    if (written != static_cast<ssize_t>(sizeof(frame))) {
        std::cerr << "[SerialHwEventDispatcher] 전송 실패: 채널 " << channel << " (" << strerror(errno) << ")\n";
        return false;
    }
    return true;
}

void SerialHwEventDispatcher::dispatch(const domain::RiskEvaluation& eval) {
    std::lock_guard<std::mutex> lock(sendStateMutex_);
    for (const auto& zone : eval.zoneLevels) {
        if (!serial_event::isValidChannelId(zone.zoneId)) {
            continue;
        }
        const auto previous = lastSentLevel_.find(zone.zoneId);
        if (previous != lastSentLevel_.end() && previous->second == zone.level) {
            continue;
        }
        if (sendRiskEventLocked(zone.zoneId, zone.level, eval.timestamp,
                                serial_event::encodeDistanceMm(zone.minDist))) {
            lastSentLevel_[zone.zoneId] = zone.level;
            mismatchRetryAttempts_[zone.zoneId] = 0;
        }
    }
}

/**
 * @details readerLoop()는 콜백 등록 여부와 무관하게 생성 시점부터 계속 heartbeat를 수신해
 *          채널별 상태를 갱신한다. 콜백 등록 즉시 현재 스냅샷을 재생해 등록 전 공백을 없앤다.
 */
void SerialHwEventDispatcher::setStatusCallback(StatusCallback callback) {
    std::vector<std::pair<veda::ChannelId, ReportedState>> snapshot;
    StatusCallback callbackCopy;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        statusCallback_ = std::move(callback);
        callbackCopy = statusCallback_;
        if (callbackCopy) {
            snapshot.assign(reportedState_.begin(), reportedState_.end());
        }
    }
    for (const auto& [channel, state] : snapshot) {
        callbackCopy(channel, state.alive, state.indicators);
    }
}

void SerialHwEventDispatcher::setFaultCallback(FaultCallback callback) {
    std::vector<std::pair<veda::ChannelId, bool>> snapshot;
    FaultCallback callbackCopy;
    {
        std::lock_guard<std::mutex> lock(sendStateMutex_);
        faultCallback_ = std::move(callback);
        callbackCopy = faultCallback_;
        if (callbackCopy) {
            snapshot.assign(faultState_.begin(), faultState_.end());
        }
    }
    for (const auto& [channel, faulted] : snapshot) {
        callbackCopy(channel, faulted);
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
                    if (veda_uplink_payload_is_valid(&pkt)) {
                        handleUplinkFrame(pkt);
                    } else {
                        std::cerr << "[SerialHwEventDispatcher] UART 상행 payload 필드 검증 실패\n";
                    }
                }
                state = WAIT_START;
                break;
        }
    }
}

void SerialHwEventDispatcher::handleUplinkFrame(const veda_uplink_packet_t& packet) {
    const auto channel = static_cast<veda::ChannelId>(packet.channel_id);
    const HwIndicatorState indicators = decodeIndicators(packet);
    reportAlive(channel, indicators);

    FaultCallback callback;
    bool notifyFault = false;
    bool faulted = false;
    {
        std::lock_guard<std::mutex> lock(sendStateMutex_);
        const auto expected = lastSentLevel_.find(channel);
        if (expected == lastSentLevel_.end()) {
            return;
        }
        if (reportedRiskLevel(indicators) == expected->second) {
            mismatchRetryAttempts_[channel] = 0;
            auto fault = faultState_.find(channel);
            if (fault != faultState_.end() && fault->second) {
                fault->second = false;
                callback = faultCallback_;
                notifyFault = true;
            }
        } else {
            auto& attempts = mismatchRetryAttempts_[channel];
            if (attempts < mismatchRetryCount_) {
                if (sendRiskEventLocked(channel, expected->second, veda_read_i64_le(&packet.timestamp_ms),
                                        VEDA_DIST_MM_NONE)) {
                    ++attempts;
                }
            } else if (mismatchEscalateAfterRetries_ && !faultState_[channel]) {
                faultState_[channel] = true;
                callback = faultCallback_;
                notifyFault = true;
                faulted = true;
            }
        }
    }
    if (notifyFault && callback) {
        callback(channel, faulted);
    }
}

void SerialHwEventDispatcher::reportAlive(veda::ChannelId channel, const HwIndicatorState& indicators) {
    StatusCallback callback;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        lastHeartbeatAt_[channel] = std::chrono::steady_clock::now();
        auto& state = reportedState_[channel];
        changed = !state.alive || state.indicators != indicators;
        state.alive = true;
        state.indicators = indicators;
        callback = statusCallback_;
    }
    if (changed && callback) {
        callback(channel, true, indicators);
    }
}

void SerialHwEventDispatcher::reportDead(veda::ChannelId channel) {
    StatusCallback callback;
    HwIndicatorState indicators;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        auto state = reportedState_.find(channel);
        if (state != reportedState_.end() && state->second.alive) {
            state->second.alive = false;
            indicators = state->second.indicators;
            callback = statusCallback_;
            changed = true;
        }
    }
    if (changed && callback) {
        callback(channel, false, indicators);
    }
}

void SerialHwEventDispatcher::watchdogLoop() {
    const auto timeoutDuration = std::chrono::milliseconds(static_cast<uint64_t>(heartbeatIntervalMs_) *
                                                           static_cast<uint64_t>(missedBeatsForTimeout_));
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(heartbeatIntervalMs_));
        std::vector<veda::ChannelId> timedOutChannels;
        {
            std::lock_guard<std::mutex> lock(heartbeatMutex_);
            const auto now = std::chrono::steady_clock::now();
            for (const auto& [channel, state] : reportedState_) {
                if (!state.alive) {
                    continue;
                }
                const auto last = lastHeartbeatAt_.find(channel);
                if (last == lastHeartbeatAt_.end() || now - last->second > timeoutDuration) {
                    timedOutChannels.push_back(channel);
                }
            }
        }
        for (const auto channel : timedOutChannels) {
            reportDead(channel);
        }
    }
}
