#pragma once

/**
 * @file    RtspSource.h
 * @brief   채널 1개의 RTSP 수집 파이프라인 (무디코드 · 무복사)
 */

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "core/AppConfig.h"
#include "interfaces/IPacketSource.h"
#include "interfaces/IRecorder.h"

class RollingRecorder;

/**
 * @brief RTSP 압축 스트림을 물어와 디스크와 중계로 동시에 흘려보내는 소스
 *
 * @par [ 파이프라인 — 이 문자열이 무전코드 계약 그 자체다 ]
 * @code
 * rtspsrc name=src location=<uri> protocols=tcp latency=200 do-retransmission=false
 *         user-id=<u> user-pw=<p>
 *   ! rtph264depay                                  # RTP 해체만. 디코드 아님
 *   ! h264parse config-interval=-1                  # NAL 경계 파싱 + IDR마다 SPS/PPS 재삽입
 *   ! tee name=t                                    # 여기서 갈린다. 이후 두 갈래는 같은 메모리를 본다
 *
 * t. ! queue name=q_rec max-size-buffers=0 max-size-time=0 max-size-bytes=16777216
 *    ! splitmuxsink name=rec muxer-factory=matroskamux
 *                   location=<dir>/seg_%05d.mkv
 *                   max-size-time=60000000000       # 60초 세그먼트
 *
 * t. ! queue name=q_relay max-size-buffers=8 max-size-time=0 max-size-bytes=0 leaky=downstream
 *    ! appsink name=relay emit-signals=true sync=false max-buffers=8 drop=true
 * @endcode
 *
 * @par [ 디코더가 없다는 것을 어떻게 보장하는가 ]
 * 파이프라인 어디에도 `decodebin` / `avdec_*` / `v4l2h264dec` 가 없다. `depay` 와 `parse`
 * 는 **비트스트림을 건드리지 않고 경계만 읽는다.** 이걸 깨는 가장 흔한 방법이
 * `decodebin` 이나 `autovideosink` 를 임시로 끼워 보는 것인데, 한 번 끼우면 RPi 4채널
 * 예산이 그 자리에서 무너진다. 디버깅은 `GST_DEBUG=3` 과 `fakesink dump=false` 로 할 것.
 *
 * @par [ 갈래별 큐 정책이 비대칭인 이유 — 의도된 것이다 ]
 * - `q_rec` 는 **leaky 가 아니다.** 녹화가 제품이다. 버리면 증거가 사라진다.
 * - `q_relay` 는 `leaky=downstream` 이다. 라이브 화면은 최신 프레임만 쓸모 있고, 밀린
 *   프레임을 붙잡고 있어 봐야 지연만 쌓인다 (compute-server MqttFrameSink 와 같은 판단).
 *
 * @warning `tee` 의 한 갈래가 막히면 **모든 갈래가 막힌다.** SD 카드처럼 느린 저장매체에서는
 *          `q_rec` 가 차면서 수집 자체가 정지하고 중계까지 죽는다. `max-size-bytes` 를
 *          16 MB 로 잡은 것은 1080p 4Mbps 기준 약 30초치 완충이며, **USB SSD 사용을 전제로
 *          한 값**이다. SD 카드로 갈 거라면 `q_rec` 도 leaky 로 바꾸고 녹화 유실을 받아들이거나,
 *          비트레이트를 낮춰야 한다. 조용히 넘어갈 문제가 아니다.
 *
 * @note [ mp4 가 아니라 mkv 인 이유 ]
 * `splitmuxsink` 는 세그먼트를 닫을 때 정상 마무리하지만, **정전으로 죽으면 진행 중이던
 * 세그먼트 하나가 통째로 날아간다** — mp4 는 moov atom 이 파일 끝에 쓰이기 때문이다.
 * matroska 는 앞에서부터 재생 가능해 꼬리만 잃는다. RPi 는 전원이 끊기는 물건이므로
 * mkv 가 맞다. 배포처가 mp4 를 요구하면 세그먼트가 닫힌 뒤 오프라인 remux 할 것
 * (remux 는 재인코딩이 아니라 값싸다).
 *
 * @note [ H.265 채널 ]
 * `rtph265depay ! h265parse` 로 바꾸면 된다. 중계 쪽 페이로더도 `rtph265pay` 여야 한다.
 * 코덱은 채널마다 다를 수 있으므로 AppConfig 의 채널 항목에서 고른다.
 *
 * @warning [ 카메라 세션 한도 ]
 * compute-server 가 이미 채널마다 ONVIF 메타데이터용 RTSP 세션을 하나 열고 있다.
 * stream-server 를 붙이면 카메라당 세션이 2개가 된다 (4채널이면 8개). 동시 세션 한도가
 * 낮은 카메라에서는 나중에 붙는 쪽이 조용히 거부당한다 — 배포 전에 카메라 사양을 확인할 것.
 */
class RtspSource final : public IPacketSource {
public:
    /// @brief 수집 튜닝 (AppConfig 전역값에서 옮겨 담는다)
    struct Tuning {
        std::uint32_t latencyMs;             ///< rtspsrc 지터버퍼
        bool tcpOnly;                        ///< UDP 손실은 녹화를 조용히 깨뜨린다
        std::uint32_t reconnectInitialSec;
        std::uint32_t reconnectMaxSec;

        // GCC 파싱 에러 방지용 기본 생성자 명시
        Tuning() : latencyMs(100), tcpOnly(true), reconnectInitialSec(1), reconnectMaxSec(30) {}
    };

    /**
     * @param channel 채널 설정 (URI, 자격증명, 코덱, 세그먼트 디렉터리)
     * @param tuning  지연/재연결 파라미터
     * @throws std::invalid_argument ChannelConfig::valid() 가 false 인 경우
     *         (조용히 틀린 파이프라인을 세우느니 기동 시점에 죽는 게 낫다)
     */
    explicit RtspSource(const ChannelConfig& channel, const Tuning& tuning = Tuning());
    ~RtspSource() override;

    RtspSource(const RtspSource&) = delete;
    RtspSource& operator=(const RtspSource&) = delete;

    void setPacketCallback(PacketCallback callback) override;
    void setStateCallback(StateCallback callback) override;

    bool start() override;
    void stop() override;

    veda::ChannelId channelId() const noexcept override { return channel_.channelId; }
    std::uint64_t packetCount() const noexcept override { return packetCount_.load(std::memory_order_relaxed); }

    /// @brief 녹화 갈래 제어 핸들 (splitmuxsink 를 감싼다). 소스가 소유한다
    IRecorder* recorder() noexcept;

private:
    /**
     * @brief 정적 토폴로지만 담은 launch 문자열을 조립한다
     * @warning URI/자격증명/파일경로는 **여기 들어가지 않는다.** start() 에서 g_object_set
     *          으로 주입해 파이프라인 인젝션을 구조적으로 불가능하게 만든다 (.cpp 주석 참고)
     */
    std::string buildLaunchString() const;

    /// @brief appsink 콜백 (GStreamer 스트리밍 스레드). 시그널이 아니라 함수 포인터 직결
    static GstFlowReturn onNewSample(GstAppSink* appsink, gpointer userData) noexcept;

    /// @brief 버스 워치 — ERROR/EOS 를 재연결 판단으로 올린다 (GMainLoop 스레드)
    static gboolean onBusMessage(GstBus* bus, GstMessage* message, gpointer userData) noexcept;

    void scheduleReconnect() noexcept;
    void doReconnect() noexcept;

    ChannelConfig channel_;
    std::uint32_t latencyMs_;
    bool tcpOnly_;
    std::uint32_t reconnectInitialSec_;
    std::uint32_t reconnectMaxSec_;

    GstElement* pipeline_ = nullptr;  ///< 소유 (gst_object_unref)
    GstElement* appsink_ = nullptr;   ///< gst_bin_get_by_name 이 준 참조를 보관 — stop() 에서 unref
    guint busWatchId_ = 0;
    guint reconnectTimerId_ = 0;

    std::unique_ptr<RollingRecorder> recorder_;  ///< splitmuxsink 제어면 (데이터 경로 없음)

    /**
     * @name 콜백 — start() 이전에 고정된다
     * @details 핫패스에서 뮤텍스도 std::function 복사도 하지 않기 위한 계약이다.
     *          std::function 복사는 캡처가 크면 힙 할당이고, 그러면 패킷당 무할당이 깨진다.
     *          start() 이후의 set*Callback 은 무시하고 에러 로그를 남긴다.
     * @{
     */
    PacketCallback packetCallback_;
    StateCallback stateCallback_;
    /** @} */

    std::atomic_bool running_{false};
    std::atomic_bool productive_{false};  ///< 첫 패킷 도착 여부 — 백오프 초기화 조건
    std::atomic<guint> backoffSec_{1};
    std::atomic_uint64_t packetCount_{0};
};
