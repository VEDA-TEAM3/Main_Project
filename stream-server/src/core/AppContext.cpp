#include "core/AppContext.h"

#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "AppContext";
}  // namespace

AppContext::AppContext(const AppConfig& config) : config_(config) {}

AppContext::~AppContext() { stop(); }

bool AppContext::start() {
    if (running_) {
        return true;
    }
    running_ = true;

    loop_ = g_main_loop_new(nullptr, FALSE);

    // --- RTSP 서버 (모든 채널이 공유) ---
    server_ = gst_rtsp_server_new();
    // 포트는 문자열 프로퍼티다. AppConfig 가 [1024,65535] 로 이미 좁혔으므로 to_string 이 안전하다.
    const std::string port = std::to_string(config_.rtspServerPort);
    g_object_set(server_, "service", port.c_str(), nullptr);

    // attach 는 GMainContext 에 소스를 붙인다. 루프가 돌기 시작해야 실제로 accept 한다.
    serverSourceId_ = gst_rtsp_server_attach(server_, nullptr);
    if (serverSourceId_ == 0) {
        logError(kIface, "RTSP 서버를 포트 " + port + " 에 붙이지 못함 (포트 사용 중?)");
        stop();
        return false;
    }

    // --- 채널 조립 ---
    RtspSource::Tuning tuning;
    tuning.latencyMs = config_.rtspLatencyMs;
    tuning.tcpOnly = config_.rtspOverTcp;
    tuning.reconnectInitialSec = config_.reconnectInitialSec;
    tuning.reconnectMaxSec = config_.reconnectMaxSec;

    channels_.reserve(config_.channels.size());
    std::size_t started = 0;

    for (const ChannelConfig& channelConfig : config_.channels) {
        Channel channel;
        try {
            channel.source = std::make_unique<RtspSource>(channelConfig, tuning);
            channel.relay = std::make_unique<RtspRelaySink>(channelConfig, server_);
        } catch (const std::exception& e) {
            // 조립 시점 오류는 설정 오류다. 그 채널만 빼고 나머지는 살린다 --
            // 한 카메라의 오타 때문에 전체 시스템이 안 뜨는 편이 더 나쁘다.
            logError(kIface, "ch=" + std::to_string(channelConfig.channelId) + " 조립 실패: " + e.what() +
                                 " — 이 채널을 건너뜀");
            continue;
        }

        // 중계를 먼저 띄운다: 마운트가 준비되기 전에 도착한 패킷은 그냥 버려진다.
        channel.relay->start();

        // [핫패스 배선] 콜백은 start() 이전에 고정한다 (RtspSource 계약).
        // 캡처는 포인터 하나뿐이라 std::function 이 SSO 안에서 끝나고 힙을 쓰지 않는다.
        RtspRelaySink* relay = channel.relay.get();
        channel.source->setPacketCallback(
            [relay](const domain::EncodedPacket& packet) { relay->onPacket(packet); });

        const veda::ChannelId id = channelConfig.channelId;
        channel.source->setStateCallback([id](bool live) {
            logSuccess(kIface, "ch=" + std::to_string(id) + (live ? " 스트림 정상" : " 스트림 끊김"));
        });

        if (channel.source->start()) {
            ++started;
        }
        channels_.push_back(std::move(channel));
    }

    if (started == 0) {
        logError(kIface, "기동한 채널이 없습니다 — 설정을 확인하세요");
    }

    // --- GMainLoop: 버스 워치와 RTSP accept 가 여기서 돈다 ---
    loopThread_ = std::thread([this] { g_main_loop_run(loop_); });

    logSuccess(kIface, "stream-server 기동 (채널 " + std::to_string(started) + "/" +
                           std::to_string(config_.channels.size()) + ", RTSP 포트 " + port + ")");
    return started > 0;
}

void AppContext::stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    // 소스를 먼저 멈춘다. 루프를 먼저 죽이면 재연결 타이머와 버스 워치가 실행될 기회를
    // 잃은 채로 파이프라인만 남아, 정리 순서가 꼬인다.
    for (Channel& channel : channels_) {
        if (channel.source) {
            channel.source->stop();
        }
        if (channel.relay) {
            channel.relay->stop();
        }
    }
    channels_.clear();

    if (loop_ != nullptr) {
        g_main_loop_quit(loop_);
    }
    if (loopThread_.joinable()) {
        loopThread_.join();
    }
    if (serverSourceId_ != 0) {
        g_source_remove(serverSourceId_);
        serverSourceId_ = 0;
    }
    if (server_ != nullptr) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    if (loop_ != nullptr) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }

    logSuccess(kIface, "stream-server 정지 완료");
}
