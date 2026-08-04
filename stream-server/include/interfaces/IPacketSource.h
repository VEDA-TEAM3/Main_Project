#pragma once

/**
 * @file    IPacketSource.h
 * @brief   카메라 한 채널의 압축 패킷 공급원 (디코드하지 않음)
 */

#include <cstdint>
#include <functional>

#include "Contract.h"  // veda::ChannelId — 채널 번호 규약을 세 서버가 공유한다
#include "domain/EncodedPacket.h"

/**
 * @brief 압축 스트림 한 채널을 물어와 패킷을 흘려보내는 포트
 *
 * @details
 * 구현체(`RtspSource`)는 GStreamer 파이프라인 하나를 소유한다. 그 파이프라인 안에
 * `tee` 가 있어 **디스크 갈래는 C++ 를 전혀 거치지 않고** 곧장 muxer 로 간다
 * (`IRecorder` 참고). 이 콜백으로 올라오는 것은 **relay 갈래뿐**이다.
 *
 * 즉 이 인터페이스는 '모든 패킷의 통로'가 아니라 '네트워크 중계에 필요한 만큼만'의
 * 통로다. 녹화 경로를 C++ 로 끌어올리는 순간 zero-copy 가 깨지는 것이 아니라
 * (참조 계수라 페이로드는 그대로다) **스케줄링 위험이 생긴다** — 사용자 코드가 한 번
 * 멈추면 tee 전체가 멈추고 녹화까지 끊긴다. 그래서 일부러 올리지 않는다.
 */
class IPacketSource {
public:
    virtual ~IPacketSource() = default;

    /**
     * @brief 패킷 도착 콜백
     *
     * @warning 인자는 **빌린 참조**다. 콜백 반환 이후에도 보관하려면 복사할 것
     *          (복사 = 참조 계수 증가, 힙 할당 없음).
     * @warning GStreamer 스트리밍 스레드에서 호출된다. 여기서 블로킹하면 tee 가 막혀
     *          **녹화까지 멈춘다.** 큐에 넣고 즉시 반환할 것.
     */
    using PacketCallback = std::function<void(const domain::EncodedPacket&)>;

    /// @brief 스트림 상태 전환 통지 (재연결/카메라 사망 판정용)
    using StateCallback = std::function<void(bool /*live*/)>;

    virtual void setPacketCallback(PacketCallback callback) = 0;
    virtual void setStateCallback(StateCallback callback) = 0;

    /**
     * @brief 파이프라인 기동
     * @return 기동 성공 여부. 실패해도 예외를 던지지 않고 내부에서 재시도한다
     *         (compute-server 의 IChannelReceiver 와 같은 계약)
     */
    virtual bool start() = 0;

    /// @brief 파이프라인 정지 및 스트리밍 스레드 join
    virtual void stop() = 0;

    virtual veda::ChannelId channelId() const noexcept = 0;

    /// @brief 수신 누적 패킷 수 (관측용, 락 없음)
    virtual std::uint64_t packetCount() const noexcept = 0;
};
