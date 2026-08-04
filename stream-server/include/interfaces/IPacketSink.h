#pragma once

/**
 * @file    IPacketSink.h
 * @brief   압축 패킷 소비 포트 (네트워크 중계 경로 전용)
 */

#include <cstdint>

#include "Contract.h"
#include "domain/EncodedPacket.h"

/**
 * @brief 압축 패킷을 받아 밖으로 내보내는 포트
 *
 * @details
 * 구현체는 `RtspRelaySink` 하나다 — 받은 GstSample 을 gst-rtsp-server 의 `appsrc` 에
 * 그대로 push 한다. push 는 소유권 이전이므로 페이로드 복사가 없다.
 *
 * @note [ 왜 저장(storage)이 이 인터페이스를 구현하지 않는가 ]
 * 녹화는 GStreamer 파이프라인 **안에서** `tee` → `splitmuxsink` 로 끝난다. 패킷이
 * C++ 로 올라올 이유가 없고, 올리면 사용자 스레드 하나가 녹화 전체의 단일 장애점이 된다.
 * 저장 계층의 제어 표면은 `IRecorder` 이고 데이터 경로는 없다.
 */
class IPacketSink {
public:
    virtual ~IPacketSink() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    /**
     * @brief 패킷 하나를 중계한다
     *
     * @details 구현체는 `packet` 을 **복사해서** 내부 큐/appsrc 로 넘긴다 (참조 계수 증가).
     *          호출자가 준 참조는 반환 즉시 무효가 될 수 있다.
     * @warning 절대 블로킹하지 말 것 — 호출자는 GStreamer 스트리밍 스레드다.
     *          소비자가 느리면 **막지 말고 버린다** (라이브 중계는 최신이 항상 유용하므로
     *          drop-oldest 가 맞다. compute-server 의 MqttFrameSink 와 같은 판단).
     */
    virtual void onPacket(const domain::EncodedPacket& packet) = 0;

    /// @brief 큐 포화로 버린 누적 패킷 수 (0 이 아니면 중계가 밀리고 있다는 뜻)
    virtual std::uint64_t droppedCount() const noexcept = 0;

    virtual veda::ChannelId channelId() const noexcept = 0;
};
