#pragma once

/**
 * @file    RtspRelaySink.h
 * @brief   수집한 압축 패킷을 Qt 클라이언트에게 RTSP 로 재발행 (무전코드)
 */

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <atomic>
#include <mutex>
#include <string>

#include "core/AppConfig.h"
#include "interfaces/IPacketSink.h"

/**
 * @brief gst-rtsp-server 의 appsrc 에 패킷을 그대로 밀어 넣는 중계 싱크
 *
 * @par [ 미디어 팩토리 파이프라인 ]
 * @code
 * appsrc name=vsrc is-live=true format=time do-timestamp=false
 *        stream-type=0 max-bytes=2097152 block=false
 *   ! h264parse config-interval=-1
 *   ! rtph264pay name=pay0 pt=96 config-interval=-1
 * @endcode
 *
 * `pay0` 이라는 이름은 **gst-rtsp-server 의 규약**이다. 다른 이름을 쓰면 SDP 가 만들어지지
 * 않아 클라이언트가 붙지 못한다 (에러 메시지가 친절하지 않으니 기억해 둘 것).
 *
 * `config-interval=-1` 을 페이로더에도 준 이유: Qt 가 중간에 접속해도 첫 IDR 앞에
 * SPS/PPS 가 실려 나가야 첫 화면이 뜬다. 없으면 다음 IDR 까지 검은 화면이 유지된다.
 *
 * @par [ 복사가 없다는 근거 ]
 * `onPacket` 은 `EncodedPacket` 을 복사한다 — 참조 계수 +1 뿐이다. 그 다음
 * `gst_app_src_push_sample()` 로 넘기는데, appsrc 는 샘플의 버퍼를 **참조로** 취한다.
 * 카메라 소켓에서 읽힌 압축 페이로드는 이 과정에서 한 번도 memcpy 되지 않는다.
 *
 * @warning [ '절대 zero-copy' 의 정확한 의미 ]
 * 애플리케이션 레벨 복사가 0 이라는 뜻이지, 물리적으로 0 이라는 뜻이 아니다. 커널
 * 경계에서는 여전히 복사가 있다: 소켓 recv → GstBuffer, GstBuffer → write()/send().
 * 그것까지 없애려면 io_uring/splice 급 작업이 필요하고, 이 규모에서 얻을 것이 없다.
 * 우리가 없앤 것은 **디코드(수백 MB/s 급 raw 프레임)와 프로세스 내 중복 버퍼**이며,
 * 실제 CPU 예산을 잡아먹던 것은 그쪽이다.
 *
 * @note [ 클라이언트가 없을 때 ]
 * 붙은 클라이언트가 0이면 미디어 팩토리가 아직 파이프라인을 만들지 않았을 수 있다.
 * 그때 push 한 패킷은 조용히 버려진다 (`droppedCount_` 에 잡힌다). 이건 정상이며,
 * **녹화는 클라이언트 유무와 무관하게 계속된다** — 그게 tee 를 쓴 이유다.
 */
class RtspRelaySink final : public IPacketSink {
public:
    /**
     * @param channel   채널 설정 (mountPoint, 코덱)
     * @param server    채널들이 공유하는 GstRTSPServer (AppContext 가 소유)
     * @throws std::invalid_argument server 가 null 이거나 mountPoint 가 비어 있을 때
     */
    RtspRelaySink(const ChannelConfig& channel, GstRTSPServer* server);
    ~RtspRelaySink() override;

    RtspRelaySink(const RtspRelaySink&) = delete;
    RtspRelaySink& operator=(const RtspRelaySink&) = delete;

    bool start() override;
    void stop() override;

    /**
     * @details 큐가 아니라 appsrc 에 직접 push 한다. appsrc 가 `max-bytes` 를 넘기면
     *          `block=false` 라 즉시 실패를 돌려주고, 우리는 그 패킷을 버린다
     *          (`droppedCount_` 증가). **절대 블로킹하지 않는다** — 여기서 막히면
     *          tee 를 통해 녹화까지 멈춘다.
     */
    void onPacket(const domain::EncodedPacket& packet) override;

    std::uint64_t droppedCount() const noexcept override { return droppedCount_.load(std::memory_order_relaxed); }
    veda::ChannelId channelId() const noexcept override { return channel_.channelId; }

    /// @brief 클라이언트가 접속할 경로 (예: "rtsp://<rpi>:8554/ch0")
    std::string mountPoint() const { return channel_.mountPoint; }

private:
    /// @brief 클라이언트 접속 시 팩토리가 미디어를 구성할 때 호출 — appsrc 를 붙잡아 둔다
    static void onMediaConfigure(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer userData) noexcept;

    /// @brief 마지막 클라이언트가 나가면 appsrc 참조를 놓는다 (dangling 방지)
    static void onMediaUnprepared(GstRTSPMedia* media, gpointer userData) noexcept;

    std::string buildFactoryLaunch() const;

    ChannelConfig channel_;
    GstRTSPServer* server_ = nullptr;          ///< 빌린 포인터 — 소유하지 않는다
    GstRTSPMediaFactory* factory_ = nullptr;   ///< mounts 가 소유

    /// @brief 활성 appsrc. 미디어가 구성될 때 set, unprepare 될 때 clear
    /// @note  스트리밍 스레드(onPacket)와 RTSP 서버 스레드(configure/unprepare)가
    ///        함께 만지므로 뮤텍스가 필요하다. 원자 포인터로는 부족하다 --
    ///        push 중에 unref 되는 창을 막아야 하기 때문
    mutable std::mutex appsrcMutex_;
    GstElement* appsrc_ = nullptr;

    std::atomic_bool running_{false};
    std::atomic_uint64_t droppedCount_{0};
};
