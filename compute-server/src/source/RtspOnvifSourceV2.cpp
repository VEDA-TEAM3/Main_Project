#include "source/RtspOnvifSourceV2.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "log/Logger.h"
#include "network/RtspClientV2.h"

namespace {
constexpr const char* kIface = "Source";

}  // namespace

RtspOnvifSourceV2::RtspOnvifSourceV2(const AppConfig& config) : config_(config) {
    ring_.resize(kRingCapacity);
    worker_ = std::thread(&RtspOnvifSourceV2::workerLoop, this);
}

RtspOnvifSourceV2::~RtspOnvifSourceV2() {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void RtspOnvifSourceV2::workerLoop() {
    int backoffSec = 1;
    constexpr int kMaxBackoffSec = 30;

    while (!stopping_) {
        RtspClientV2 client(config_);
        client.onPayloadReceived = [this](std::string_view payload) {
            std::lock_guard<std::mutex> lk(mtx_);

            std::size_t writeIdx = 0;
            if (count_ == kRingCapacity) {
                // 링이 가득 참: 가장 오래된 프레임 자리를 그대로 재사용 -> drop-oldest
                writeIdx = head_;
                head_ = (head_ + 1) % kRingCapacity;
                ++metrics_.droppedCount;
            } else {
                writeIdx = (head_ + count_) % kRingCapacity;
                ++count_;
            }

            // 슬롯에 직접 assign(): 슬롯의 기존 vector capacity를 재사용 (콜백당 힙 할당 없음)
            domain::RawPacket& slot = ring_[writeIdx];
            slot.channelId = config_.channelId;
            slot.bytes.assign(payload.begin(), payload.end());
            slot.recvTime = std::chrono::system_clock::now();

            ++metrics_.producedCount;
            metrics_.totalBytes += slot.bytes.size();

            cv_.notify_one();
        };

        if (client.connect() && client.setup()) {
            client.play();
            backoffSec = 1;
            client.run();
        }

        if (stopping_)
            break;

        logError(kIface, "ch=" + std::to_string(config_.channelId) + " 연결 끊김 - " + std::to_string(backoffSec) +
                             "초 후 재시도");

        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::seconds(backoffSec), [this] { return stopping_.load(); });
        backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
    }
}

bool RtspOnvifSourceV2::next(domain::RawPacket& out) {
    std::string report;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return count_ > 0 || stopping_.load(); });

        if (count_ == 0)
            return false;

        const domain::RawPacket& slot = ring_[head_];

        // move 대신 copy: 슬롯의 capacity를 보존해서 다음 write에서 재사용(할당 제거)
        out.channelId = slot.channelId;
        out.bytes.assign(slot.bytes.begin(), slot.bytes.end());
        out.recvTime = slot.recvTime;

        head_ = (head_ + 1) % kRingCapacity;
        --count_;

        // 지연 시간 계측: 네트워크 도착(recvTime) ~ Pipeline이 실제로 꺼내가는 시점
        const auto latency = std::chrono::system_clock::now() - out.recvTime;
        metrics_.totalQueueLatency += std::chrono::duration_cast<std::chrono::nanoseconds>(latency);
        ++metrics_.consumedCount;

        report = buildMetricsReportIfDue();
    }
    if (!report.empty())
        logSuccess(kIface, report);
    return true;
}

std::string RtspOnvifSourceV2::buildMetricsReportIfDue() {
    using namespace std::chrono;

    const auto now = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(now - metrics_.windowStart);
    if (elapsed < kMetricsReportInterval || metrics_.consumedCount == 0)
        return {};

    const double avgLatencyUs = duration_cast<duration<double, std::micro>>(metrics_.totalQueueLatency).count() /
                                static_cast<double>(metrics_.consumedCount);
    const double fps = static_cast<double>(metrics_.consumedCount) * 1000.0 / static_cast<double>(elapsed.count());
    const double throughputKBs =
        (static_cast<double>(metrics_.totalBytes) / 1024.0) * 1000.0 / static_cast<double>(elapsed.count());

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << "최근 " << elapsed.count() << "ms 지표 - 프레임 "
        << metrics_.consumedCount << "개, 평균 큐 지연시간 " << avgLatencyUs << "us, 처리율 " << fps << "fps, 드랍 "
        << metrics_.droppedCount << "개, 처리량 " << throughputKBs << "KB/s";

    metrics_.producedCount = 0;
    metrics_.consumedCount = 0;
    metrics_.droppedCount = 0;
    metrics_.totalBytes = 0;
    metrics_.totalQueueLatency = nanoseconds{0};
    metrics_.windowStart = now;

    return oss.str();
}
