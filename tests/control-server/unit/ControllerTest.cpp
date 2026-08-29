/**
 * @file ControllerTest.cpp
 * @brief Controller의 무관측 위험도 유지 회귀 테스트
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "core/Controller.h"

class ControllerTestPeer {
public:
    static std::uint64_t sampleCount(const Controller& controller) {
        return controller.pipelineLatencySampleCount_;
    }
};

namespace {

class FakeReceiver final : public IChannelReceiver {
public:
    void setCallback(FrameCallback callback) override { callback_ = std::move(callback); }
    void setAliveCallback(AliveCallback callback) override { aliveCallback_ = std::move(callback); }
    void start() override {}
    void stop() override {}
    void emitAlive(veda::ChannelId channel, bool alive) { aliveCallback_(channel, alive); }

private:
    FrameCallback callback_;
    AliveCallback aliveCallback_;
};

class FakeAggregator final : public IFrameAggregator {
public:
    void setCallback(AggregationCallback callback) override { callback_ = std::move(callback); }
    void push(const veda::TopViewFrame&) override {}
    void emit(const AggregatedFrames& frames) { callback_(frames); }

private:
    AggregationCallback callback_;
};

class FakeTransform final : public ILocalToWorldTransform {
public:
    void transform(const std::vector<veda::TopViewFrame>&, std::vector<domain::ObservationFrame>& out) override {
        out.clear();
    }
};

class SequencedFuser final : public ICrossChannelFuser {
public:
    domain::WorldFrame fuse(const std::vector<domain::ObservationFrame>&) override {
        domain::WorldFrame frame;
        if (callCount++ < 2) {
            domain::WorldObject coasted;
            coasted.gid = 1;
            coasted.cls = veda::ObjectClass::Vehicle;
            frame.objects.push_back(coasted);
        }
        return frame;
    }

private:
    int callCount = 0;
};

class NoopParkingPolicy final : public IParkingPolicy {
public:
    void apply(domain::WorldFrame&) override {}
};

class NoopZoneMapper final : public IZoneMapper {
public:
    void assign(domain::WorldFrame&) override {}
};

class SequencedRiskPolicy final : public IRiskPolicy {
public:
    void evaluate(domain::WorldFrame& frame, domain::RiskEvaluation& out) override {
        ++callCount;
        const auto level = callCount == 1 ? veda::RiskLevel::Danger : veda::RiskLevel::None;
        frame.level = level;
        out.timestamp = frame.timestamp;
        out.zoneLevels = {{0, level, level == veda::RiskLevel::None ? -1.0 : 1.0}};
    }

    int callCount = 0;
};

class RecordingDispatcher final : public IHwEventDispatcher {
public:
    void dispatch(const domain::RiskEvaluation& evaluation) override {
        ++callCount;
        last = evaluation;
        evaluations.push_back(evaluation);
    }
    void emitStatus(veda::ChannelId channel, bool alive, const HwIndicatorState& indicators) {
        statusCallback_(channel, alive, indicators);
    }
    void setStatusCallback(StatusCallback callback) override { statusCallback_ = std::move(callback); }
    void setFaultCallback(FaultCallback callback) override { faultCallback_ = std::move(callback); }

    int callCount = 0;
    domain::RiskEvaluation last;
    std::vector<domain::RiskEvaluation> evaluations;

private:
    StatusCallback statusCallback_;
    FaultCallback faultCallback_;
};

class RecordingSink final : public ISink {
public:
    void send(const domain::WorldFrame& frame) override { frames.push_back(frame); }
    void sendChannelStatus(const veda::ChannelStatus& status) override { statuses.push_back(status); }

    std::vector<domain::WorldFrame> frames;
    std::vector<veda::ChannelStatus> statuses;
};

class FakeClock final : public IClock {
public:
    veda::TimestampMs now() const override { return 1234; }
};

TEST(ControllerTest, CoastedWindowIsPublishedButExcludedFromRisk) {
    auto receiver = std::make_shared<FakeReceiver>();
    auto aggregator = std::make_shared<FakeAggregator>();
    auto transform = std::make_shared<FakeTransform>();
    auto fuser = std::make_shared<SequencedFuser>();
    auto parking = std::make_shared<NoopParkingPolicy>();
    auto zoneMapper = std::make_shared<NoopZoneMapper>();
    auto riskPolicy = std::make_shared<SequencedRiskPolicy>();
    auto dispatcher = std::make_shared<RecordingDispatcher>();
    auto sink = std::make_shared<RecordingSink>();
    auto clock = std::make_shared<FakeClock>();

    Controller controller(receiver, aggregator, transform, fuser, parking, zoneMapper, riskPolicy, dispatcher, sink,
                          clock, 1);

    veda::TopViewFrame observed;
    observed.v = veda::kSchemaVersion;
    observed.ts = 100;
    observed.ch = 0;
    aggregator->emit({observed});
    aggregator->emit({});
    aggregator->emit({});

    ASSERT_EQ(sink->frames.size(), 3U);
    EXPECT_EQ(riskPolicy->callCount, 3);
    EXPECT_EQ(dispatcher->callCount, 3);
    ASSERT_EQ(dispatcher->evaluations.size(), sink->frames.size());
    for (std::size_t i = 0; i < sink->frames.size(); ++i) {
        veda::RiskLevel maximum = veda::RiskLevel::None;
        for (const auto& zone : dispatcher->evaluations[i].zoneLevels) {
            maximum = std::max(maximum, zone.level);
        }
        EXPECT_EQ(sink->frames[i].level, maximum);
    }
    EXPECT_EQ(sink->frames[0].level, veda::RiskLevel::Danger);
    EXPECT_EQ(sink->frames[1].level, veda::RiskLevel::None);
    EXPECT_EQ(sink->frames[1].timestamp, 1234);
    EXPECT_EQ(sink->frames[2].level, veda::RiskLevel::None);
    EXPECT_TRUE(sink->frames[2].objects.empty());
    EXPECT_EQ(ControllerTestPeer::sampleCount(controller), 3U);
    ASSERT_EQ(dispatcher->last.zoneLevels.size(), 1U);
    EXPECT_EQ(dispatcher->last.zoneLevels[0].level, veda::RiskLevel::None);
}

TEST(ControllerTest, PublishesOnlyCameraTransitionsAndPreservesStaleHardwareState) {
    auto receiver = std::make_shared<FakeReceiver>();
    auto aggregator = std::make_shared<FakeAggregator>();
    auto transform = std::make_shared<FakeTransform>();
    auto fuser = std::make_shared<SequencedFuser>();
    auto parking = std::make_shared<NoopParkingPolicy>();
    auto zoneMapper = std::make_shared<NoopZoneMapper>();
    auto riskPolicy = std::make_shared<SequencedRiskPolicy>();
    auto dispatcher = std::make_shared<RecordingDispatcher>();
    auto sink = std::make_shared<RecordingSink>();
    auto clock = std::make_shared<FakeClock>();
    Controller controller(receiver, aggregator, transform, fuser, parking, zoneMapper, riskPolicy, dispatcher, sink,
                          clock, 1);

    receiver->emitAlive(0, false);
    EXPECT_TRUE(sink->statuses.empty());

    receiver->emitAlive(0, true);
    receiver->emitAlive(0, true);
    ASSERT_EQ(sink->statuses.size(), 1U);
    EXPECT_TRUE(sink->statuses.back().cameraAlive);
    EXPECT_FALSE(sink->statuses.back().hardwareAlive);
    EXPECT_EQ(sink->statuses.back().ts, 1234);

    const HwIndicatorState active{true, true, true, false, false};
    dispatcher->emitStatus(0, true, active);
    ASSERT_EQ(sink->statuses.size(), 2U);
    EXPECT_TRUE(sink->statuses.back().cameraAlive);
    EXPECT_TRUE(sink->statuses.back().hardwareAlive);
    EXPECT_TRUE(sink->statuses.back().sirenOn);
    EXPECT_TRUE(sink->statuses.back().ledRed);

    dispatcher->emitStatus(0, false, active);
    ASSERT_EQ(sink->statuses.size(), 3U);
    EXPECT_FALSE(sink->statuses.back().hardwareAlive);
    EXPECT_TRUE(sink->statuses.back().sirenOn);
    EXPECT_TRUE(sink->statuses.back().ledRed);

    receiver->emitAlive(4, true);
    EXPECT_EQ(sink->statuses.size(), 3U);
}

}  // namespace
