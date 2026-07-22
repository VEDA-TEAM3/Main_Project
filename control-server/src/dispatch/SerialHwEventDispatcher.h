#pragma once

/**
 * @file    SerialHwEventDispatcher.h
 * @brief   STM32로 위험 이벤트를 실제 UART로 통지하고, STM32의 ACK/HEARTBEAT를 수신하는 구현체
 * @details ConsoleDispatcher의 "변경분만 전송" 로직은 그대로 가져오되, 콘솔 출력 대신
 *          driver_protocol.h(veda 바이너리 프로토콜)로 실제 시리얼 포트에 쓴다.
 *          별도 스레드가 상행 프레임(ACK/HEARTBEAT)을 계속 읽고, HEARTBEAT 수신 간격을
 *          감시해 missedBeatsForTimeout을 넘기면 setStatusCallback으로 등록된 콜백에
 *          alive=false를 통지한다.
 */

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "Contract.h"
#include "driver_protocol.h"
#include "interfaces/IHwEventDispatcher.h"

class SerialHwEventDispatcher : public IHwEventDispatcher {
public:
    /**
     * @param devicePath              시리얼 장치 경로 (예: "/dev/serial0")
     * @param heartbeatIntervalMs     STM32가 HEARTBEAT를 보내기로 되어 있는 주기 (ms)
     * @param missedBeatsForTimeout   연속 몇 번 유실되면 dead 판정할지
     */
    SerialHwEventDispatcher(std::string devicePath, uint32_t heartbeatIntervalMs, uint32_t missedBeatsForTimeout);
    ~SerialHwEventDispatcher() override;

    SerialHwEventDispatcher(const SerialHwEventDispatcher&) = delete;
    SerialHwEventDispatcher& operator=(const SerialHwEventDispatcher&) = delete;

    void dispatch(const domain::RiskEvaluation& eval) override;
    void setStatusCallback(StatusCallback callback) override;

private:
    void openPort();
    void readerLoop();
    void watchdogLoop();
    void handleUplinkFrame(const veda_uplink_packet_t& pkt);
    void markAlive(veda::ChannelId ch);

    std::string devicePath_;
    uint32_t heartbeatIntervalMs_;
    uint32_t missedBeatsForTimeout_;

    int fd_ = -1;

    std::unordered_map<veda::ChannelId, veda::RiskLevel> lastSentLevel_;
    StatusCallback statusCallback_;

    std::mutex heartbeatMutex_;
    std::unordered_map<veda::ChannelId, std::chrono::steady_clock::time_point> lastHeartbeatAt_;
    std::unordered_map<veda::ChannelId, bool> aliveState_;

    std::atomic<bool> running_{false};
    std::thread readerThread_;
    std::thread watchdogThread_;
};
