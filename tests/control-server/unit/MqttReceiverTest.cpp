/**
 * @file MqttReceiverTest.cpp
 * @brief control-server MQTT trust-boundary and bounded-queue tests
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "receive/MqttChannelReceiver.h"

class MqttChannelReceiverTestPeer {
public:
    static void process(MqttChannelReceiver& receiver, std::string_view topic, std::string_view payload) {
        receiver.processMessage(topic, payload);
    }

    static void setRunning(MqttChannelReceiver& receiver, bool running) { receiver.running_.store(running); }

    static void enqueue(MqttChannelReceiver& receiver, std::string_view topic, std::string_view payload) {
        receiver.handleMessage(topic, payload);
    }

    static std::size_t queueSize(const MqttChannelReceiver& receiver) { return receiver.queue_.size(); }
    static std::size_t queuedBytes(const MqttChannelReceiver& receiver) { return receiver.queuedBytes_; }
    static std::string backPayload(const MqttChannelReceiver& receiver) { return receiver.queue_.back().payload; }
    static std::uint64_t queueDroppedCount(const MqttChannelReceiver& receiver) {
        return receiver.queueDroppedCount_.load();
    }
    static constexpr std::size_t maxPayloadBytes() { return MqttChannelReceiver::kMaxTopViewPayloadBytes; }
    static constexpr std::size_t maxQueuedBytes() { return MqttChannelReceiver::kMaxQueuedPayloadBytes; }
};

namespace {

veda::TopViewFrame validFrame(int channel = 1) {
    veda::TopViewFrame frame;
    frame.v = veda::kSchemaVersion;
    frame.ts = 1787000000123;
    frame.ch = channel;
    frame.objects.push_back({7, veda::ObjectClass::Human, {1.0, 2.0}, false});
    return frame;
}

TEST(MqttReceiverTest, ValidatesTopicPayloadSchemaAndChannelBeforeCallback) {
    MqttChannelReceiver receiver(nullptr, 2, 1);
    int callbackCount = 0;
    veda::TopViewFrame received;
    receiver.setCallback([&](const veda::TopViewFrame& frame) {
        ++callbackCount;
        received = frame;
    });

    const auto frame = validFrame();
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/1/topview", veda::encode(frame));
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(received.ch, 1);
    EXPECT_EQ(receiver.receivedCount(), 1U);

    auto wrongSchema = frame;
    wrongSchema.v = veda::kSchemaVersion + 1;
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/1/topview", veda::encode(wrongSchema));
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/0/topview", veda::encode(frame));
    MqttChannelReceiverTestPeer::process(receiver, "other/ch/1/topview", veda::encode(frame));
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/1/topview", "{broken");

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(receiver.droppedCount(), 4U);
}

TEST(MqttReceiverTest, AlivePayloadRequiresExactChannelAndBooleanByte) {
    MqttChannelReceiver receiver(nullptr, 2, 1);
    std::vector<std::pair<int, bool>> states;
    receiver.setAliveCallback([&](veda::ChannelId channel, bool alive) { states.emplace_back(channel, alive); });

    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/0/alive", "1");
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/1/alive", "0");
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/2/alive", "1");
    MqttChannelReceiverTestPeer::process(receiver, "veda/ch/0/alive", "true");

    ASSERT_EQ(states.size(), 2U);
    EXPECT_EQ(states[0], std::make_pair(0, true));
    EXPECT_EQ(states[1], std::make_pair(1, false));
    EXPECT_EQ(receiver.droppedCount(), 2U);
}

TEST(MqttReceiverTest, QueueIsByteBoundedAndKeepsNewestMessage) {
    MqttChannelReceiver receiver(nullptr, 2, 1);
    MqttChannelReceiverTestPeer::setRunning(receiver, true);
    const std::string payload(MqttChannelReceiverTestPeer::maxPayloadBytes(), 'x');
    const std::string newest(MqttChannelReceiverTestPeer::maxPayloadBytes(), 'z');

    for (int i = 0; i < 140; ++i) {
        MqttChannelReceiverTestPeer::enqueue(receiver, "veda/ch/0/topview", payload);
    }
    MqttChannelReceiverTestPeer::enqueue(receiver, "veda/ch/0/topview", newest);

    EXPECT_LE(MqttChannelReceiverTestPeer::queuedBytes(receiver), MqttChannelReceiverTestPeer::maxQueuedBytes());
    EXPECT_GT(MqttChannelReceiverTestPeer::queueDroppedCount(receiver), 0U);
    EXPECT_EQ(MqttChannelReceiverTestPeer::backPayload(receiver), newest);

    const auto sizeBeforeOversized = MqttChannelReceiverTestPeer::queueSize(receiver);
    const std::string oversized(MqttChannelReceiverTestPeer::maxPayloadBytes() + 1, '!');
    MqttChannelReceiverTestPeer::enqueue(receiver, "veda/ch/0/topview", oversized);
    EXPECT_EQ(MqttChannelReceiverTestPeer::queueSize(receiver), sizeBeforeOversized);
    EXPECT_EQ(receiver.droppedCount(), 1U);

    receiver.stop();
}

}  // namespace
