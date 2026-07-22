#include "aggregate/TimeWindowAggregator.h"

#include <iomanip>
#include <sstream>
#include <string>

#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Aggregator";
}  // namespace

TimeWindowAggregator::TimeWindowAggregator(std::shared_ptr<IClock> clock, uint64_t windowSizeMs)
    : clock_(std::move(clock)), windowSizeMs_(windowSizeMs), windowStartTime_(0) {}

void TimeWindowAggregator::setCallback(AggregationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void TimeWindowAggregator::push(const veda::TopViewFrame& frame) {
    // 지연 시간 계측: mutex_를 실제로 쥐고 있는 시간 (V1은 콜백 호출까지 이 락 안에서 일어남 ->
    // TimeWindowAggregatorV2와 비교하면 락 경합이 얼마나 줄었는지 바로 드러남)
    const auto lockStart = std::chrono::steady_clock::now();
    bool windowClosed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = clock_->now();

        if (latestByChannel_.empty()) {
            windowStartTime_ = now;
        }

        if (now - windowStartTime_ >= windowSizeMs_) {
            if (!latestByChannel_.empty()) {
                if (callback_) {
                    std::vector<veda::TopViewFrame> frames;
                    frames.reserve(latestByChannel_.size());
                    for (const auto& [ch, f] : latestByChannel_) {
                        frames.push_back(f);
                    }
                    const std::size_t channelCount = frames.size();
                    callback_(std::move(frames));  // 원본 그대로: 콜백(=다운스트림 파이프라인 전체)이 락 안에서 실행됨
                    logSuccess(kIface,
                               "윈도우 마감, " + std::to_string(channelCount) + "채널 집계 완료 → 다음 단계 전달");
                    windowClosed = true;
                } else {
                    logError(kIface, "윈도우 마감했지만 콜백 미등록 - " + std::to_string(latestByChannel_.size()) +
                                         "채널 데이터 유실");
                }
            }
            latestByChannel_.clear();
            windowStartTime_ = now;
        }

        latestByChannel_[frame.ch] = frame;
    }  // <- mutex_ 해제 시점

    std::string report;
    {
        std::lock_guard<std::mutex> mlock(metricsMutex_);
        metrics_.totalLockHoldTime += std::chrono::steady_clock::now() - lockStart;
        ++metrics_.pushCount;
        if (windowClosed)
            ++metrics_.windowCount;
        report = buildMetricsReportIfDue();
    }
    if (!report.empty())
        logSuccess(kIface, report);
}

std::string TimeWindowAggregator::buildMetricsReportIfDue() {
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