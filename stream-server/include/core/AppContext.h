#pragma once

/**
 * @file    AppContext.h
 * @brief   의존성 조립 + GMainLoop 수명주기
 */

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <memory>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "sink/RtspRelaySink.h"
#include "source/RtspSource.h"

/**
 * @brief 채널별 소스/중계를 조립하고 GMainLoop 을 배경 스레드에서 돌린다
 *
 * @note [ 기동 순서가 중요하다 ]
 * 1. GstRTSPServer 를 먼저 attach 한다 (마운트가 준비되기 전에 패킷이 오면 그냥 버려진다)
 * 2. RelaySink 를 start (팩토리 등록)
 * 3. RtspSource 를 start (여기서부터 패킷이 흐른다)
 * 4. GMainLoop 시작 -- 버스 워치와 RTSP 서버가 이 루프 위에서 돈다
 *
 * GMainLoop 이 없으면 `gst_bus_add_watch` 와 `gst_rtsp_server_attach` 가 아무것도 하지
 * 않는다. 파이프라인은 PLAYING 인데 오류 처리도 클라이언트 접속도 안 되는,
 * 진단하기 고약한 상태가 된다.
 */
class AppContext {
public:
    explicit AppContext(const AppConfig& config);
    ~AppContext();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    /// @return 채널이 하나라도 기동했으면 true
    bool start();

    /// @brief GMainLoop 정지 → 소스/중계 정지 → 스레드 join (멱등)
    void stop();

private:
    struct Channel {
        std::unique_ptr<RtspSource> source;
        std::unique_ptr<RtspRelaySink> relay;
    };

    AppConfig config_;

    GstRTSPServer* server_ = nullptr;  ///< 소유 (g_object_unref)
    guint serverSourceId_ = 0;

    GMainLoop* loop_ = nullptr;  ///< 소유 (g_main_loop_unref)
    std::thread loopThread_;

    std::vector<Channel> channels_;
    bool running_ = false;
};
