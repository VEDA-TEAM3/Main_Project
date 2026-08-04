#include "sink/RtspRelaySink.h"

#include <stdexcept>

#include "Logger.h"

namespace {
constexpr const char* kIface = "RtspRelaySink";

/// @brief appsrc 내부 상한. 넘으면 push 가 실패를 돌려주고 우리는 그 패킷을 버린다
constexpr guint64 kAppSrcMaxBytes = 2ULL * 1024ULL * 1024ULL;
}  // namespace

RtspRelaySink::RtspRelaySink(const ChannelConfig& channel, GstRTSPServer* server)
    : channel_(channel), server_(server) {
    if (server_ == nullptr) {
        throw std::invalid_argument("RtspRelaySink: server must not be null");
    }
    // mountPoint 는 gst_rtsp_mount_points_add_factory 에 그대로 들어가는 경로다.
    // AppConfig 가 이미 걸렀지만 DI 경로까지 막는다.
    if (!isSafeMountPoint(channel_.mountPoint)) {
        throw std::invalid_argument("RtspRelaySink: unsafe mountPoint");
    }
}

RtspRelaySink::~RtspRelaySink() { stop(); }

std::string RtspRelaySink::buildFactoryLaunch() const {
    // [보안] 이 문자열에도 외부 값은 들어가지 않는다. codec 은 {"h264","h265"} 로 이미 좁다.
    //
    // pay0 이라는 이름은 gst-rtsp-server 의 규약이다. 다른 이름이면 SDP 가 만들어지지
    // 않아 클라이언트가 붙지 못하는데, 에러 메시지가 그 원인을 알려주지 않는다.
    //
    // do-timestamp=true: 카메라 PTS 대신 도착 시각으로 타임스탬프를 다시 찍는다.
    //   장점 - 카메라 클럭 스큐/점프가 중계 타임라인을 흔들지 않는다 (라이브 뷰에 유리).
    //   대가 - 중계 타임라인과 녹화 파일의 타임라인이 갈라진다. 두 소스를 프레임 단위로
    //          대조해야 하는 용도라면 이 값을 false 로 두고 카메라 PTS 를 살려야 한다.
    const bool h265 = channel_.codec == "h265";
    std::string launch;
    launch.reserve(384);
    launch += "( appsrc name=vsrc is-live=true format=time do-timestamp=true stream-type=0 block=false ";
    launch += "! ";
    launch += h265 ? "h265parse" : "h264parse";
    launch += " config-interval=-1 ";
    launch += "! ";
    launch += h265 ? "rtph265pay" : "rtph264pay";
    launch += " name=pay0 pt=96 config-interval=-1 )";
    return launch;
}

bool RtspRelaySink::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    factory_ = gst_rtsp_media_factory_new();
    const std::string launch = buildFactoryLaunch();
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());

    // shared=TRUE: 클라이언트가 여러 개여도 파이프라인 하나를 공유한다.
    // FALSE 면 클라이언트마다 appsrc 가 새로 생기고, 우리는 그중 하나에만 push 하게 되어
    // 두 번째 클라이언트가 영원히 검은 화면을 본다.
    gst_rtsp_media_factory_set_shared(factory_, TRUE);

    g_signal_connect(factory_, "media-configure", G_CALLBACK(&RtspRelaySink::onMediaConfigure), this);

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
    // add_factory 는 factory_ 의 소유권을 가져간다 -- 우리가 unref 하면 안 된다.
    gst_rtsp_mount_points_add_factory(mounts, channel_.mountPoint.c_str(), factory_);
    g_object_unref(mounts);

    logSuccess(kIface, "ch=" + std::to_string(channel_.channelId) + " 중계 마운트 " + channel_.mountPoint);
    return true;
}

void RtspRelaySink::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (server_ != nullptr) {
        GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
        gst_rtsp_mount_points_remove_factory(mounts, channel_.mountPoint.c_str());
        g_object_unref(mounts);
    }
    factory_ = nullptr;  // mounts 가 소유했으므로 여기서 unref 하지 않는다

    std::lock_guard<std::mutex> lock(appsrcMutex_);
    if (appsrc_ != nullptr) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
}

void RtspRelaySink::onPacket(const domain::EncodedPacket& packet) {
    if (!packet) {
        return;
    }

    std::lock_guard<std::mutex> lock(appsrcMutex_);
    if (appsrc_ == nullptr) {
        // 붙은 클라이언트가 없으면 미디어가 아직 구성되지 않았다. 정상 상태다 --
        // 녹화는 tee 반대편에서 계속 돌고 있다.
        droppedCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // [참조 계수 계약 — 여기가 누수/이중해제가 나는 자리다]
    // gst_app_src_push_sample() 은 **소유권을 가져가지 않는다.** 내부에서 버퍼를
    // gst_buffer_ref() 할 뿐이다. 따라서:
    //   - 우리는 packet 의 참조를 계속 들고 있고, 소멸자가 정상적으로 unref 한다
    //   - 여기서 packet.release() 를 쓰면 참조가 하나 새어 나간다 (누수)
    //   - 여기서 추가로 unref 하면 이중 해제로 즉시 죽는다
    // push_buffer() 계열은 반대로 소유권을 가져가므로 섞어 쓰면 안 된다.
    const GstFlowReturn result = gst_app_src_push_sample(GST_APP_SRC(appsrc_), packet.get());
    if (result != GST_FLOW_OK) {
        const std::uint64_t dropped = droppedCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        // 밀리는 상황은 프레임마다 연속으로 발생한다 -- 첫 건과 100건마다만 기록하고,
        // 문자열 조립도 그때만 한다 (억제될 로그의 힙 할당 제거).
        if ((dropped == 1 || dropped % 100 == 0) && isLogEnabled(LogLevel::Error)) {
            logError(kIface, "ch=" + std::to_string(channel_.channelId) + " appsrc push 실패 (누적 " +
                                 std::to_string(dropped) + "건) — 중계가 밀리고 있음");
        }
    }
}

void RtspRelaySink::onMediaConfigure(GstRTSPMediaFactory* /*factory*/, GstRTSPMedia* media,
                                     gpointer userData) noexcept {
    auto* self = static_cast<RtspRelaySink*>(userData);

    GstElement* element = gst_rtsp_media_get_element(media);  // 새 참조
    if (element == nullptr) {
        return;
    }
    // recurse_up: appsrc 가 팩토리가 만든 bin 안쪽에 들어 있다
    GstElement* appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(element), "vsrc");
    gst_object_unref(element);
    if (appsrc == nullptr) {
        logError(kIface, "미디어에서 appsrc(vsrc)를 찾을 수 없음");
        return;
    }

    g_object_set(appsrc, "max-bytes", kAppSrcMaxBytes, nullptr);

    {
        std::lock_guard<std::mutex> lock(self->appsrcMutex_);
        if (self->appsrc_ != nullptr) {
            gst_object_unref(self->appsrc_);
        }
        self->appsrc_ = appsrc;  // 참조를 그대로 보관 (unprepared 에서 반납)
    }

    // 마지막 클라이언트가 나가면 미디어가 unprepare 된다. 그때 포인터를 놓지 않으면
    // 다음 onPacket 이 해제된 엘리먼트에 push 한다 (use-after-free).
    g_signal_connect(media, "unprepared", G_CALLBACK(&RtspRelaySink::onMediaUnprepared), self);

    logSuccess(kIface, "ch=" + std::to_string(self->channel_.channelId) + " 클라이언트 접속 — 중계 시작");
}

void RtspRelaySink::onMediaUnprepared(GstRTSPMedia* /*media*/, gpointer userData) noexcept {
    auto* self = static_cast<RtspRelaySink*>(userData);

    std::lock_guard<std::mutex> lock(self->appsrcMutex_);
    if (self->appsrc_ != nullptr) {
        gst_object_unref(self->appsrc_);
        self->appsrc_ = nullptr;
    }
    logSuccess(kIface, "ch=" + std::to_string(self->channel_.channelId) + " 클라이언트 종료 — 중계 대기");
}
