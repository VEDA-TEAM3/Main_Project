/**
 * @file    PipelineTrajectoryTest.cpp
 * @brief   [임시 진단] 합성 등속 궤적으로 서버 파이프라인의 좌표 왜곡/프레임 드랍 격리
 *
 * @details
 * UI 끊김의 원인이 서버 파이프라인인지 Qt 렌더링인지 가르기 위한 통합 테스트다.
 * Aggregator -> Transform -> Fuser -> RiskPolicy 를 실제 구현으로 엮고,
 * 완벽한 등속 직선 운동(200ms 마다 +0.5m, +0.2m)을 50프레임 주입해 출력을 추적한다.
 *
 * 확인 항목:
 *   1. gid 가 전 구간 동일한가        -> 아니면 트래킹 끊김(객체 파괴/재생성)
 *   2. 출력 간 delta_d 가 일정한가     -> 아니면 좌표 왜곡/지터
 *   3. 입력 대비 출력 프레임 수         -> 집계 윈도우에 의한 데시메이션 정량화
 *
 * @note ZoneMapper 는 순수 (x,y)->zoneId 함수라 좌표를 바꾸지 않으므로 체인에서 제외하고
 *       zoneId 를 직접 넣는다 (RiskPolicy 의 zone 집계 경로만 만족시키기 위함).
 * @note 진단용 임시 테스트다. 원인 규명이 끝나면 이 파일과 CMake 등록을 함께 지운다.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "Logger.h"
#include "aggregate/TimeWindowAggregatorV2.h"
#include "core/AppConfig.h"
#include "fuse/ConcatFuser.h"
#include "metric/EuclideanMetric.h"
#include "risk/ThresholdRiskPolicy.h"
#include "transform/AffineLocalToWorldTransform.h"

namespace {

constexpr veda::TimestampMs kTsrcMs = 200;  ///< 소스 프레임 주기 (compute-server 실측 201.6ms 근사)
constexpr double kStepX = 0.5;              ///< T_src 당 X 이동량 (m)
constexpr double kStepY = 0.2;              ///< T_src 당 Y 이동량 (m)
constexpr int kFrameCount = 50;
constexpr veda::ChannelId kChannel = 0;
constexpr veda::ObjectId kObjectId = 7;  ///< 카메라 내부 추적 ID (전 구간 안정)

/// @brief 등속 직선 운동 1스텝의 이동 거리 = sqrt(0.5^2 + 0.2^2)
const double kExpectedStepDist = std::sqrt(kStepX * kStepX + kStepY * kStepY);

class FakeClock final : public IClock {
public:
    veda::TimestampMs now() const override { return t_; }
    void set(veda::TimestampMs t) { t_ = t; }

private:
    veda::TimestampMs t_ = 0;
};

struct LoggerOff {
    LoggerOff() {
        LogConfig c;
        c.level = LogLevel::Off;
        c.console = false;
        c.file = false;
        initLogger(c);
    }
};
const LoggerOff g_loggerOff;

/// @brief 파이프라인이 실제로 내보낸 한 프레임
struct Emitted {
    veda::TimestampMs ts = 0;
    veda::GlobalId gid = 0;
    double x = 0.0;
    double y = 0.0;
};

/// @brief 항등 월드 변환용 캘리브레이션 (facingAngleDeg=0, lateralSign=+1 이면 x->x, y->y)
std::vector<CameraCalibration> identityCalibration() {
    CameraCalibration cal;
    cal.channelId = kChannel;
    cal.cameraPosX = 0.0;
    cal.cameraPosY = 0.0;
    cal.facingAngleDeg = 0.0;
    cal.lateralSign = 1;
    return {cal};
}

/**
 * @brief   합성 궤적을 파이프라인에 주입하고 출력된 프레임들을 수집
 * @param   windowSizeMs      집계 윈도우 (ms)
 * @param   trackMaxDistance  gid 승계 최대 이동 거리 (m)
 */
std::vector<Emitted> runTrajectory(std::uint64_t windowSizeMs, double trackMaxDistance) {
    auto clock = std::make_shared<FakeClock>();
    auto metric = std::make_shared<EuclideanMetric>();

    TimeWindowAggregatorV2 aggregator(clock, windowSizeMs, /*channelCount=*/1);
    AffineLocalToWorldTransform transform(identityCalibration(), /*dropUncalibrated=*/true, WorldBounds{});
    ConcatFuser fuser(metric, /*dedupMergeDistance=*/1.0, trackMaxDistance);

    RiskConfig riskCfg;
    riskCfg.warningDistance = 5.0;
    riskCfg.dangerousDistance = 2.0;
    ThresholdRiskPolicy risk(metric, riskCfg, /*channelCount=*/1);

    std::vector<Emitted> emitted;
    std::vector<domain::ObservationFrame> observations;
    domain::RiskEvaluation riskEval;

    // Controller::processPipeline 과 동일한 순서로 다운스트림을 엮는다
    aggregator.setCallback([&](const IFrameAggregator::AggregatedFrames& frames) {
        transform.transform(frames, observations);
        domain::WorldFrame world = fuser.fuse(observations);

        // ZoneMapper 대역 (좌표 불변, RiskPolicy 의 zone 집계 경로만 만족)
        for (auto& o : world.objects) {
            o.zoneId = kChannel;
        }
        risk.evaluate(world, riskEval);

        for (const auto& o : world.objects) {
            emitted.push_back(Emitted{world.timestamp, o.gid, o.pos.x, o.pos.y});
        }
    });

    for (int i = 0; i < kFrameCount; ++i) {
        const veda::TimestampMs t = static_cast<veda::TimestampMs>(i) * kTsrcMs;
        clock->set(t);

        veda::TopViewFrame f;
        f.v = veda::kSchemaVersion;
        f.ts = t;
        f.ch = kChannel;
        f.objects.resize(1);
        f.objects[0].id = kObjectId;
        f.objects[0].cls = veda::ObjectClass::Vehicle;
        f.objects[0].pos.x = kStepX * static_cast<double>(i);
        f.objects[0].pos.y = kStepY * static_cast<double>(i);
        f.objects[0].edge = false;

        aggregator.push(f);
    }
    return emitted;
}

/// @brief 출력 궤적을 표로 찍고 delta 통계를 돌려준다
struct DeltaStats {
    double minDelta = 0.0;
    double maxDelta = 0.0;
    std::size_t gidChanges = 0;
};

DeltaStats traceAndSummarize(const char* label, const std::vector<Emitted>& out) {
    std::printf("\n=== %s ===\n", label);
    std::printf("입력 %d프레임 (T_src=%lldms, 스텝 dx=%.1f dy=%.1f, 스텝거리=%.4fm)\n", kFrameCount,
                static_cast<long long>(kTsrcMs), kStepX, kStepY, kExpectedStepDist);
    std::printf("출력 %zu프레임\n\n", out.size());
    std::printf("  #  ts(ms)   gid        x        y     dx      dy   delta_d\n");
    std::printf("---- ------- ----- -------- -------- ------- ------- --------\n");

    DeltaStats st;
    st.minDelta = 1e300;
    st.maxDelta = -1e300;

    for (std::size_t i = 0; i < out.size(); ++i) {
        double dx = 0.0, dy = 0.0, dd = 0.0;
        if (i > 0) {
            dx = out[i].x - out[i - 1].x;
            dy = out[i].y - out[i - 1].y;
            dd = std::sqrt(dx * dx + dy * dy);
            st.minDelta = std::min(st.minDelta, dd);
            st.maxDelta = std::max(st.maxDelta, dd);
            if (out[i].gid != out[i - 1].gid) ++st.gidChanges;
        }
        std::printf("%4zu %7lld %5llu %8.3f %8.3f %7.3f %7.3f %8.4f%s\n", i,
                    static_cast<long long>(out[i].ts), static_cast<unsigned long long>(out[i].gid), out[i].x, out[i].y,
                    dx, dy, dd, (i > 0 && out[i].gid != out[i - 1].gid) ? "  <-- GID CHANGED" : "");
    }

    if (out.size() < 2) {
        st.minDelta = st.maxDelta = 0.0;
        return st;
    }
    std::printf("\n  delta_d  min=%.6f  max=%.6f  spread=%.3e   gid 변경 %zu회\n", st.minDelta, st.maxDelta,
                st.maxDelta - st.minDelta, st.gidChanges);
    std::printf("  입력 대비 출력 비율 = %zu/%d = %.1f%%  (데시메이션 %.1f%%)\n", out.size(), kFrameCount,
                100.0 * static_cast<double>(out.size()) / kFrameCount,
                100.0 * (1.0 - static_cast<double>(out.size()) / kFrameCount));
    return st;
}

}  // namespace

// ============================================================================
// 1. 현재 프로덕션 기본값 (windowSizeMs=220, trackMaxDistance=4.0)
// ============================================================================

TEST(PipelineTrajectoryTest, ConstantVelocityProducesUniformStepsAtCurrentDefaults) {
    const auto out = runTrajectory(/*windowSizeMs=*/220, /*trackMaxDistance=*/4.0);
    const auto st = traceAndSummarize("현재 기본값  W=220ms  trackMaxDistance=4.0m", out);

    ASSERT_GE(out.size(), 2u) << "출력이 2프레임 미만 — 파이프라인이 아무것도 내보내지 못했다";

    // (1) gid churn 없음 -- 있으면 UI 보간이 원리적으로 불가능하다
    EXPECT_EQ(st.gidChanges, 0u) << "gid 가 도중에 바뀌었다 = 객체가 파괴/재생성되고 있다";

    // (2) 출력 간 이동 거리가 일정 -- 좌표 왜곡이나 지터가 없음
    EXPECT_NEAR(st.maxDelta - st.minDelta, 0.0, 1e-9) << "출력 간 delta_d 가 흔들린다 = 파이프라인이 좌표를 왜곡한다";

    // (3) 서버가 좌표를 '변형'하지 않았는지 -- 항등 변환이므로 입력 좌표와 정확히 같아야 한다
    for (const auto& e : out) {
        const double k = e.x / kStepX;  // 몇 번째 소스 프레임인지
        EXPECT_NEAR(e.y, kStepY * k, 1e-9) << "x/y 비율이 깨졌다 = 변환 단계가 좌표를 왜곡했다";
    }
}

// ============================================================================
// 2. 이전 기본값 (windowSizeMs=100, trackMaxDistance=2.0) — 회귀 비교용
// ============================================================================

TEST(PipelineTrajectoryTest, PreviousDefaultsForComparison) {
    const auto out = runTrajectory(/*windowSizeMs=*/100, /*trackMaxDistance=*/2.0);
    const auto st = traceAndSummarize("이전 기본값  W=100ms  trackMaxDistance=2.0m", out);

    ASSERT_GE(out.size(), 2u);
    // 비교 목적이므로 단정하지 않고 수치만 남긴다 (트레이스가 판단 근거)
    SUCCEED() << "delta spread=" << (st.maxDelta - st.minDelta) << " gid 변경=" << st.gidChanges;
}

// ============================================================================
// 3. 4채널 스태거 — 실제 배포 조건 (CCTV 1대 = 4채널이 같은 객체를 본다)
// ============================================================================

TEST(PipelineTrajectoryTest, FourStaggeredChannelsMatchProductionConditions) {
    // 단일 채널 결과만 보면 오해한다 -- 윈도우 마감은 push 구동이라, 다른 채널의 push 가
    // 마감을 앞당긴다. 실제 배포(4채널 x 5fps, 채널당 50ms 스태거)를 모사한다.
    constexpr int kChannels = 4;
    constexpr veda::TimestampMs kStaggerMs = 50;

    auto clock = std::make_shared<FakeClock>();
    auto metric = std::make_shared<EuclideanMetric>();

    TimeWindowAggregatorV2 aggregator(clock, /*windowSizeMs=*/220, kChannels);

    std::vector<CameraCalibration> cals;
    for (int ch = 0; ch < kChannels; ++ch) {
        CameraCalibration c;
        c.channelId = static_cast<veda::ChannelId>(ch);
        c.cameraPosX = 0.0;
        c.cameraPosY = 0.0;
        c.facingAngleDeg = 0.0;
        c.lateralSign = 1;
        cals.push_back(c);
    }
    AffineLocalToWorldTransform transform(cals, true, WorldBounds{});
    ConcatFuser fuser(metric, /*dedupMergeDistance=*/1.0, /*trackMaxDistance=*/4.0);

    RiskConfig riskCfg;
    riskCfg.warningDistance = 5.0;
    riskCfg.dangerousDistance = 2.0;
    ThresholdRiskPolicy risk(metric, riskCfg, kChannels);

    std::vector<Emitted> emitted;
    std::vector<domain::ObservationFrame> observations;
    domain::RiskEvaluation riskEval;

    aggregator.setCallback([&](const IFrameAggregator::AggregatedFrames& frames) {
        transform.transform(frames, observations);
        domain::WorldFrame world = fuser.fuse(observations);
        for (auto& o : world.objects) o.zoneId = 0;
        risk.evaluate(world, riskEval);
        for (const auto& o : world.objects) emitted.push_back(Emitted{world.timestamp, o.gid, o.pos.x, o.pos.y});
    });

    // 채널별로 kStaggerMs 씩 어긋난 시각에, 각자 T_src 주기로 같은 객체를 관측
    struct Push {
        veda::TimestampMs t;
        veda::ChannelId ch;
    };
    std::vector<Push> schedule;
    for (int i = 0; i < kFrameCount; ++i) {
        for (int ch = 0; ch < kChannels; ++ch) {
            schedule.push_back(Push{static_cast<veda::TimestampMs>(i) * kTsrcMs +
                                        static_cast<veda::TimestampMs>(ch) * kStaggerMs,
                                    static_cast<veda::ChannelId>(ch)});
        }
    }
    std::sort(schedule.begin(), schedule.end(), [](const Push& a, const Push& b) { return a.t < b.t; });

    for (const auto& p : schedule) {
        clock->set(p.t);
        // 객체의 '실제' 위치는 시각 t 의 함수 -- 채널마다 관측 시각이 다르므로 좌표도 다르다
        const double k = static_cast<double>(p.t) / static_cast<double>(kTsrcMs);
        veda::TopViewFrame f;
        f.v = veda::kSchemaVersion;
        f.ts = p.t;
        f.ch = p.ch;
        f.objects.resize(1);
        f.objects[0].id = kObjectId;
        f.objects[0].cls = veda::ObjectClass::Vehicle;
        f.objects[0].pos.x = kStepX * k;
        f.objects[0].pos.y = kStepY * k;
        aggregator.push(f);
    }

    const auto st = traceAndSummarize("4채널 스태거  W=220ms  trackMaxDistance=4.0m (배포 조건)", emitted);

    ASSERT_GE(emitted.size(), 2u);
    EXPECT_EQ(st.gidChanges, 0u) << "배포 조건에서 gid churn 이 발생했다";
    EXPECT_LT(st.maxDelta - st.minDelta, 0.35)
        << "출력 간 이동 거리가 크게 흔들린다 = 채널 스태거가 좌표 지터로 새어 나온다";
}

// ============================================================================
// 4. trackMaxDistance 상한 초과 — gid churn 재현 (불변식 위반 시 무슨 일이 나는가)
// ============================================================================

TEST(PipelineTrajectoryTest, ExceedingTrackMaxDistanceCausesGidChurn) {
    // 스텝 거리 0.5385m > trackMaxDistance 0.3m -> 승계 실패가 매 프레임 발생해야 한다.
    // CLAUDE.md 의 trackMaxDistance >= v_max * max(windowSizeMs, T_src) 불변식 위반 재현.
    const auto out = runTrajectory(/*windowSizeMs=*/220, /*trackMaxDistance=*/0.3);
    const auto st = traceAndSummarize("불변식 위반  W=220ms  trackMaxDistance=0.3m", out);

    ASSERT_GE(out.size(), 2u);
    EXPECT_GT(st.gidChanges, 0u) << "불변식을 위반했는데 gid 가 유지됐다 -- 승계 게이트가 사라졌는지 확인할 것";
}
