#include "receive/MqttChannelReceiver.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <utility>

#include "sink/MqttTransport.h"

namespace {

constexpr std::string_view channelPrefix = "veda/ch/";

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

MqttTransport::Config makeDefaultTransportConfig() {
    MqttTransport::Config config;
    config.clientId = "veda-control-receiver";

    if (const char* value = std::getenv("VEDA_MQTT_HOST")) {
        config.host = value;
    }
    if (const char* value = std::getenv("VEDA_MQTT_PORT")) {
        try {
            config.port = std::stoi(value);
        } catch (...) {
            std::fprintf(stderr, "[MqttChannelReceiver] invalid VEDA_MQTT_PORT; using %d\n", config.port);
        }
    }
    if (const char* value = std::getenv("VEDA_MQTT_CA_FILE")) {
        config.caFile = value;
    }
    if (const char* value = std::getenv("VEDA_MQTT_USE_TLS")) {
        const std::string_view enabled(value);
        config.useTls = enabled == "1" || enabled == "true" || enabled == "TRUE" || enabled == "on";
    }
    return config;
}

}  // namespace

MqttChannelReceiver::MqttChannelReceiver()
    : MqttChannelReceiver(std::make_shared<MqttTransport>(makeDefaultTransportConfig()), Config{}) {}

MqttChannelReceiver::MqttChannelReceiver(std::shared_ptr<MqttTransport> transport)
    : MqttChannelReceiver(std::move(transport), Config{}) {}

MqttChannelReceiver::MqttChannelReceiver(std::shared_ptr<MqttTransport> transport, Config config)
    : transport_(std::move(transport)), config_(std::move(config)) {}

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
        std::fprintf(stderr, "[MqttChannelReceiver] transport is null\n");
        return;
    }

    transport_->setMessageHandler(
        [this](std::string_view topic, std::string_view payload) { handleMessage(topic, payload); });
    transport_->setConnectionHandler(
        [this](bool connected, int) { handleConnection(connected); });

    if (!transport_->subscribe(config_.topViewTopic, config_.topViewQos) ||
        !transport_->subscribe(config_.aliveTopic, config_.aliveQos) || !transport_->start()) {
        running_.store(false, std::memory_order_release);
        transport_->setMessageHandler({});
        transport_->setConnectionHandler({});
        std::fprintf(stderr, "[MqttChannelReceiver] start failed\n");
    }
}

void MqttChannelReceiver::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (transport_ != nullptr) {
        transport_->setMessageHandler({});
        transport_->setConnectionHandler({});
        transport_->stop();
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
        if (!isValidTopViewFrame(frame, config_.channelCount)) {
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

    for (veda::ChannelId channel = 0; channel < config_.channelCount; ++channel) {
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
    if (!topic.starts_with(channelPrefix) || !topic.ends_with(suffix)) {
        return std::nullopt;
    }

    const std::size_t numberBegin = channelPrefix.size();
    const std::size_t numberLength = topic.size() - channelPrefix.size() - suffix.size();
    if (numberLength == 0) {
        return std::nullopt;
    }

    veda::ChannelId channel = -1;
    const char* begin = topic.data() + numberBegin;
    const char* end = begin + numberLength;
    const auto [parsedEnd, error] = std::from_chars(begin, end, channel);
    if (error != std::errc{} || parsedEnd != end || channel < 0 || channel >= config_.channelCount) {
        return std::nullopt;
    }
    return channel;
}

void MqttChannelReceiver::recordDrop(std::string_view topic, const char* reason) noexcept {
    const std::uint64_t count = droppedCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 100 == 0) {
        std::fprintf(stderr, "[MqttChannelReceiver] dropped=%llu topic=%.*s reason=%s\n",
                     static_cast<unsigned long long>(count), static_cast<int>(topic.size()), topic.data(),
                     reason != nullptr ? reason : "unknown");
    }
}
