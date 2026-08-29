/**
 * @file MqttSinkTest.cpp
 * @brief compute-server MQTT sink validation and queue policy tests
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IMqttTransport.h"
#include "sink/MqttBlurSink.h"
#include "sink/MqttTopViewSink.h"

namespace {

class FakeMqttTransport final : public IMqttTransport {
public:
    struct Publication {
        std::string topic;
        std::string payload;
        int qos = -1;
        bool retain = false;
    };

    ListenerId addConnectionListener(std::function<void(bool)> listener) override {
        std::lock_guard lock(mutex_);
        listener_ = std::move(listener);
        return 1;
    }

    void removeConnectionListener(ListenerId) noexcept override {
        std::lock_guard lock(mutex_);
        listener_ = {};
    }

    bool start() noexcept override { return true; }
    void stop() noexcept override {}

    bool publish(std::string_view topic, std::string_view payload, int qos, bool retain) noexcept override {
        {
            std::lock_guard lock(mutex_);
            publications_.push_back({std::string(topic), std::string(payload), qos, retain});
        }
        published_.notify_all();
        return true;
    }

    bool isReady() const noexcept override { return ready_.load(); }
    bool isConnected() const noexcept override { return connected_.load(); }

    void setConnected(bool connected) {
        connected_.store(connected);
        std::function<void(bool)> listener;
        {
            std::lock_guard lock(mutex_);
            listener = listener_;
        }
        if (listener) {
            listener(connected);
        }
    }

    bool waitForPublications(std::size_t count) {
        std::unique_lock lock(mutex_);
        return published_.wait_for(lock, std::chrono::seconds(1), [&] { return publications_.size() >= count; });
    }

    Publication publication(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return publications_.at(index);
    }

    std::size_t publicationCount() const {
        std::lock_guard lock(mutex_);
        return publications_.size();
    }

private:
    std::atomic_bool ready_{true};
    std::atomic_bool connected_{true};
    mutable std::mutex mutex_;
    std::condition_variable published_;
    std::function<void(bool)> listener_;
    std::vector<Publication> publications_;
};

AppConfig configForChannel(int channel) {
    AppConfig config;
    config.channelId = channel;
    config.mqttTopViewMaxQueueSize = 2;
    config.mqttBlurMaxQueueSize = 2;
    return config;
}

veda::TopViewFrame topViewFrame(int channel, veda::TimestampMs timestamp) {
    veda::TopViewFrame frame;
    frame.v = veda::kSchemaVersion;
    frame.ch = channel;
    frame.ts = timestamp;
    return frame;
}

TEST(MqttSinkTest, EmptyTopViewSnapshotUsesContractTopicAndQos) {
    auto transport = std::make_shared<FakeMqttTransport>();
    MqttTopViewSink sink(transport, configForChannel(2));
    sink.start();

    sink.send(topViewFrame(2, 100));

    ASSERT_TRUE(transport->waitForPublications(1));
    const auto publication = transport->publication(0);
    EXPECT_EQ(publication.topic, veda::topic::topView(2));
    EXPECT_EQ(publication.qos, veda::qos::kTopView);
    EXPECT_FALSE(publication.retain);
    const auto decoded = veda::decode<veda::TopViewFrame>(publication.payload);
    EXPECT_EQ(decoded.ts, 100);
    EXPECT_TRUE(decoded.objects.empty());
}

TEST(MqttSinkTest, StructurallyInvalidTopViewFrameIsDropped) {
    auto transport = std::make_shared<FakeMqttTransport>();
    MqttTopViewSink sink(transport, configForChannel(2));
    sink.start();

    sink.send(topViewFrame(1, 100));

    EXPECT_EQ(sink.droppedCount(), 1U);
    EXPECT_EQ(transport->publicationCount(), 0U);
}

TEST(MqttSinkTest, InvalidBlurTargetDoesNotDiscardValidPrivacyTarget) {
    auto transport = std::make_shared<FakeMqttTransport>();
    MqttBlurSink sink(transport, configForChannel(2));
    sink.start();

    veda::BlurFrame frame;
    frame.v = veda::kSchemaVersion;
    frame.ch = 2;
    frame.ts = 200;
    frame.blurs = {
        {1, veda::ObjectClass::Unknown, {0.1, 0.2, 0.3, 0.4}},
        {2, veda::ObjectClass::Human, {0.2, 0.3, 0.4, 0.5}},
    };
    sink.send(frame);

    ASSERT_TRUE(transport->waitForPublications(1));
    const auto publication = transport->publication(0);
    EXPECT_EQ(publication.topic, veda::topic::blur(2));
    const auto decoded = veda::decode<veda::BlurFrame>(publication.payload);
    ASSERT_EQ(decoded.blurs.size(), 1U);
    EXPECT_EQ(decoded.blurs.front().id, 1);
    EXPECT_EQ(decoded.blurs.front().cls, veda::ObjectClass::Unknown);
    EXPECT_EQ(sink.droppedCount(), 1U);
}

TEST(MqttSinkTest, BacklogDropsOldestAndPublishesLatestSnapshot) {
    auto transport = std::make_shared<FakeMqttTransport>();
    transport->setConnected(false);
    MqttTopViewSink sink(transport, configForChannel(2));
    sink.start();

    sink.send(topViewFrame(2, 100));
    sink.send(topViewFrame(2, 200));
    sink.send(topViewFrame(2, 300));
    EXPECT_GE(sink.droppedCount(), 1U);

    transport->setConnected(true);

    ASSERT_TRUE(transport->waitForPublications(1));
    const auto decoded = veda::decode<veda::TopViewFrame>(transport->publication(0).payload);
    EXPECT_EQ(decoded.ts, 300);
}

}  // namespace
