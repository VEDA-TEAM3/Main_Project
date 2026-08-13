#include "aggregate/TimeWindowAggregatorV2.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Aggregator";
constexpr int kMaxChannelCount = static_cast<int>(std::numeric_limits<std::uint8_t>::max()) + 1;
}  // namespace

TimeWindowAggregatorV2::TimeWindowAggregatorV2(std::shared_ptr<IClock> clock, uint64_t windowSizeMs, int channelCount)
    : clock_(std::move(clock)),
      windowSizeMs_(windowSizeMs),
      channelCount_(channelCount),
      flushPool_(kFlushBufferPoolSize, static_cast<std::size_t>(channelCount > 0 ? channelCount : 1)) {
    // AppConfig 가 이미 [1, 256] 으로 보정하지만, DI 로 직접 넣는 경로(테스트/다른 조립부)까지
    // 막으려면 여기서도 거부해야 한다. 음수가 그대로 오면 static_cast<size_t> 가 거대한 값이 되어
    // 생성자가 곧바로 수 GB 를 요구한다 -- 조용히 통과시키면 안 되는 값이다.
    if (!clock_) {
        throw std::invalid_argument("aggregator requires a non-null clock");
    }
    if (channelCount_ < 1 || channelCount_ > kMaxChannelCount) {
        throw std::invalid_argument("aggregator channelCount out of range [1, 256]");
    }

    const auto count = static_cast<std::size_t>(channelCount_);
    slots_.resize(count);
    occupied_.assign(count, 0);
    activeChannels_.reserve(count);  // 이후 push_back 은 재할당하지 않는다
}

void TimeWindowAggregatorV2::setCallback(AggregationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void TimeWindowAggregatorV2::fillFlushBufferLocked(FrameBufferPool::Buffer& out) {
    // resize 는 정상 운용(모든 채널 생존)에서 매 윈도우 같은 값이라 사실상 no-op 이고,
    // 생성 시 reserve(channelCount) 를 해 두었으므로 외곽 벡터 재할당도 없다.
    out.resize(activeChannels_.size());

    std::size_t i = 0;
    for (const veda::ChannelId ch : activeChannels_) {
        auto& slot = slots_[static_cast<std::size_t>(ch)];
        auto& dst = out[i++];

        dst.v = slot.v;
        dst.ts = slot.ts;
        dst.ch = slot.ch;
        // swap: 복사가 아니라 버퍼 교환. 슬롯은 dst 가 갖고 있던 (비었지만 capacity 는 살아 있는)
        // 버퍼를 넘겨받아 다음 윈도우에서 재사용한다 -> 양쪽 다 해제가 일어나지 않는다.
        dst.objects.swap(slot.objects);
        slot.objects.clear();

        occupied_[static_cast<std::size_t>(ch)] = 0;
    }
    activeChannels_.clear();
}

void TimeWindowAggregatorV2::clearSlotsLocked() {
    for (const veda::ChannelId ch : activeChannels_) {
        const auto idx = static_cast<std::size_t>(ch);
        slots_[idx].objects.clear();  // capacity 유지
        occupied_[idx] = 0;
    }
    activeChannels_.clear();
}

void TimeWindowAggregatorV2::push(const veda::TopViewFrame& frame) {
    if (frame.ch < 0 || frame.ch >= channelCount_) {
        logError(kIface, "channelId " + std::to_string(frame.ch) + " 이 channelCount(" + std::to_string(channelCount_) +
                             ") 범위 밖 - 프레임 드롭");
        std::lock_guard<std::mutex> mlock(metricsMutex_);
        ++metrics_.droppedCount;
        return;
    }
    // [ OOM 방어 ] 원격에서 온 프레임의 객체 수는 신뢰할 수 없다. 상한을 넘으면 통째로 버린다
    // -- 잘라서 받으면 '일부만 반영된 프레임'이라는 더 나쁜 상태가 된다.
    if (frame.objects.size() > kMaxObjectsPerFrame) {
        logError(kIface, "ch=" + std::to_string(frame.ch) + " 객체 " + std::to_string(frame.objects.size()) +
                             "개가 상한(" + std::to_string(kMaxObjectsPerFrame) + ") 초과 - 프레임 드롭");
        std::lock_guard<std::mutex> mlock(metricsMutex_);
        ++metrics_.droppedCount;
        return;
    }

    FrameBufferPool::Buffer flushed;
    AggregationCallback callbackCopy;
    bool windowClosed = false;
    std::size_t missedChannelCount = 0;

    // 지연 계측: mutex_ 를 실제로 쥐고 있는 시간만 잰다 (콜백은 락 밖이라 제외)
    const auto lockStart = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto now = clock_->now();
        if (activeChannels_.empty()) {
            windowStartTime_ = now;
        }

        if (now - windowStartTime_ >= static_cast<veda::TimestampMs>(windowSizeMs_)) {
            if (!activeChannels_.empty()) {
                if (callback_) {
                    flushed = flushPool_.acquire();
                    fillFlushBufferLocked(flushed);
                    callbackCopy = callback_;
                    windowClosed = true;
                } else {
                    missedChannelCount = activeChannels_.size();
                    clearSlotsLocked();
                }
            }
            windowStartTime_ = now;
        }

        const auto idx = static_cast<std::size_t>(frame.ch);
        auto& slot = slots_[idx];
        slot.v = frame.v;
        slot.ts = frame.ts;
        slot.ch = frame.ch;
        // assign 은 목적지 capacity 가 충분하면 재할당하지 않는다 -> warmup 이후 무할당
        slot.objects.assign(frame.objects.begin(), frame.objects.end());
        if (!occupied_[idx]) {
            occupied_[idx] = 1;
            activeChannels_.push_back(frame.ch);
        }
    }  // <- mutex_ 해제 (콜백은 아직 호출 전)
    const auto lockEnd = std::chrono::steady_clock::now();

    // 콜백(=다운스트림 파이프라인 전체)은 락 밖에서 호출 -> 다른 채널의 push() 를 막지 않음
    if (windowClosed) {
        const std::size_t flushedCount = flushed.size();
        callbackCopy(flushed);
        flushPool_.release(std::move(flushed));  // 버퍼 반납 (해제 아님)

        // 윈도우마다(초당 10회) 도는 정상 경로라 Debug -- 문자열 조립까지 레벨로 걸러냄
        if (isLogEnabled(LogLevel::Debug)) {
            logDebug(kIface, "윈도우 마감, " + std::to_string(flushedCount) +
                                 "채널 집계 완료 → 다음 단계 전달 (락 밖에서 호출)");
        }
    } else if (missedChannelCount > 0) {
        logError(kIface, "윈도우 마감했지만 콜백 미등록 - " + std::to_string(missedChannelCount) + "채널 데이터 유실");
    }

    std::string report;
    {
        std::lock_guard<std::mutex> mlock(metricsMutex_);
        metrics_.totalLockHoldTime += lockEnd - lockStart;
        ++metrics_.pushCount;
        if (windowClosed) {
            ++metrics_.windowCount;
        }
        report = buildMetricsReportIfDue(lockEnd);
    }
    if (!report.empty()) {
        logSuccess(kIface, report);
    }
}

std::string TimeWindowAggregatorV2::buildMetricsReportIfDue(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;

    const auto elapsed = duration_cast<milliseconds>(now - metrics_.windowStart);
    if (elapsed < kMetricsReportInterval || metrics_.pushCount == 0) {
        return {};
    }

    const double avgLockHoldUs = duration_cast<duration<double, std::micro>>(metrics_.totalLockHoldTime).count() /
                                 static_cast<double>(metrics_.pushCount);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << "최근 " << elapsed.count() << "ms 지표 - push() " << metrics_.pushCount
        << "회, 윈도우 마감 " << metrics_.windowCount << "회, 평균 락 보유시간 " << avgLockHoldUs << "us, 드롭 "
        << metrics_.droppedCount << "건, 풀 고갈 " << flushPool_.exhaustedCount() << "회";

    metrics_.pushCount = 0;
    metrics_.windowCount = 0;
    metrics_.droppedCount = 0;
    metrics_.totalLockHoldTime = nanoseconds{0};
    metrics_.windowStart = now;

    return oss.str();
}
