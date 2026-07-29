#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "fuse/GridFuser.h"
#include "metric/EuclideanMetric.h"

namespace {

using Clock = std::chrono::steady_clock;

struct StabilityStats {
    double rmsPositionError = 0.0;
    double rmsFrameStep = 0.0;
    double maxFrameStep = 0.0;
};

std::vector<domain::ObservationFrame> makeSingleObjectFrame(veda::TimestampMs timestamp, double x, double y) {
    return {{timestamp, 0, {{1, veda::ObjectClass::Human, {x, y}}}}};
}

StabilityStats measureStationaryStability(double radius) {
    constexpr int kFrames = 5'000;
    constexpr double kTrueX = 25.0;
    constexpr double kTrueY = -10.0;

    auto metric = std::make_shared<EuclideanMetric>();
    GridFuser fuser(metric, 0.75, 2.0, radius);
    std::mt19937_64 random(0x53544142494C495AULL);
    std::uniform_real_distribution<double> jitter(-0.20, 0.20);

    double squaredErrorSum = 0.0;
    double squaredStepSum = 0.0;
    double maxStep = 0.0;
    domain::WorldPoint previous;

    for (int frameIndex = 0; frameIndex < kFrames; ++frameIndex) {
        const auto result =
            fuser.fuse(makeSingleObjectFrame(frameIndex * 100, kTrueX + jitter(random), kTrueY + jitter(random)));
        if (result.objects.size() != 1)
            std::abort();

        const auto& position = result.objects.front().pos;
        const double error = std::hypot(position.x - kTrueX, position.y - kTrueY);
        squaredErrorSum += error * error;

        if (frameIndex > 0) {
            const double step = std::hypot(position.x - previous.x, position.y - previous.y);
            squaredStepSum += step * step;
            maxStep = std::max(maxStep, step);
        }
        previous = position;
    }

    return {
        std::sqrt(squaredErrorSum / static_cast<double>(kFrames)),
        std::sqrt(squaredStepSum / static_cast<double>(kFrames - 1)),
        maxStep,
    };
}

std::vector<domain::ObservationFrame> makeObjectFrame(std::size_t objectCount, double jitter) {
    domain::ObservationFrame frame;
    frame.ts = 1000;
    frame.ch = 0;
    frame.objects.reserve(objectCount);
    for (std::size_t index = 0; index < objectCount; ++index) {
        const double baseX = static_cast<double>(index % 32) * 3.0;
        const double baseY = static_cast<double>(index / 32) * 3.0;
        frame.objects.push_back({static_cast<veda::ObjectId>(index + 1), veda::ObjectClass::Human,
                                 {baseX + jitter, baseY - jitter}});
    }
    return {std::move(frame)};
}

struct LatencyStats {
    double disabledNs = 0.0;
    double enabledNs = 0.0;
    double pairedOverheadPercent = 0.0;
};

double measureNsPerFrame(GridFuser& fuser, const std::vector<domain::ObservationFrame>& frameA,
                         const std::vector<domain::ObservationFrame>& frameB, int repetitions) {
    std::uint64_t checksum = 0;
    const auto begin = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto result = fuser.fuse((repetition & 1) == 0 ? frameA : frameB);
        checksum += static_cast<std::uint64_t>(result.objects.size());
        if (!result.objects.empty())
            checksum ^= static_cast<std::uint64_t>(std::llround(result.objects.front().pos.x * 1'000'000.0));
    }
    const auto end = Clock::now();

    if (checksum == 0)
        std::abort();

    const double elapsedNs =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    return elapsedNs / static_cast<double>(repetitions);
}

LatencyStats measurePairedLatency(std::size_t objectCount, int repetitions) {
    constexpr int kTrials = 9;
    std::vector<double> disabledSamples;
    std::vector<double> enabledSamples;
    std::vector<double> overheadSamples;
    disabledSamples.reserve(kTrials);
    enabledSamples.reserve(kTrials);
    overheadSamples.reserve(kTrials);
    const auto frameA = makeObjectFrame(objectCount, -0.05);
    const auto frameB = makeObjectFrame(objectCount, 0.05);

    for (int trial = 0; trial < kTrials; ++trial) {
        auto metric = std::make_shared<EuclideanMetric>();
        GridFuser disabled(metric, 0.75, 2.0, 0.0);
        GridFuser enabled(metric, 0.75, 2.0, 0.15);

        for (int warmup = 0; warmup < 100; ++warmup) {
            const auto& frame = (warmup & 1) == 0 ? frameA : frameB;
            disabled.fuse(frame);
            enabled.fuse(frame);
        }

        double disabledNs = 0.0;
        double enabledNs = 0.0;
        if ((trial & 1) == 0) {
            disabledNs = measureNsPerFrame(disabled, frameA, frameB, repetitions);
            enabledNs = measureNsPerFrame(enabled, frameA, frameB, repetitions);
        } else {
            enabledNs = measureNsPerFrame(enabled, frameA, frameB, repetitions);
            disabledNs = measureNsPerFrame(disabled, frameA, frameB, repetitions);
        }

        disabledSamples.push_back(disabledNs);
        enabledSamples.push_back(enabledNs);
        overheadSamples.push_back((enabledNs / disabledNs - 1.0) * 100.0);
    }

    std::sort(disabledSamples.begin(), disabledSamples.end());
    std::sort(enabledSamples.begin(), enabledSamples.end());
    std::sort(overheadSamples.begin(), overheadSamples.end());
    return {
        disabledSamples[disabledSamples.size() / 2],
        enabledSamples[enabledSamples.size() / 2],
        overheadSamples[overheadSamples.size() / 2],
    };
}

}  // namespace

int main() {
    constexpr double kEnabledRadius = 0.15;
    const StabilityStats disabled = measureStationaryStability(0.0);
    const StabilityStats enabled = measureStationaryStability(kEnabledRadius);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "mode,rms_position_error_m,rms_frame_step_m,max_frame_step_m\n";
    std::cout << "disabled," << disabled.rmsPositionError << ',' << disabled.rmsFrameStep << ','
              << disabled.maxFrameStep << '\n';
    std::cout << "enabled," << enabled.rmsPositionError << ',' << enabled.rmsFrameStep << ',' << enabled.maxFrameStep
              << '\n';

    struct LatencyCase {
        std::size_t objects;
        int repetitions;
    };
    const std::vector<LatencyCase> cases = {
        {1, 5'000},
        {64, 300},
        {256, 50},
    };

    std::cout << "objects,disabled_ns_per_frame,enabled_ns_per_frame,paired_overhead_percent\n";
    for (const auto& benchmark : cases) {
        const LatencyStats latency = measurePairedLatency(benchmark.objects, benchmark.repetitions);
        std::cout << benchmark.objects << ',' << latency.disabledNs << ',' << latency.enabledNs << ','
                  << latency.pairedOverheadPercent << '\n';
    }
}
