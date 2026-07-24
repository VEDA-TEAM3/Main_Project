#include "dispatch/SerialHwEventDispatcher.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "Logger.h"

namespace {
constexpr const char* kIface = "HwDispatcher";

/**
 * @brief   veda_uplink_packet_t 의 개별 LED 상태를 RiskLevel 로 디코드
 * @details driver_protocol.h 의 veda_uplink_packet_t 에는 risk_level 필드가 없고,
 *          STM32 가 실제로 켜고 있는 led_red/led_yellow/led_green 불리언만 올라온다
 *          (하행 veda_risk_event_t.risk_level 과 대칭이 아님).
 *          신호등 관례: led_red -> Danger, led_yellow -> Warning, 그 외 -> None.
 *          두 LED 가 동시에 켜진 경우(전이 중 순간 등) 더 위험한 쪽을 우선한다.
 */
veda::RiskLevel decodeRiskLevel(const veda_uplink_packet_t& pkt) {
    if (pkt.led_red)
        return veda::RiskLevel::Danger;
    if (pkt.led_yellow)
        return veda::RiskLevel::Warning;
    return veda::RiskLevel::None;
}

/// @brief veda_uplink_packet_t 의 개별 표시 상태를 그대로 HwIndicatorState 로 옮김
HwIndicatorState decodeIndicators(const veda_uplink_packet_t& pkt) {
    return HwIndicatorState{static_cast<bool>(pkt.siren_on), static_cast<bool>(pkt.buzzer_on),
                            static_cast<bool>(pkt.led_red), static_cast<bool>(pkt.led_yellow),
                            static_cast<bool>(pkt.led_green)};
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
        logError(kIface,
                 "포트 열기 실패: " + devicePath_ + " (" + strerror(errno) + ") — STM32로 전송/수신이 비활성화됩니다");
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

    logSuccess(kIface, devicePath_ + " 연결됨 (115200 8N1)");
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

    std::lock_guard<std::mutex> lock(sendStateMutex_);

    for (const auto& zone : eval.zoneLevels) {
        auto it = lastSentLevel_.find(zone.zoneId);
        const bool changed = (it == lastSentLevel_.end()) || (it->second != zone.level);
        if (!changed) {
            // 변경분만 전송 -- 예전에는 여기 도달 전에 매 zone 마다 콘솔에 찍고 있어서
            // 윈도우마다(초당 10회) x 채널수 만큼 동기 출력이 발생했음
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
            logError(kIface, "전송 실패: 채널 " + std::to_string(zone.zoneId) + " (" + strerror(errno) + ")");
            continue;  // lastSentLevel_ 갱신 안 함 -> 다음 프레임에서 재시도됨
        }

        // [LOAD-BEARING: zoneId == channelId] send 측 상태를 zoneId 로 키잉한다.
        // 상행 비교(checkChannelMismatch)는 pkt.channel_id 로 조회하므로, 둘이 같은 정수여야
        // 성립한다. 자세한 경고는 checkChannelMismatch 상단 참고.
        lastSentLevel_[zone.zoneId] = zone.level;
        // 새 명령을 보냈으니 이전 불일치 재시도 카운트는 리셋 -- 이 명령에 대한 새 ACK로 다시 판정
        mismatchRetryAttempts_.erase(zone.zoneId);
        // 레벨이 바뀔 때만 찍히므로 Info 여도 도배되지 않음 (상태 전이 = 운영자가 봐야 할 이벤트)
        logSuccess(kIface, "UART 통지 채널 " + std::to_string(zone.zoneId) + " -> " +
                               std::string(veda::toString(zone.level)) + " (거리 " + std::to_string(zone.minDist) +
                               "m)");
    }
}

/**
 * @details readerLoop()는 콜백 등록 여부와 무관하게 생성 시점부터 계속 상행 프레임을 수신해
 *          reportedState_를 갱신해왔다. 여기서 콜백을 뒤늦게 등록하면, 등록 이전에 이미 파악된
 *          채널별 상태는 다음 전환(alive/indicators 변화)이 생길 때까지 통지되지 않으므로,
 *          등록 즉시 현재 reportedState_ 스냅샷을 한 번 통지해 그 공백을 없앤다.
 */
void SerialHwEventDispatcher::setStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    statusCallback_ = std::move(callback);

    if (statusCallback_) {
        for (const auto& [ch, state] : reportedState_) {
            statusCallback_(ch, state.alive, state.indicators);
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
    const auto ch = static_cast<veda::ChannelId>(pkt.channel_id);

    if (pkt.reason == VEDA_UPLINK_REASON_HEARTBEAT) {
        reportAlive(ch, true);
    }
    // ACK/HEARTBEAT 둘 다 led_red/led_yellow/led_green 상태를 담고 있으므로, reason과
    // 무관하게 매 프레임마다 명령-상태 불일치를 검증하고, 표시 상태 변화를 통지한다
    // (HEARTBEAT는 새 명령 없이도 드리프트를 계속 감시할 수 있다는 부수 효과가 있음).
    reportIndicators(ch, decodeIndicators(pkt));
    checkChannelMismatch(pkt);
}

/**
 * @details pkt의 led_red/led_yellow 상태를 decodeRiskLevel()로 RiskLevel 로 디코드해
 *          (그 채널 LED가 실제로 표시 중인 값), 이 채널에 마지막으로 보낸 명령(lastSentLevel_)과
 *          비교한다.
 *          - 아직 명령을 보낸 적 없는 채널은 비교 기준이 없으므로 스킵
 *          - 일치하면 재시도 카운터를 리셋하고 fault 상태였다면 해소 처리
 *          - 불일치면 재시도 카운터를 올리고 mismatchRetryCount_ 이내면 재전송,
 *            소진되면 mismatchEscalateAfterRetries_ 설정에 따라 에스컬레이션
 */
void SerialHwEventDispatcher::checkChannelMismatch(const veda_uplink_packet_t& pkt) {
    const auto ch = static_cast<veda::ChannelId>(pkt.channel_id);
    const veda::RiskLevel reportedLevel = decodeRiskLevel(pkt);

    std::lock_guard<std::mutex> lock(sendStateMutex_);

    // ============================================================================
    // ⚠  하중을 견디는 가정 (LOAD-BEARING ASSUMPTION): zoneId == channelId
    // ----------------------------------------------------------------------------
    // 이 불일치/fault 로직은 "우리가 보낸 명령"과 "STM32 가 보고한 실제 상태"를 비교한다:
    //   - 하행(dispatch):  lastSentLevel_[zone.zoneId]      // zoneId 를 키로 저장
    //   - 상행(여기):      lastSentLevel_.find(pkt.channel_id) // channel_id 를 키로 조회
    // 이 비교는 두 키가 같은 정수라는 사실(현재 zone 과 채널이 1:1, zoneId == channelId)에
    // 전적으로 의존한다.
    //
    // 만약 향후 ZoneId 와 ChannelId 를 분리하면(예: 한 zone 이 여러 채널을 구동), 두 정수 타입이
    // 여전히 int 라 컴파일은 통과하지만, 여기서 '엉뚱한 채널끼리' 레벨을 비교하게 된다. 그 결과
    // 명령-상태 불일치 감지 / 재전송 / fault 에스컬레이션이 조용히 오작동하고, 실제 하드웨어가
    // 틀린 상태인데도 서버는 정상으로 착각한다(= silent hardware failure).
    // 분리하려면 반드시 send 측 상태(lastSentLevel_ / mismatchRetryAttempts_ / faultState_)를
    // zoneId 가 아니라 '번역된 ChannelId' 로 다시 키잉할 것. 절대 타입만 무심코 갈라놓지 말 것.
    // ============================================================================
    auto sentIt = lastSentLevel_.find(ch);
    if (sentIt == lastSentLevel_.end()) {
        return;  // 이 채널에 아직 명령을 보낸 적 없음 -> 비교 기준이 없으므로 스킵
    }

    if (sentIt->second == reportedLevel) {
        mismatchRetryAttempts_.erase(ch);
        clearFault(ch);
        return;
    }

    const uint32_t attempts = ++mismatchRetryAttempts_[ch];

    logError(kIface, "채널 " + std::to_string(ch) +
                         " 명령-상태 불일치: 기대=" + std::string(veda::toString(sentIt->second)) +
                         " 실제=" + std::string(veda::toString(reportedLevel)) + " (재시도 " +
                         std::to_string(attempts) + "/" + std::to_string(mismatchRetryCount_) + ")");

    if (attempts <= mismatchRetryCount_) {
        resendLastCommand(ch, sentIt->second);
        return;
    }

    if (mismatchEscalateAfterRetries_) {
        raiseFault(ch);
    }
}

/**
 * @details lastSentLevel_에 저장된 값을 그대로 새 타임스탬프로 재전송한다.
 *          원본 dist_mm은 알 수 없으므로 VEDA_DIST_MM_NONE으로 보낸다 -- 재전송의
 *          목적은 "이 채널이 어떤 risk_level을 표시해야 하는지"를 다시 알리는 것이지
 *          원래 프레임의 거리 측정값을 복원하는 게 아니기 때문이다.
 * @note 호출자(checkChannelMismatch)가 이미 sendStateMutex_를 잡고 있는 상태에서 불린다.
 */
void SerialHwEventDispatcher::resendLastCommand(veda::ChannelId ch, veda::RiskLevel level) {
    if (fd_ < 0) {
        return;
    }

    veda_risk_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.channel_id = static_cast<uint8_t>(ch);
    ev.risk_level = static_cast<uint8_t>(level);
    ev.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    ev.dist_mm = VEDA_DIST_MM_NONE;

    veda_downlink_frame_t frame;
    frame.start_byte = VEDA_START_BYTE;
    frame.payload = ev;
    frame.checksum = veda_downlink_checksum(&ev);
    frame.end_byte = VEDA_END_BYTE;

    ssize_t written = write(fd_, &frame, sizeof(frame));
    if (written != static_cast<ssize_t>(sizeof(frame))) {
        logError(kIface, "재전송 실패: 채널 " + std::to_string(ch) + " (" + strerror(errno) + ")");
        return;
    }

    logSuccess(kIface, "채널 " + std::to_string(ch) + " 명령 재전송 (" + std::string(veda::toString(level)) + ")");
}

/// @note 호출자가 이미 sendStateMutex_를 잡고 있는 상태에서 불린다.
void SerialHwEventDispatcher::raiseFault(veda::ChannelId ch) {
    auto it = faultState_.find(ch);
    const bool wasFaulted = (it != faultState_.end()) && it->second;
    faultState_[ch] = true;

    if (!wasFaulted) {
        logError(kIface, "채널 " + std::to_string(ch) + " 재시도 소진 -> fault 에스컬레이션");
        if (faultCallback_) {
            faultCallback_(ch, true);
        }
    }
}

/// @note 호출자가 이미 sendStateMutex_를 잡고 있는 상태에서 불린다.
void SerialHwEventDispatcher::clearFault(veda::ChannelId ch) {
    auto it = faultState_.find(ch);
    const bool wasFaulted = (it != faultState_.end()) && it->second;
    faultState_[ch] = false;

    if (wasFaulted) {
        logSuccess(kIface, "채널 " + std::to_string(ch) + " fault 해소");
        if (faultCallback_) {
            faultCallback_(ch, false);
        }
    }
}

/**
 * @details setStatusCallback과 동일한 이유로, 등록 즉시 현재 파악된 채널별 fault 상태를
 *          스냅샷으로 한 번 통지해 등록 이전 상태 공백을 없앤다.
 */
void SerialHwEventDispatcher::setFaultCallback(FaultCallback callback) {
    std::lock_guard<std::mutex> lock(sendStateMutex_);
    faultCallback_ = std::move(callback);

    if (faultCallback_) {
        for (const auto& [ch, faulted] : faultState_) {
            faultCallback_(ch, faulted);
        }
    }
}

/**
 * @details alive 상태를 갱신하고, 실제로 바뀐 경우에만 콜백을 통지한다. HEARTBEAT를 받을
 *          때마다(alive=true) lastHeartbeatAt_는 전이 여부와 무관하게 항상 갱신해야
 *          watchdogLoop()가 타임아웃을 정확히 판단할 수 있다.
 */
void SerialHwEventDispatcher::reportAlive(veda::ChannelId ch, bool alive) {
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    if (alive) {
        lastHeartbeatAt_[ch] = std::chrono::steady_clock::now();
    }

    ReportedState& state = reportedState_[ch];
    if (state.alive == alive) {
        return;
    }
    state.alive = alive;

    if (statusCallback_) {
        statusCallback_(ch, state.alive, state.indicators);
    }
}

/// @details 표시 상태(led/siren/buzzer)가 실제로 바뀐 경우에만 콜백을 통지한다.
void SerialHwEventDispatcher::reportIndicators(veda::ChannelId ch, const HwIndicatorState& indicators) {
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    ReportedState& state = reportedState_[ch];
    if (state.indicators == indicators) {
        return;
    }
    state.indicators = indicators;

    if (statusCallback_) {
        statusCallback_(ch, state.alive, state.indicators);
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

        std::vector<veda::ChannelId> timedOutChannels;
        {
            std::lock_guard<std::mutex> lock(heartbeatMutex_);
            const auto now = std::chrono::steady_clock::now();
            for (const auto& [ch, state] : reportedState_) {
                if (!state.alive) {
                    continue;
                }
                auto lastIt = lastHeartbeatAt_.find(ch);
                const bool timedOut = (lastIt == lastHeartbeatAt_.end()) || (now - lastIt->second > timeoutDuration);
                if (timedOut) {
                    timedOutChannels.push_back(ch);
                }
            }
        }

        for (veda::ChannelId ch : timedOutChannels) {
            reportAlive(ch, false);
        }
    }
}
