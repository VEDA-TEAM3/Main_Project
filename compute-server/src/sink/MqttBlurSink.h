#pragma once

/**
 * @file    MqttBlurSink.h
 * @brief   BlurFrame 전용 MQTT 발행 Sink
 *
 * @details
 * 큐잉/워커/드랍 집계는 MqttFrameSink<T>가, 연결은 IMqttTransport가 담당하므로
 * 이 클래스에는 BlurFrame을 어떻게 검증하고 어느 토픽으로 보낼지만 남음
 */

#include <cstddef>
#include <memory>
#include <string>

#include "Contract.h"
#include "core/AppConfig.h"
#include "interfaces/IMqttTransport.h"
#include "sink/MqttFrameSink.h"

class MqttBlurSink final : public MqttFrameSink<veda::BlurFrame> {
public:
    MqttBlurSink(std::shared_ptr<IMqttTransport> transport, const AppConfig& config);
    ~MqttBlurSink() override;

protected:
    bool prepare(const veda::BlurFrame& in, veda::BlurFrame& out) override;
    std::string describe(const veda::BlurFrame& frame) const override;

private:
    /**
     * @brief   프레임 단위 유효성 검사 (스키마 버전/타임스탬프/채널 범위)
     * @details 여기서 실패하면 Blurs 내용과 무관하게 프레임 자체가 구조적으로 잘못된 것이므로
     *          프레임 전체를 버림
     *          개별 Blur 대상 하나의 클래스/좌표 문제는 isValidBlurTarget()이 담당
     *          → 얼굴 하나가 인식 안 되는 클래스라고 같은 프레임의 나머지 Blur까지
     *          통째로 버려지는 일이 없도록 분리함
     */
    bool isValidFrame(const veda::BlurFrame& frame) const noexcept;

    /// @brief 개별 Blur 대상 하나의 유효성 검사 (Head/LicensePlate 또는 parent-derived Unknown인지, box 좌표가 정상인지)
    bool isValidBlurTarget(const veda::BlurTarget& blur) const noexcept;

    veda::ChannelId channelId_;  ///< 이 프로세스의 채널 (frame.ch는 반드시 이 값과 같아야 함)
};