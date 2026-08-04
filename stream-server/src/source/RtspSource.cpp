#include "source/RtspSource.h"

#include <gst/app/gstappsink.h>

#include <stdexcept>
#include <string>

#include "Logger.h"
#include "storage/RollingRecorder.h"

namespace {

constexpr const char* kIface = "RtspSource";

/// @brief 초저지연 설정. appsink 는 최신 1장만 들고 나머지는 버린다
constexpr int kAppSinkMaxBuffers = 1;

/// @brief 녹화 갈래 완충 (USB SSD 전제, 1080p 4Mbps 기준 약 30초)
constexpr const char* kRecordQueueBytes = "16777216";

}  // namespace

RtspSource::RtspSource(const ChannelConfig& channel, const Tuning& tuning)
    : channel_(channel),
      latencyMs_(tuning.latencyMs),
      tcpOnly_(tuning.tcpOnly),
      reconnectInitialSec_(tuning.reconnectInitialSec),
      reconnectMaxSec_(tuning.reconnectMaxSec) {
    backoffSec_.store(reconnectInitialSec_, std::memory_order_relaxed);
    // [조립 시점 fail-fast] 설정 검증은 AppConfig 가 이미 했지만, DI 로 직접 넣는 경로까지
    // 막아야 한다. 잘못된 URI 로 조용히 기동하면 '카메라가 안 나온다'는 증상만 남는다.
    if (!channel_.valid()) {
        throw std::invalid_argument("RtspSource: invalid ChannelConfig (uri/mount/dir/codec)");
    }
}

RtspSource::~RtspSource() { stop(); }

std::string RtspSource::buildLaunchString() const {
    // [보안 — 이 함수의 핵심]
    // URI/사용자/비밀번호/파일경로는 **이 문자열에 들어가지 않는다.** 전부 start() 에서
    // g_object_set 으로 주입한다. gst_parse_launch 는 문자열을 '파싱'해 그래프를 만들기
    // 때문에, 값에 공백이나 '!' 가 하나만 있어도 임의 엘리먼트를 주입할 수 있다.
    // 값을 이스케이프하는 방식은 파서 규칙을 우리가 재구현하는 셈이라 언젠가 어긋난다.
    // 아예 넣지 않으면 그 취약점이 '완화'되는 게 아니라 **존재할 수 없게** 된다.
    //
    // 이 문자열에 들어가는 가변 요소는 codec 하나뿐이며, 그것도 {"h264","h265"} 로
    // 이미 좁혀져 있다 (ChannelConfig::valid).
    const bool h265 = channel_.codec == "h265";
    const char* depay = h265 ? "rtph265depay" : "rtph264depay";
    const char* parse = h265 ? "h265parse" : "h264parse";

    std::string launch;
    launch.reserve(768);
    launch += "rtspsrc name=src do-retransmission=false ";
    launch += "! ";
    launch += depay;
    launch += " ! ";
    launch += parse;
    launch += " config-interval=-1 ";
    launch += "! tee name=t ";

    // 녹화 갈래: leaky 아님. 녹화가 제품이고, 버리면 증거가 사라진다.
    launch += "t. ! queue name=q_rec max-size-buffers=0 max-size-time=0 max-size-bytes=";
    launch += kRecordQueueBytes;
    launch += " ! splitmuxsink name=rec muxer-factory=matroskamux ";

    // 중계 갈래: leaky. 라이브 화면은 최신 프레임만 쓸모 있다.
    launch += "t. ! queue name=q_relay max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ";
    launch += "! appsink name=relay sync=false max-buffers=1 drop=true";
    return launch;
}

bool RtspSource::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;  // 이미 기동됨 (멱등)
    }

    GError* error = nullptr;
    const std::string launch = buildLaunchString();
    pipeline_ = gst_parse_launch(launch.c_str(), &error);
    if (pipeline_ == nullptr || error != nullptr) {
        logError(kIface, "ch=" + std::to_string(channel_.channelId) + " 파이프라인 생성 실패: " +
                             std::string(error != nullptr ? error->message : "unknown"));
        if (error != nullptr) {
            g_error_free(error);
        }
        running_.store(false, std::memory_order_release);
        return false;
    }

    // --- 값 주입: 파싱을 거치지 않으므로 인젝션이 성립하지 않는다 ---
    GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    if (src == nullptr) {
        logError(kIface, "rtspsrc 를 찾을 수 없음 (launch 문자열 손상)");
        stop();
        return false;
    }
    g_object_set(src, "location", channel_.rtspUri.c_str(), nullptr);
    g_object_set(src, "latency", static_cast<guint>(latencyMs_), nullptr);
    // protocols: 4 = GST_RTSP_LOWER_TRANS_TCP. UDP 손실은 녹화 파일을 조용히 깨뜨린다.
    g_object_set(src, "protocols", tcpOnly_ ? 0x4 : 0x7, nullptr);
    if (!channel_.username.empty()) {
        g_object_set(src, "user-id", channel_.username.c_str(), nullptr);
        g_object_set(src, "user-pw", channel_.password.c_str(), nullptr);
    }
    gst_object_unref(src);  // gst_bin_get_by_name 은 새 참조를 준다

    // --- 녹화 제어기: splitmuxsink 를 감싸되 데이터는 지나가지 않는다 ---
    GstElement* splitmux = gst_bin_get_by_name(GST_BIN(pipeline_), "rec");
    if (splitmux == nullptr) {
        logError(kIface, "splitmuxsink 를 찾을 수 없음");
        stop();
        return false;
    }
    const guint64 segmentNs = static_cast<guint64>(channel_.segmentSeconds) * GST_SECOND;
    g_object_set(splitmux, "max-size-time", segmentNs, nullptr);
    // async-finalize: 세그먼트 마감(muxer 교체)이 스트리밍 스레드를 붙잡지 않게 한다.
    // 이게 없으면 60초마다 짧은 지연 스파이크가 중계 쪽에도 그대로 나타난다.
    g_object_set(splitmux, "async-finalize", TRUE, nullptr);
    recorder_ = std::make_unique<RollingRecorder>(channel_, splitmux);
    recorder_->scanExistingSegments();
    gst_object_unref(splitmux);

    // --- appsink: 시그널이 아니라 콜백으로 받는다 ---
    // emit-signals=true + g_signal_connect 는 패킷마다 GSignal 마샬링을 태운다.
    // set_callbacks 는 함수 포인터 직접 호출이라 그 비용이 사라진다 (지연/CPU 양쪽 이득).
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "relay");
    if (appsink_ == nullptr) {
        logError(kIface, "appsink 를 찾을 수 없음");
        stop();
        return false;
    }
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &RtspSource::onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &callbacks, this, nullptr);
    g_object_set(appsink_, "max-buffers", static_cast<guint>(kAppSinkMaxBuffers), "drop", TRUE, "sync", FALSE,
                 nullptr);

    GstBus* bus = gst_element_get_bus(pipeline_);
    busWatchId_ = gst_bus_add_watch(bus, &RtspSource::onBusMessage, this);
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        logError(kIface, "ch=" + std::to_string(channel_.channelId) + " PLAYING 전환 실패 — 백오프 재시도");
        scheduleReconnect();
        return false;
    }

    logSuccess(kIface, "ch=" + std::to_string(channel_.channelId) + " 수집 시작 (codec=" + channel_.codec +
                           ", tcp=" + (tcpOnly_ ? "yes" : "no") + ")");
    return true;
}

void RtspSource::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (reconnectTimerId_ != 0) {
        g_source_remove(reconnectTimerId_);
        reconnectTimerId_ = 0;
    }
    if (busWatchId_ != 0) {
        g_source_remove(busWatchId_);
        busWatchId_ = 0;
    }

    if (pipeline_ != nullptr) {
        // NULL 전환이 스트리밍 스레드를 join 한다 -- 이 시점 이후로 onNewSample 은 호출되지 않는다.
        // 콜백 해제보다 먼저 해야 use-after-free 창이 생기지 않는다.
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsink_ != nullptr) {
        gst_object_unref(appsink_);  // gst_bin_get_by_name 이 준 참조 반납
        appsink_ = nullptr;
    }
    recorder_.reset();
    if (pipeline_ != nullptr) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

void RtspSource::setPacketCallback(PacketCallback callback) {
    // [계약] start() 이전에만 설정한다. 그래야 핫패스에서 락도 std::function 복사도 없다.
    // (std::function 복사는 캡처가 크면 힙 할당이다 -- 패킷마다 그러면 무할당이 깨진다)
    if (running_.load(std::memory_order_acquire)) {
        logError(kIface, "setPacketCallback 은 start() 이전에만 호출해야 함 — 무시");
        return;
    }
    packetCallback_ = std::move(callback);
}

void RtspSource::setStateCallback(StateCallback callback) {
    if (running_.load(std::memory_order_acquire)) {
        logError(kIface, "setStateCallback 은 start() 이전에만 호출해야 함 — 무시");
        return;
    }
    stateCallback_ = std::move(callback);
}

GstFlowReturn RtspSource::onNewSample(GstAppSink* appsink, gpointer userData) noexcept {
    auto* self = static_cast<RtspSource*>(userData);

    // pull_sample 은 소유권을 넘겨준다 (transfer full).
    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (sample == nullptr) {
        return GST_FLOW_OK;  // EOS 는 버스에서 처리한다
    }

    // [무할당 지점] 스택 위 8바이트 핸들. new/malloc/make_shared 없음.
    // 소멸자가 gst_sample_unref 를 부르므로 아래 어느 경로로 빠져나가도 누수가 없다.
    const domain::EncodedPacket packet(sample);

    self->packetCount_.fetch_add(1, std::memory_order_relaxed);

    // 첫 패킷이 실제로 도착해야 '생산적인 세션'이다. 연결/SETUP 성공만으로 백오프를
    // 초기화하면, 잘못된 URI 가 1초 간격 재연결 폭풍을 만든다 (compute-server 와 같은 교훈).
    if (!self->productive_.exchange(true, std::memory_order_acq_rel)) {
        self->backoffSec_.store(self->reconnectInitialSec_, std::memory_order_relaxed);
        if (self->stateCallback_) {
            self->stateCallback_(true);
        }
    }

    // 콜백은 start() 이전에 고정되었으므로 락 없이 읽는다.
    if (self->packetCallback_) {
        self->packetCallback_(packet);  // 빌린 참조 -- 보관하려면 수신측이 복사한다
    }
    return GST_FLOW_OK;
}

gboolean RtspSource::onBusMessage(GstBus* /*bus*/, GstMessage* message, gpointer userData) noexcept {
    auto* self = static_cast<RtspSource*>(userData);

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            logError(kIface, "ch=" + std::to_string(self->channel_.channelId) + " 파이프라인 오류: " +
                                 std::string(error != nullptr ? error->message : "unknown"));
            if (error != nullptr) {
                g_error_free(error);
            }
            g_free(debug);
            self->scheduleReconnect();
            break;
        }
        case GST_MESSAGE_EOS:
            logError(kIface, "ch=" + std::to_string(self->channel_.channelId) + " EOS — 카메라 연결 종료");
            self->scheduleReconnect();
            break;
        default:
            break;
    }
    return TRUE;  // 워치 유지
}

void RtspSource::scheduleReconnect() noexcept {
    if (!running_.load(std::memory_order_acquire) || reconnectTimerId_ != 0) {
        return;
    }

    if (productive_.exchange(false, std::memory_order_acq_rel) && stateCallback_) {
        stateCallback_(false);
    }

    const guint delay = backoffSec_.load(std::memory_order_relaxed);
    logError(kIface, "ch=" + std::to_string(channel_.channelId) + " " + std::to_string(delay) + "초 후 재연결");

    reconnectTimerId_ = g_timeout_add_seconds(
        delay,
        [](gpointer data) -> gboolean {
            auto* self = static_cast<RtspSource*>(data);
            self->reconnectTimerId_ = 0;
            self->doReconnect();
            return G_SOURCE_REMOVE;
        },
        this);

    // 지수 백오프 (상한 고정). 생산적 세션이 확인되면 onNewSample 이 초기값으로 되돌린다.
    const guint next = std::min<guint>(delay * 2U, reconnectMaxSec_);
    backoffSec_.store(next, std::memory_order_relaxed);
}

void RtspSource::doReconnect() noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    // running_ 을 내렸다 올리는 대신 파이프라인만 갈아끼운다.
    running_.store(false, std::memory_order_release);
    stop();
    start();
}

IRecorder* RtspSource::recorder() noexcept { return recorder_.get(); }
