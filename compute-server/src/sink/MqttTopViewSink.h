#pragma once

/**
 * @file    MqttTopViewSink.h
 * @brief   TopViewFrame 전용 MQTT 발행 Sink
 *
 * @details
 * 큐잉/워커/드랍 집계는 MqttFrameSink<T> 가, 연결은 IMqttTransport 가 담당하므로
 * 이 클래스에는 'TopViewFrame 을 어떻게 검증하고 어느 토픽으로 보낼지'만 남음
 */

#include <cstddef>
#include <memory>
#include <string>

#include "Contract.h"
#include "core/AppConfig.h"
#include "interfaces/IMqttTransport.h"
#include "sink/MqttFrameSink.h"

class MqttTopViewSink final : public MqttFrameSink<veda::TopViewFrame> {
public:
    MqttTopViewSink(std::shared_ptr<IMqttTransport> transport, const AppConfig& config);
    ~MqttTopViewSink() override;

protected:
    bool prepare(const veda::TopViewFrame& in, veda::TopViewFrame& out) override;
    std::string describe(const veda::TopViewFrame& frame) const override;

private:
    bool isValidFrame(const veda::TopViewFrame& frame) const noexcept;

    veda::ChannelId channelId_;  ///< 이 프로세스의 채널 (frame.ch는 반드시 이 값과 같아야 함)
};