/**
 * @file SerialHwEventDispatcherTest.cpp
 * @brief UART framing, deduplication, mismatch retry, and heartbeat timeout tests
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dispatch/SerialHwEventDispatcher.h"

namespace {

class PseudoTerminal {
public:
    PseudoTerminal() {
        master_ = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
            throw std::runtime_error("failed to create pseudo terminal");
        }
        const char* path = ptsname(master_);
        if (path == nullptr) {
            throw std::runtime_error("failed to resolve pseudo terminal path");
        }
        slavePath_ = path;
    }

    ~PseudoTerminal() {
        if (master_ >= 0) {
            close(master_);
        }
    }

    const std::string& slavePath() const { return slavePath_; }

    template <typename Frame>
    bool readFrame(Frame& frame, int timeoutMs = 500) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(&frame);
        std::size_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (received < sizeof(Frame)) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return false;
            }
            pollfd descriptor{master_, POLLIN, 0};
            if (poll(&descriptor, 1, static_cast<int>(remaining.count())) <= 0) {
                return false;
            }
            const ssize_t count = read(master_, bytes + received, sizeof(Frame) - received);
            if (count > 0) {
                received += static_cast<std::size_t>(count);
            }
        }
        return true;
    }

    bool hasData(int timeoutMs) {
        pollfd descriptor{master_, POLLIN, 0};
        return poll(&descriptor, 1, timeoutMs) > 0;
    }

    bool writeFrame(const veda_uplink_frame_t& frame) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&frame);
        std::size_t written = 0;
        while (written < sizeof(frame)) {
            const ssize_t count = write(master_, bytes + written, sizeof(frame) - written);
            if (count <= 0) {
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

private:
    int master_ = -1;
    std::string slavePath_;
};

veda_uplink_frame_t uplink(std::uint8_t channel, std::uint8_t reason, bool red, bool yellow, bool green) {
    veda_uplink_frame_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.start_byte = VEDA_START_BYTE;
    frame.payload.channel_id = channel;
    frame.payload.reason = reason;
    frame.payload.siren_on = red;
    frame.payload.buzzer_on = red || yellow;
    frame.payload.led_red = red;
    frame.payload.led_yellow = yellow;
    frame.payload.led_green = green;
    veda_write_i64_le(&frame.payload.timestamp_ms, 1234);
    frame.checksum = veda_uplink_checksum(&frame.payload);
    frame.end_byte = VEDA_END_BYTE;
    return frame;
}

struct StatusRecord {
    int channel = -1;
    bool alive = false;
    HwIndicatorState indicators;
};

TEST(SerialHwEventDispatcherTest, EnforcesWireAndHealthContractsEndToEnd) {
    PseudoTerminal terminal;
    SerialHwEventDispatcher dispatcher(terminal.slavePath(), 20, 3, 2, true);

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<StatusRecord> statuses;
    std::vector<std::pair<int, bool>> faults;
    dispatcher.setStatusCallback([&](veda::ChannelId channel, bool alive, const HwIndicatorState& indicators) {
        {
            std::lock_guard lock(mutex);
            statuses.push_back({channel, alive, indicators});
        }
        changed.notify_all();
    });
    dispatcher.setFaultCallback([&](veda::ChannelId channel, bool faulted) {
        {
            std::lock_guard lock(mutex);
            faults.emplace_back(channel, faulted);
        }
        changed.notify_all();
    });

    domain::RiskEvaluation evaluation;
    evaluation.timestamp = 987654321;
    evaluation.zoneLevels = {{3, veda::RiskLevel::Danger, 1.234}};
    dispatcher.dispatch(evaluation);

    veda_downlink_frame_t downlink;
    ASSERT_TRUE(terminal.readFrame(downlink));
    EXPECT_EQ(downlink.start_byte, VEDA_START_BYTE);
    EXPECT_EQ(downlink.end_byte, VEDA_END_BYTE);
    EXPECT_EQ(downlink.checksum, veda_downlink_checksum(&downlink.payload));
    EXPECT_EQ(downlink.payload.channel_id, 3);
    EXPECT_EQ(downlink.payload.risk_level, VEDA_RISK_DANGER);
    EXPECT_EQ(veda_read_i64_le(&downlink.payload.timestamp_ms), evaluation.timestamp);
    EXPECT_EQ(veda_read_u16_le(&downlink.payload.dist_mm), 1234);

    dispatcher.dispatch(evaluation);
    EXPECT_FALSE(terminal.hasData(40)) << "unchanged risk level must not be retransmitted";

    auto malformed = uplink(3, VEDA_UPLINK_REASON_HEARTBEAT, false, true, false);
    malformed.checksum ^= 0xFF;
    ASSERT_TRUE(terminal.writeFrame(malformed));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard lock(mutex);
        EXPECT_TRUE(statuses.empty());
    }

    const auto mismatch = uplink(3, VEDA_UPLINK_REASON_HEARTBEAT, false, true, false);
    ASSERT_TRUE(terminal.writeFrame(mismatch));
    ASSERT_TRUE(terminal.readFrame(downlink));
    std::size_t statusCountAfterFirst = 0;
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::milliseconds(500),
                                     [&] { return !statuses.empty() && statuses.back().indicators.ledYellow; }));
        statusCountAfterFirst = statuses.size();
        EXPECT_TRUE(statuses.back().alive);
    }

    ASSERT_TRUE(terminal.writeFrame(mismatch));
    ASSERT_TRUE(terminal.readFrame(downlink));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(statuses.size(), statusCountAfterFirst) << "identical heartbeat state must be deduplicated";
    }

    ASSERT_TRUE(terminal.writeFrame(mismatch));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::milliseconds(500),
                                     [&] { return !faults.empty() && faults.back() == std::make_pair(3, true); }));
    }
    EXPECT_FALSE(terminal.hasData(40)) << "retry budget is two frames";

    const auto matched = uplink(3, VEDA_UPLINK_REASON_HEARTBEAT, true, false, false);
    ASSERT_TRUE(terminal.writeFrame(matched));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::milliseconds(500),
                                     [&] { return !faults.empty() && faults.back() == std::make_pair(3, false); }));
    }

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::milliseconds(500),
                                     [&] { return !statuses.empty() && !statuses.back().alive; }));
        EXPECT_TRUE(statuses.back().indicators.ledRed)
            << "dead status must preserve the last indicators as explicitly stale data";
    }
}

}  // namespace
