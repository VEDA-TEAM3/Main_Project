#include "aggregate/TimeWindowAggregatorV2.h"

#include <iomanip>
#include <sstream>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Aggregator";
}  // namespace

TimeWindowAggregatorV2::TimeWindowAggregatorV2(std::shared_ptr<IClock> clock, uint64_t windowSizeMs, int channelCount)
    : clock_(std::move(clock)),
      windowSizeMs_(windowSizeMs),
      channelCount_(channelCount),
      latestByChannel_(static_cast<std::size_t>(channelCount)) {}

void TimeWindowAggregatorV2::setCallback(AggregationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void TimeWindowAggregatorV2::push(const veda::TopViewFrame& frame) {
    if (frame.ch < 0 || frame.ch >= channelCount_) {
        logError(kIface, "channelId " + std::to_string(frame.ch) + " 이 channelCount(" + std::to_string(channelCount_) +
                             ") 범위 밖 - 프레임 드롭");
        return;
    }

    std::vector<veda::TopViewFrame> flushedFrames;
    AggregationCallback callbackCopy;
    bool windowClosed = false;
    bool callbackMissing = false;
    std::size_t missedChannelCount = 0;

    // 지연 시간 계측: mutex_를 실제로 쥐고 있는 시간만 잰다 (콜백 호출은 락 밖에서 일어나므로 제외)
    const auto lockStart = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = clock_->now();

        if (filledCount_ == 0) {
            windowStartTime_ = now;
        }

        if (now - windowStartTime_ >= static_cast<veda::TimestampMs>(windowSizeMs_)) {
            if (filledCount_ > 0) {
                if (callback_) {
                    // move: 어차피 슬롯을 비울 것이므로 복사 없이 그대로 꺼냄
                    flushedFrames.reserve(filledCount_);
                    for (auto& slot : latestByChannel_) {
                        if (slot.has_value()) {
                            flushedFrames.push_back(std::move(*slot));
                            slot.reset();
                        }
                    }
                    callbackCopy = callback_;
                    windowClosed = true;
                } else {
                    callbackMissing = true;
                    missedChannelCount = filledCount_;
                    for (auto& slot : latestByChannel_) {
                        slot.reset();
                    }
                }
            }
            filledCount_ = 0;
            windowStartTime_ = now;
        }

        auto& slot = latestByChannel_[static_cast<std::size_t>(frame.ch)];
        if (!slot.has_value())
            ++filledCount_;
        slot = frame;
    }  // <- mutex_ 해제 시점 (콜백은 아직 호출 전)
    const auto lockHoldTime = std::chrono::steady_clock::now() - lockStart;

    // 콜백(=다운스트림 파이프라인 전체)은 락 밖에서 호출 -> 다른 채널의 push()를 막지 않음
    if (windowClosed) {
        const std::size_t channelCount = flushedFrames.size();
        callbackCopy(std::move(flushedFrames));
        // 윈도우마다(초당 10회) 도는 정상 경로라 Debug
        if (isLogEnabled(LogLevel::Debug)) {
            logDebug(kIface, "윈도우 마감, " + std::to_string(channelCount) +
                                 "채널 집계 완료 → 다음 단계 전달 (락 밖에서 호출)");
        }
    } else if (callbackMissing) {
        logError(kIface, "윈도우 마감했지만 콜백 미등록 - " + std::to_string(missedChannelCount) + "채널 데이터 유실");
    }

    std::string report;
    {
        std::lock_guard<std::mutex> mlock(metricsMutex_);
        metrics_.totalLockHoldTime += lockHoldTime;
        ++metrics_.pushCount;
        if (windowClosed)
            ++metrics_.windowCount;
        report = buildMetricsReportIfDue();
    }
    if (!report.empty())
        logSuccess(kIface, report);
}

std::string TimeWindowAggregatorV2::buildMetricsReportIfDue() {
    using namespace std::chrono;

    const auto now = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(now - metrics_.windowStart);
    if (elapsed < kMetricsReportInterval || metrics_.pushCount == 0)
        return {};

    const double avgLockHoldUs = duration_cast<duration<double, std::micro>>(metrics_.totalLockHoldTime).count() /
                                 static_cast<double>(metrics_.pushCount);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << "최근 " << elapsed.count() << "ms 지표 - push() " << metrics_.pushCount
        << "회, 윈도우 마감 " << metrics_.windowCount << "회, 평균 락 보유시간 " << avgLockHoldUs << "us";

    metrics_.pushCount = 0;
    metrics_.windowCount = 0;
    metrics_.totalLockHoldTime = nanoseconds{0};
    metrics_.windowStart = now;

    return oss.str();
}
