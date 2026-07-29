#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "../tests/control-server/FuserTestSupport.h"
#include "fuse/ConcatFuser.h"
#include "fuse/GridFuser.h"
#include "metric/EuclideanMetric.h"

namespace {

using Clock = std::chrono::steady_clock;

template <typename Fuser>
double measureMedianNsPerFrame(const std::vector<domain::ObservationFrame>& frames, int repetitions) {
    constexpr int kTrials = 9;
    std::vector<double> samples;
    samples.reserve(kTrials);

    for (int trial = 0; trial < kTrials; ++trial) {
        auto metric = std::make_shared<EuclideanMetric>();
        Fuser fuser(metric, 0.75, 0.0);
        std::size_t checksum = 0;

        for (int warmup = 0; warmup < 20; ++warmup)
            checksum += fuser.fuse(frames).objects.size();

        const auto begin = Clock::now();
        for (int repetition = 0; repetition < repetitions; ++repetition)
            checksum += fuser.fuse(frames).objects.size();
        const auto end = Clock::now();

        if (checksum == 0)
            std::abort();
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
        auto frames = fuser_test::makeRandomFrames(random, 10'000 + window * 100);
        const auto expected = baseline.fuse(frames);
        const auto actual = optimized.fuse(frames);
        const std::string difference = fuser_test::compareFrames(expected, actual);
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
    if (!verifyEquivalence())
        return 1;

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
        const auto frames = fuser_test::makeOverlappingFrames(benchmark.totalObjects, 4);
        const double baseline = measureMedianNsPerFrame<ConcatFuser>(frames, benchmark.repetitions);
        const double optimized = measureMedianNsPerFrame<GridFuser>(frames, benchmark.repetitions);
        const double speedup = baseline / optimized;
        const double reduction = (1.0 - optimized / baseline) * 100.0;
        std::cout << benchmark.totalObjects << ',' << benchmark.totalObjects / 4 << ',' << baseline << ','
                  << optimized << ',' << speedup << ',' << reduction << '\n';
    }
    return 0;
}
