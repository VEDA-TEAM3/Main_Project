#include "receive/MqttChannelReceiver.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <utility>

#include "log/Logger.h"
#include "sink/MqttTransport.h"

namespace {

constexpr const char* kIface = "MqttChannelReceiver";
constexpr std::string_view kChannelPrefix = "veda/ch/";

bool isValidTopViewFrame(const veda::TopViewFrame& frame, int channelCount) noexcept {
    if (frame.v != veda::kSchemaVersion || frame.ts <= 0 || frame.ch < 0 || frame.ch >= channelCount) {
        return false;
    }
    for (const veda::TopViewObject& object : frame.objects) {
        if (!veda::isRiskClass(object.cls) || !std::isfinite(object.pos.x) || !std::isfinite(object.pos.y)) {
            return false;
        }
    }
    return true;
}

}  // namespace

MqttChannelReceiver::MqttChannelReceiver(std::shared_ptr<MqttTransport> transport, int channelCount,
                                        std::uint64_t retryIntervalMs)
    : transport_(std::move(transport)), channelCount_(channelCount), retryInterval_(retryIntervalMs) {}

MqttChannelReceiver::~MqttChannelReceiver() { stop(); }

void MqttChannelReceiver::setCallback(FrameCallback callback) {
    std::lock_guard lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void MqttChannelReceiver::setAliveCallback(AliveCallback callback) {
    std::lock_guard lock(callbackMutex_);
    aliveCallback_ = std::move(callback);
}

void MqttChannelReceiver::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (transport_ == nullptr) {
        running_.store(false, std::memory_order_release);
        logError(kIface, "transport가 null임 (AppContext에서 공유 MqttTransport 주입 필요)");
        return;
    }

    transport_->setMessageHandler(
        [this](std::string_view topic, std::string_view payload) { handleMessage(topic, payload); });
    transport_->setConnectionHandler([this](bool connected, int) { handleConnection(connected); });

    if (!tryConnect()) {
        logError(kIface, "start 실패 (구독 또는 transport 시작 실패) — " + std::to_string(retryInterval_.count()) +
                              "ms 간격으로 백그라운드 재시도함");
        retryThread_ = std::thread(&MqttChannelReceiver::retryLoop, this);
    }
}

void MqttChannelReceiver::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    retryCv_.notify_all();
    if (retryThread_.joinable()) {
        retryThread_.join();
    }
    if (transport_ != nullptr) {
        // transport_는 sink(발행 경로)와 공유하는 연결이므로 여기서 stop()하지 않고
        // 이 receiver가 등록한 핸들러만 해제함 (연결 자체의 수명주기는 AppContext 몫)
        transport_->setMessageHandler({});
        transport_->setConnectionHandler({});
    }
}

bool MqttChannelReceiver::tryConnect() noexcept {
    if (!transport_->subscribe(std::string(veda::topic::kTopViewAll), veda::qos::kTopView) ||
        !transport_->subscribe(std::string(veda::topic::kAliveAll), veda::qos::kAlive) || !transport_->start()) {
        return false;
    }

    logSuccess(kIface, "구독 시작 (topView=" + std::string(veda::topic::kTopViewAll) +
                            ", alive=" + std::string(veda::topic::kAliveAll) + ")");
    return true;
}

void MqttChannelReceiver::retryLoop() noexcept {
    std::unique_lock<std::mutex> lock(retryMutex_);
    while (running_.load(std::memory_order_acquire)) {
        const bool stopped =
            retryCv_.wait_for(lock, retryInterval_, [this] { return !running_.load(std::memory_order_acquire); });
        if (stopped) {
            return;
        }

        lock.unlock();
        const bool connected = tryConnect();
        lock.lock();

        if (connected) {
            return;
        }
        logError(kIface, "재시도 실패, " + std::to_string(retryInterval_.count()) + "ms 후 다시 시도함");
    }
}

std::uint64_t MqttChannelReceiver::receivedCount() const noexcept {
    return receivedCount_.load(std::memory_order_relaxed);
}

std::uint64_t MqttChannelReceiver::droppedCount() const noexcept {
    return droppedCount_.load(std::memory_order_relaxed);
}

void MqttChannelReceiver::handleMessage(std::string_view topic, std::string_view payload) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    if (const auto channel = parseChannel(topic, "/topview")) {
        const veda::TopViewFrame frame = veda::decode<veda::TopViewFrame>(payload);
        if (!isValidTopViewFrame(frame, channelCount_)) {
            recordDrop(topic, "invalid TopViewFrame");
            return;
        }
        if (frame.ch != *channel) {
            recordDrop(topic, "topic/payload channel mismatch");
            return;
        }

        FrameCallback callback;
        {
            std::lock_guard lock(callbackMutex_);
            callback = frameCallback_;
        }
        if (callback) {
            try {
                callback(frame);
                receivedCount_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                recordDrop(topic, "frame callback exception");
            }
        }
        return;
    }

    if (const auto channel = parseChannel(topic, "/alive")) {
        if (payload != "0" && payload != "1") {
            recordDrop(topic, "alive payload must be 0 or 1");
            return;
        }

        AliveCallback callback;
        {
            std::lock_guard lock(callbackMutex_);
            callback = aliveCallback_;
        }
        if (callback) {
            try {
                callback(*channel, payload == "1");
            } catch (...) {
                recordDrop(topic, "alive callback exception");
            }
        }
        return;
    }

    recordDrop(topic, "unsupported topic");
}

void MqttChannelReceiver::handleConnection(bool connected) noexcept {
    if (connected || !running_.load(std::memory_order_acquire)) {
        return;
    }

    AliveCallback callback;
    {
        std::lock_guard lock(callbackMutex_);
        callback = aliveCallback_;
    }
    if (!callback) {
        return;
    }

    for (veda::ChannelId channel = 0; channel < channelCount_; ++channel) {
        try {
            callback(channel, false);
        } catch (...) {
            recordDrop("<connection>", "alive callback exception");
            return;
        }
    }
}

std::optional<veda::ChannelId> MqttChannelReceiver::parseChannel(std::string_view topic,
                                                                 std::string_view suffix) const noexcept {
    if (!topic.starts_with(kChannelPrefix) || !topic.ends_with(suffix)) {
        return std::nullopt;
    }

    const std::size_t numberBegin = kChannelPrefix.size();
    const std::size_t numberLength = topic.size() - kChannelPrefix.size() - suffix.size();
    if (numberLength == 0) {
        return std::nullopt;
    }

    veda::ChannelId channel = -1;
    const char* begin = topic.data() + numberBegin;
    const char* end = begin + numberLength;
    const auto [parsedEnd, error] = std::from_chars(begin, end, channel);
    if (error != std::errc{} || parsedEnd != end || channel < 0 || channel >= channelCount_) {
        return std::nullopt;
    }
    return channel;
}

void MqttChannelReceiver::recordDrop(std::string_view topic, const char* reason) noexcept {
    const std::uint64_t count = droppedCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 100 == 0) {
        logError(kIface, "드랍 누적 " + std::to_string(count) + "건, topic=" + std::string(topic) +
                              ", 사유=" + std::string(reason != nullptr ? reason : "unknown"));
    }
}
