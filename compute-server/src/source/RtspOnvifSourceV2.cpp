#include "source/RtspOnvifSourceV2.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

#include "Logger.h"
#include "network/RtspClientV2.h"

namespace {
constexpr const char* kIface = "Source";

}  // namespace

RtspOnvifSourceV2::RtspOnvifSourceV2(const AppConfig& config)
    : ringCapacity_(static_cast<std::size_t>(config.sourceRingCapacity)),
      metricsReportInterval_(config.metricsReportIntervalMs),
      backoffInitialSec_(config.rtspReconnectBackoffInitialSec),
      backoffMaxSec_(config.rtspReconnectBackoffMaxSec),
      config_(config) {
    ring_.resize(ringCapacity_);
    worker_ = std::thread(&RtspOnvifSourceV2::workerLoop, this);
}

RtspOnvifSourceV2::~RtspOnvifSourceV2() {
    stop();
    if (worker_.joinable())
        worker_.join();
}

void RtspOnvifSourceV2::stop() noexcept {
    stopping_.store(true);

    // 진행 중인 RTSP 세션을 즉시 끊는다. 이게 없으면 스트림이 건강한 동안 recv() 가 계속 성공해
    // run() 이 반환하지 않고, 소멸자의 worker_.join() 이 무한 대기한다
    // (= 프로세스가 SIGINT/SIGTERM 에 응답하지 못하고 MQTT 종료 신호도 못 보냄)
    {
        std::lock_guard<std::mutex> lk(clientMutex_);
        if (activeClient_ != nullptr)
            activeClient_->cancel();
    }

    cv_.notify_all();
}

void RtspOnvifSourceV2::workerLoop() {
    int backoffSec = backoffInitialSec_;

    while (!stopping_) {
        // client 보다 먼저 선언 -> client 가 먼저 파괴되므로, 콜백이 참조하는 동안 항상 살아있음
        bool sessionProductive = false;

        RtspClientV2 client(config_);
        client.onPayloadReceived = [this, &sessionProductive](std::string_view payload) {
            // 실제로 payload 를 받은 세션만 '생산적'으로 본다 (백오프 초기화의 유일한 기준)
            sessionProductive = true;

            std::lock_guard<std::mutex> lk(mtx_);

            std::size_t writeIdx = 0;
            if (count_ == ringCapacity_) {
                // 링이 가득 참: 가장 오래된 프레임 자리를 그대로 재사용 -> drop-oldest
                writeIdx = head_;
                head_ = (head_ + 1) % ringCapacity_;
                ++metrics_.droppedCount;
            } else {
                writeIdx = (head_ + count_) % ringCapacity_;
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

        // stop() 이 이 세션을 취소할 수 있도록 등록 (파괴 전에 반드시 해제해야 UAF 가 없음)
        {
            std::lock_guard<std::mutex> lk(clientMutex_);
            if (stopping_)
                break;
            activeClient_ = &client;
        }

        // PLAY 가 200 이 아니면 스트리밍이 시작되지 않으므로 run() 에 들어가지 않는다
        // (예전에는 실패해도 run() 을 불렀고, 그 직전에 백오프를 무조건 1로 리셋하고 있었음)
        if (client.connect() && client.setup()) {
            client.play();
            if (client.playSucceeded())
                client.run();
        }

        {
            std::lock_guard<std::mutex> lk(clientMutex_);
            activeClient_ = nullptr;
        }

        if (stopping_)
            break;

        // [백오프 정책] 실제로 데이터를 받은 세션이었을 때만 초기값으로 되돌린다.
        // connect/setup 은 되는데 PLAY 가 계속 실패하는 설정 오류(잘못된 rtspPlayUri 등)에서도
        // 지수 백오프가 제대로 커지도록 하기 위함 -- 예전에는 매 시도마다 1초로 리셋되어
        // 카메라를 1초 간격으로 두드리는 재접속 폭풍이 발생했음
        if (sessionProductive)
            backoffSec = backoffInitialSec_;

        logError(kIface, "ch=" + std::to_string(config_.channelId) + " 연결 끊김 - " + std::to_string(backoffSec) +
                             "초 후 재시도");

        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::seconds(backoffSec), [this] { return stopping_.load(); });
        }
        if (stopping_)
            break;

        // 다음 실패에 대비해 증가 (backoffMaxSec_ 에서 포화)
        backoffSec = std::min(backoffSec * 2, backoffMaxSec_);
    }
}

bool RtspOnvifSourceV2::next(domain::RawPacket& out) {
    std::string report;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return count_ > 0 || stopping_.load(); });

        if (count_ == 0)
            return false;

        domain::RawPacket& slot = ring_[head_];

        out.channelId = slot.channelId;
        // 복사 대신 버퍼 소유권 교환(O(1) 포인터 스왑): out 이 들고 있던(이미 소비된) 버퍼가
        // 슬롯으로 넘어가 다음 write 의 capacity 로 재사용됨 -> per-frame memcpy 제거 + capacity 보존.
        // 슬롯/파이프라인 모두 mtx_ 안에서만 bytes 를 만지므로 스왑은 스레드 안전함.
        std::swap(out.bytes, slot.bytes);
        out.recvTime = slot.recvTime;

        head_ = (head_ + 1) % ringCapacity_;
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
    if (elapsed < metricsReportInterval_ || metrics_.consumedCount == 0)
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
