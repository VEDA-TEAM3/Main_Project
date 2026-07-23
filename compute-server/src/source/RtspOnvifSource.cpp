#include "source/RtspOnvifSource.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "Logger.h"
#include "network/RtspClientV2.h"

namespace {
constexpr const char* kIface = "Source";
}  // namespace

RtspOnvifSource::RtspOnvifSource(const AppConfig& config) : config_(config) {
    worker_ = std::thread(&RtspOnvifSource::workerLoop, this);
}

RtspOnvifSource::~RtspOnvifSource() {
    stop();
    if (worker_.joinable())
        worker_.join();
}

void RtspOnvifSource::stop() noexcept {
    stopping_.store(true);
    cv_.notify_all();
}

void RtspOnvifSource::workerLoop() {
    int backoffSec = 1;
    constexpr int kMaxBackoffSec = 30;

    while (!stopping_) {
        RtspClientV2 client(config_);
        client.onPayloadReceived = [this](std::string_view payload) {
            domain::RawPacket pkt;
            pkt.channelId = config_.channelId;
            pkt.bytes.assign(payload.begin(), payload.end());
            pkt.recvTime = std::chrono::system_clock::now();
            const std::size_t sz = pkt.bytes.size();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                queue_.push(std::move(pkt));
                ++metrics_.producedCount;
                metrics_.totalBytes += sz;
            }
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

bool RtspOnvifSource::next(domain::RawPacket& out) {
    std::string report;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !queue_.empty() || stopping_.load(); });

        if (queue_.empty())
            return false;

        out = std::move(queue_.front());
        queue_.pop();

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

std::string RtspOnvifSource::buildMetricsReportIfDue() {
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