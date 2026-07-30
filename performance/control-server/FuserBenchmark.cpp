#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "fuse/ConcatFuser.h"
#include "fuse/GridFuser.h"
#include "metric/EuclideanMetric.h"

namespace {

using Clock = std::chrono::steady_clock;

bool sameDouble(double lhs, double rhs) {
    return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

std::string compareFrames(const domain::WorldFrame& expected, const domain::WorldFrame& actual) {
    if (expected.timestamp != actual.timestamp) {
        return "timestamp mismatch";
    }
    if (expected.level != actual.level) {
        return "frame risk level mismatch";
    }
    if (expected.objects.size() != actual.objects.size()) {
        return "object count mismatch";
    }

    for (std::size_t index = 0; index < expected.objects.size(); ++index) {
        const auto& lhs = expected.objects[index];
        const auto& rhs = actual.objects[index];
        if (lhs.gid != rhs.gid || lhs.cls != rhs.cls || !sameDouble(lhs.pos.x, rhs.pos.x) ||
            !sameDouble(lhs.pos.y, rhs.pos.y) || lhs.riskLevel != rhs.riskLevel ||
            lhs.nearestObj != rhs.nearestObj || !sameDouble(lhs.nearestDist, rhs.nearestDist) ||
            lhs.zoneId != rhs.zoneId || lhs.sourceChannels.count != rhs.sourceChannels.count ||
            lhs.sourceChannels.truncated != rhs.sourceChannels.truncated) {
            return "object[" + std::to_string(index) + "] mismatch";
        }
        for (std::uint8_t channel = 0; channel < lhs.sourceChannels.count; ++channel) {
            if (lhs.sourceChannels.ids[channel] != rhs.sourceChannels.ids[channel]) {
                return "object[" + std::to_string(index) + "] source channel order mismatch";
            }
        }
    }
    return {};
}

std::vector<domain::ObservationFrame> makeOverlappingFrames(std::size_t totalObjects, int channelCount,
                                                            veda::TimestampMs timestamp = 1000) {
    std::vector<domain::ObservationFrame> frames(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        frames[static_cast<std::size_t>(channel)].ch = channel;
        frames[static_cast<std::size_t>(channel)].ts = timestamp + channel;
    }

    const std::size_t entities = totalObjects / static_cast<std::size_t>(channelCount);
    for (std::size_t entity = 0; entity < entities; ++entity) {
        const double baseX = static_cast<double>(entity % 32) * 3.0;
        const double baseY = static_cast<double>(entity / 32) * 3.0;
        for (int channel = 0; channel < channelCount; ++channel) {
            const double offset = static_cast<double>(channel) * 0.03;
            frames[static_cast<std::size_t>(channel)].objects.push_back(
                {static_cast<veda::ObjectId>(entity + 1),
                 (entity % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle,
                 {baseX + offset, baseY - offset}});
        }
    }
    return frames;
}

std::vector<domain::ObservationFrame> makeRandomFrames(std::mt19937_64& random, veda::TimestampMs timestamp) {
    std::uniform_int_distribution<int> channelCountDistribution(1, 8);
    std::uniform_int_distribution<int> objectCountDistribution(0, 24);
    std::uniform_int_distribution<int> classDistribution(0, 2);
    std::uniform_real_distribution<double> coordinateDistribution(-75.0, 75.0);
    std::uniform_real_distribution<double> jitterDistribution(-0.35, 0.35);

    const int channelCount = channelCountDistribution(random);
    std::vector<domain::ObservationFrame> frames;
    frames.reserve(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        domain::ObservationFrame frame;
        frame.ch = channel;
        frame.ts = timestamp + channel;

        const int objectCount = objectCountDistribution(random);
        frame.objects.reserve(static_cast<std::size_t>(objectCount));
        for (int object = 0; object < objectCount; ++object) {
            const int sharedId = object % 8;
            const bool shared = object < 8;
            const double baseX =
                shared ? static_cast<double>(sharedId) * 4.0 : coordinateDistribution(random);
            const double baseY =
                shared ? static_cast<double>(sharedId % 3) * 5.0 : coordinateDistribution(random);
            const auto objectClass =
                classDistribution(random) == 0 ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle;
            frame.objects.push_back(
                {static_cast<veda::ObjectId>(object + 1), objectClass,
                 {baseX + jitterDistribution(random), baseY + jitterDistribution(random)}});
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

template <typename Fuser>
double measureMedianNsPerFrame(const std::vector<domain::ObservationFrame>& frames, int repetitions) {
    constexpr int kTrials = 9;
    std::vector<double> samples;
    samples.reserve(kTrials);

    for (int trial = 0; trial < kTrials; ++trial) {
        auto metric = std::make_shared<EuclideanMetric>();
        Fuser fuser(metric, 0.75, 0.0);
        std::size_t checksum = 0;

        for (int warmup = 0; warmup < 20; ++warmup) {
            checksum += fuser.fuse(frames).objects.size();
        }

        const auto begin = Clock::now();
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            checksum += fuser.fuse(frames).objects.size();
        }
        const auto end = Clock::now();

        if (checksum == 0) {
            std::abort();
        }
        const double elapsedNs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        samples.push_back(elapsedNs / static_cast<double>(repetitions));
    }

    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

bool verifyEquivalence() {
    auto metric = std::make_shared<EuclideanMetric>();
    ConcatFuser baseline(metric, 0.75, 2.0);
    GridFuser optimized(metric, 0.75, 2.0);
    std::mt19937_64 random(0x56454441ULL);

    for (int window = 0; window < 500; ++window) {
        auto frames = makeRandomFrames(random, 10'000 + window * 100);
        const auto expected = baseline.fuse(frames);
        const auto actual = optimized.fuse(frames);
        const std::string difference = compareFrames(expected, actual);
        if (!difference.empty()) {
            std::cerr << "EQUIVALENCE_FAIL window=" << window << " " << difference << '\n';
            return false;
        }
    }
    std::cout << "EQUIVALENCE_PASS windows=500 seed=0x56454441 exact_double_bits=true\n";
    return true;
}

}  // namespace

int main() {
    if (!verifyEquivalence()) {
        return 1;
    }

    struct Case {
        std::size_t totalObjects;
        int repetitions;
    };
    const std::vector<Case> cases = {
        {64, 1800},
        {128, 800},
        {256, 300},
        {512, 100},
        {1024, 30},
    };

    std::cout << "objects,entities,baseline_ns_per_frame,optimized_ns_per_frame,speedup,latency_reduction_pct\n";
    std::cout << std::fixed << std::setprecision(2);
    for (const auto& benchmark : cases) {
        const auto frames = makeOverlappingFrames(benchmark.totalObjects, 4);
        const double baseline = measureMedianNsPerFrame<ConcatFuser>(frames, benchmark.repetitions);
        const double optimized = measureMedianNsPerFrame<GridFuser>(frames, benchmark.repetitions);
        const double speedup = baseline / optimized;
        const double reduction = (1.0 - optimized / baseline) * 100.0;
        std::cout << benchmark.totalObjects << ',' << benchmark.totalObjects / 4 << ',' << baseline << ','
                  << optimized << ',' << speedup << ',' << reduction << '\n';
    }
    return 0;
}
