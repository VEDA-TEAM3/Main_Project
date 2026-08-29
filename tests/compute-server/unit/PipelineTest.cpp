/**
 * @file    PipelineTest.cpp
 * @brief   compute-server Parser-to-Sink 처리 경로 회귀 테스트
 */

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Pipeline.h"
#include "ground/BottomCenterExtractor.h"
#include "route/ParentBasedRouter.h"

class PipelineTestPeer {
public:
    static std::uint64_t sampleCount(const Pipeline& pipeline) { return pipeline.pipelineSampleCount_; }
};

namespace {

class StubParser : public IMetadataParser {
public:
    domain::ChannelFrame frame;

    domain::ChannelFrame parse(const domain::RawPacket&) override { return frame; }
};

class PassthroughSanitizer : public IObjectSanitizer {
public:
    domain::ChannelFrame sanitize(domain::ChannelFrame frame) override { return frame; }
};

class RecordingMapper : public IImageCoordinateMapper {
public:
    void map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const override {
        called = true;
        seenChannel = channelId;
        for (auto& object : objects) {
            object.box.l += 0.1;
            object.box.r += 0.1;
        }
        if (dropAll) {
            objects.clear();
        }
    }

    mutable bool called = false;
    mutable veda::ChannelId seenChannel = -1;
    bool dropAll = false;
};

class RecordingTransform : public ICoordinateTransform {
public:
    std::optional<veda::LocalPoint> toLocal(const domain::ImagePoint& point) override {
        seen.push_back(point);
        if (fail) {
            return std::nullopt;
        }
        return veda::LocalPoint{point.u * 10.0, point.v * 10.0};
    }

    std::vector<domain::ImagePoint> seen;
    bool fail = false;
};

template <typename Frame>
class CaptureSink : public ISink<Frame> {
public:
    CaptureSink(std::string name, std::vector<std::string>& order) : name_(std::move(name)), order_(order) {}

    void send(const Frame& frame) override {
        frames.push_back(frame);
        order_.push_back(name_);
    }

    std::vector<Frame> frames;

private:
    std::string name_;
    std::vector<std::string>& order_;
};

domain::DetectedObject makeObject(veda::ObjectId id, veda::ObjectClass cls, const domain::NormBox& box) {
    domain::DetectedObject object;
    object.id = id;
    object.cls = cls;
    object.box = box;
    return object;
}

class PipelineTest : public ::testing::Test {
protected:
    std::uint64_t run(const domain::ChannelFrame& frame,
                      RiskEdgePolicy edgePolicy = RiskEdgePolicy::DropBottomTruncated) {
        parser->frame = frame;
        PipelineOptions options;
        options.edgePolicy = edgePolicy;
        Pipeline pipeline(parser, mapper, sanitizer, router, ground, transform, riskSink, blurSink, options);
        pipeline.onPacket(domain::RawPacket{});
        return PipelineTestPeer::sampleCount(pipeline);
    }

    std::vector<std::string> sendOrder;
    std::shared_ptr<StubParser> parser = std::make_shared<StubParser>();
    std::shared_ptr<RecordingMapper> mapper = std::make_shared<RecordingMapper>();
    std::shared_ptr<PassthroughSanitizer> sanitizer = std::make_shared<PassthroughSanitizer>();
    std::shared_ptr<ParentBasedRouter> router = std::make_shared<ParentBasedRouter>();
    std::shared_ptr<BottomCenterExtractor> ground = std::make_shared<BottomCenterExtractor>();
    std::shared_ptr<RecordingTransform> transform = std::make_shared<RecordingTransform>();
    std::shared_ptr<CaptureSink<veda::TopViewFrame>> riskSink =
        std::make_shared<CaptureSink<veda::TopViewFrame>>("risk", sendOrder);
    std::shared_ptr<CaptureSink<veda::BlurFrame>> blurSink =
        std::make_shared<CaptureSink<veda::BlurFrame>>("blur", sendOrder);
};

}  // namespace

TEST_F(PipelineTest, RoutesRiskAndBlurAndPreservesFrameIdentity) {
    domain::ChannelFrame frame;
    frame.utcTime = 1787000000123;
    frame.channelId = 7;
    frame.objects = {
        makeObject(11, veda::ObjectClass::Vehicle, {0.2, 0.3, 0.6, 0.9}),
        makeObject(12, veda::ObjectClass::LicensePlate, {0.3, 0.4, 0.5, 0.6}),
    };

    const auto sampleCount = run(frame);

    EXPECT_EQ(sampleCount, 1U);
    ASSERT_EQ(riskSink->frames.size(), 1U);
    ASSERT_EQ(blurSink->frames.size(), 1U);
    EXPECT_EQ(riskSink->frames.front().ts, frame.utcTime);
    EXPECT_EQ(riskSink->frames.front().ch, frame.channelId);
    ASSERT_EQ(riskSink->frames.front().objects.size(), 1U);
    EXPECT_EQ(riskSink->frames.front().objects.front().id, 11);
    EXPECT_EQ(riskSink->frames.front().objects.front().cls, veda::ObjectClass::Vehicle);
    EXPECT_EQ(blurSink->frames.front().ts, frame.utcTime);
    EXPECT_EQ(blurSink->frames.front().ch, frame.channelId);
    ASSERT_EQ(blurSink->frames.front().blurs.size(), 1U);
    EXPECT_EQ(blurSink->frames.front().blurs.front().id, 12);
}

TEST_F(PipelineTest, BlurMappingNeverChangesRiskGroundPoint) {
    domain::ChannelFrame frame;
    frame.channelId = 3;
    frame.objects = {
        makeObject(1, veda::ObjectClass::Human, {0.2, 0.3, 0.6, 0.9}),
        makeObject(2, veda::ObjectClass::Head, {0.2, 0.3, 0.6, 0.9}),
    };

    run(frame);

    ASSERT_EQ(transform->seen.size(), 1U);
    EXPECT_DOUBLE_EQ(transform->seen.front().u, 0.4);
    EXPECT_DOUBLE_EQ(transform->seen.front().v, 0.9);
    ASSERT_EQ(riskSink->frames.front().objects.size(), 1U);
    EXPECT_DOUBLE_EQ(riskSink->frames.front().objects.front().pos.x, 4.0);
    EXPECT_DOUBLE_EQ(riskSink->frames.front().objects.front().pos.y, 9.0);
    ASSERT_EQ(blurSink->frames.front().blurs.size(), 1U);
    EXPECT_DOUBLE_EQ(blurSink->frames.front().blurs.front().box.l, 0.3);
    EXPECT_DOUBLE_EQ(blurSink->frames.front().blurs.front().box.r, 0.7);
    EXPECT_TRUE(mapper->called);
    EXPECT_EQ(mapper->seenChannel, 3);
}

TEST_F(PipelineTest, DefaultPolicyDropsOnlyBottomTruncatedRisk) {
    auto bottom = makeObject(1, veda::ObjectClass::Human, {0.1, 0.1, 0.3, 1.0});
    bottom.touchesBorder = true;
    bottom.bottomTruncated = true;
    auto side = makeObject(2, veda::ObjectClass::Vehicle, {0.0, 0.2, 0.4, 0.8});
    side.touchesBorder = true;

    domain::ChannelFrame frame;
    frame.objects = {bottom, side};
    run(frame);

    ASSERT_EQ(riskSink->frames.front().objects.size(), 1U);
    EXPECT_EQ(riskSink->frames.front().objects.front().id, 2);
}

TEST_F(PipelineTest, DropAnyEdgeRejectsSideTruncation) {
    auto side = makeObject(2, veda::ObjectClass::Vehicle, {0.0, 0.2, 0.4, 0.8});
    side.touchesBorder = true;

    domain::ChannelFrame frame;
    frame.objects = {side};
    run(frame, RiskEdgePolicy::DropAnyEdge);

    EXPECT_TRUE(riskSink->frames.front().objects.empty());
    EXPECT_TRUE(transform->seen.empty());
}

TEST_F(PipelineTest, KeepPolicyPassesBottomTruncatedRiskWithEdgeFlag) {
    auto bottom = makeObject(1, veda::ObjectClass::Human, {0.1, 0.1, 0.3, 1.0});
    bottom.touchesBorder = true;
    bottom.bottomTruncated = true;

    domain::ChannelFrame frame;
    frame.objects = {bottom};
    run(frame, RiskEdgePolicy::Keep);

    ASSERT_EQ(riskSink->frames.front().objects.size(), 1U);
    EXPECT_TRUE(riskSink->frames.front().objects.front().edge);
}

TEST_F(PipelineTest, FailedCoordinateTransformDropsRiskButStillPublishesBlur) {
    transform->fail = true;
    domain::ChannelFrame frame;
    frame.objects = {
        makeObject(1, veda::ObjectClass::Human, {0.2, 0.3, 0.6, 0.9}),
        makeObject(2, veda::ObjectClass::Head, {0.2, 0.3, 0.6, 0.9}),
    };

    run(frame);

    EXPECT_TRUE(riskSink->frames.front().objects.empty());
    ASSERT_EQ(blurSink->frames.front().blurs.size(), 1U);
    EXPECT_EQ(blurSink->frames.front().blurs.front().id, 2);
}

TEST_F(PipelineTest, MapperMayFilterBlurWithoutAffectingRisk) {
    mapper->dropAll = true;
    domain::ChannelFrame frame;
    frame.objects = {
        makeObject(1, veda::ObjectClass::Human, {0.2, 0.3, 0.6, 0.9}),
        makeObject(2, veda::ObjectClass::Head, {0.2, 0.3, 0.6, 0.9}),
    };

    run(frame);

    ASSERT_EQ(riskSink->frames.front().objects.size(), 1U);
    EXPECT_TRUE(blurSink->frames.front().blurs.empty());
}

TEST_F(PipelineTest, EmptyFramePublishesExplicitEmptySnapshots) {
    domain::ChannelFrame frame;
    frame.utcTime = 1787000000999;
    frame.channelId = 5;

    run(frame);

    ASSERT_EQ(riskSink->frames.size(), 1U);
    ASSERT_EQ(blurSink->frames.size(), 1U);
    EXPECT_TRUE(riskSink->frames.front().objects.empty());
    EXPECT_TRUE(blurSink->frames.front().blurs.empty());
    EXPECT_EQ(riskSink->frames.front().ts, frame.utcTime);
    EXPECT_EQ(blurSink->frames.front().ts, frame.utcTime);
}

TEST_F(PipelineTest, SafetyCriticalRiskSnapshotIsSentBeforeBlurSnapshot) {
    domain::ChannelFrame frame;
    run(frame);

    ASSERT_EQ(sendOrder.size(), 2U);
    EXPECT_EQ(sendOrder[0], "risk");
    EXPECT_EQ(sendOrder[1], "blur");
}
