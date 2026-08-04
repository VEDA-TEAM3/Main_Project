#pragma once

/**
 * @file    RollingRecorder.h
 * @brief   splitmuxsink 롤링 DVR 의 제어면 (데이터 경로 없음)
 */

#include <gst/gst.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "core/AppConfig.h"
#include "interfaces/IRecorder.h"

/**
 * @brief 세그먼트 회전과 보존 정책만 담당하는 녹화 제어기
 *
 * @details
 * **패킷은 이 클래스를 지나가지 않는다.** `RtspSource` 의 파이프라인 안에서
 * `tee → queue → splitmuxsink` 로 이미 디스크까지 간다. 이 클래스가 하는 일은:
 *  1. `splitmuxsink` 의 `format-location` 시그널로 세그먼트 파일 이름을 정하고
 *  2. 세그먼트가 닫힐 때마다 보존 정책을 적용해 오래된 파일을 지우는 것
 * 두 가지뿐이다.
 *
 * @warning [ 보존 정책은 선택이 아니다 ]
 * 디스크가 가득 차면 `splitmuxsink` 가 쓰기에 실패하고 파이프라인이 ERROR/EOS 로 죽는다.
 * `tee` 로 묶여 있으므로 **중계까지 함께 죽는다.** 녹화만 멈추는 게 아니다.
 * `enforceRetention()` 이 돌지 않으면 SD 카드 크기에 따라 몇 시간 만에 그 상태가 된다.
 *
 * @note [ 왜 max-files 가 아니라 직접 지우는가 ]
 * `splitmuxsink` 의 `max-files` 는 개수만 본다. 비트레이트가 변하는 카메라(가변 장면
 * 복잡도)에서는 같은 개수라도 디스크 사용량이 몇 배까지 흔들린다. 용량 상한과 개수 상한을
 * 함께 걸고 **먼저 걸리는 쪽**으로 지워야 예측 가능해진다.
 *
 * @note 스레드: `format-location` 과 세그먼트 종료 콜백은 GStreamer 스트리밍 스레드에서
 *       온다. 파일 삭제(`enforceRetention`)는 blocking I/O 이므로 **그 스레드에서 직접
 *       하면 안 된다** — tee 가 막힌다. 워커에 넘길 것.
 */
class RollingRecorder final : public IRecorder {
public:
    /**
     * @param channel      채널 설정 (세그먼트 디렉터리, 보존 상한)
     * @param splitMuxSink 소스 파이프라인 안의 splitmuxsink 엘리먼트 (빌린 포인터)
     * @throws std::invalid_argument splitMuxSink 가 null 이거나 디렉터리가 비어 있을 때
     */
    RollingRecorder(const ChannelConfig& channel, GstElement* splitMuxSink);
    ~RollingRecorder() override;

    RollingRecorder(const RollingRecorder&) = delete;
    RollingRecorder& operator=(const RollingRecorder&) = delete;

    void setSegmentClosedCallback(SegmentClosedCallback callback) override;

    /// @details 용량 상한과 개수 상한 중 먼저 걸리는 쪽으로 오래된 세그먼트부터 지운다
    void enforceRetention() override;

    std::uint64_t usedBytes() const noexcept override { return usedBytes_.load(std::memory_order_relaxed); }
    std::string currentSegment() const override;
    veda::ChannelId channelId() const noexcept override { return channel_.channelId; }

    /// @brief 기동 시 기존 세그먼트를 스캔해 usedBytes_/목록을 복원 (재시작 후에도 상한 유지)
    void scanExistingSegments();

private:
    /// @brief splitmuxsink 의 format-location 시그널 — 다음 세그먼트 경로를 돌려준다
    static gchar* onFormatLocation(GstElement* splitmux, guint fragmentId, gpointer userData) noexcept;

    /// @brief 새 세그먼트가 열렸다 = 직전 세그먼트가 닫혔다 (스트리밍 스레드에서 호출)
    void onSegmentOpened(const char* path) noexcept;

    /// @brief 워커를 깨워 보존 정책을 돌린다. **삭제를 여기서 하지 않는다**
    void requestRetention() noexcept;

    void retentionLoop() noexcept;

    struct Segment {
        std::string path;
        std::uint64_t bytes = 0;
    };

    ChannelConfig channel_;
    GstElement* splitMuxSink_ = nullptr;  ///< 빌린 포인터 — 파이프라인이 소유

    mutable std::mutex segmentMutex_;
    std::deque<Segment> segments_;  ///< 오래된 것이 앞. 앞에서 지운다
    std::string currentSegment_;

    mutable std::mutex callbackMutex_;
    SegmentClosedCallback segmentClosedCallback_;

    /**
     * @name 비동기 삭제 워커
     * @details 파일 삭제는 blocking I/O 다. GStreamer 스트리밍 스레드에서 하면 tee 가 막혀
     *          **녹화와 중계가 함께 멈춘다.** 그래서 전용 스레드로 분리한다.
     * @warning `workCv_.notify_*` 는 반드시 `workMutex_` 를 거친 뒤에 호출할 것 —
     *          이 저장소가 반복해서 겪은 lost-wakeup 규약이다 (CLAUDE.md 참고).
     * @{
     */
    std::mutex workMutex_;
    std::condition_variable workCv_;
    bool retentionPending_ = false;
    bool stopping_ = false;
    std::thread retentionWorker_;
    /** @} */

    std::atomic_uint64_t usedBytes_{0};
};
