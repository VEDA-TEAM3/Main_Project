#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "core/AppConfig.h"
#include "domain/WorldObservation.h"
#include "transform/AffineLocalToWorldTransform.h"

namespace {

using Clock = std::chrono::steady_clock;

std::vector<CameraCalibration> makeCalibrations(int channelCount) {
    std::vector<CameraCalibration> calibrations;
    calibrations.reserve(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        calibrations.push_back(
            {channel, channel * 2.5 - 10.0, channel * -1.75 + 5.0, channel * 23.0 + 7.5,
             (channel % 2 == 0) ? -1 : 1});
    }
    return calibrations;
}

std::vector<veda::TopViewFrame> makeFrames(int channelCount, int objectsPerFrame) {
    std::vector<veda::TopViewFrame> frames(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        auto& frame = frames[static_cast<std::size_t>(channel)];
        frame.ts = 1000 + channel;
        frame.ch = channel;
        frame.objects.reserve(static_cast<std::size_t>(objectsPerFrame));
        for (int object = 0; object < objectsPerFrame; ++object) {
            frame.objects.push_back(
                {object + 1, (object % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle,
                 {static_cast<double>(object % 16) * 0.35 - 2.0,
                  static_cast<double>(object / 16) * 0.45 + 1.0},
                 false});
        }
    }
    return frames;
}

double measureMedianNsPerCall(int channelCount, int objectsPerFrame, bool boundsEnabled, int repetitions) {
    constexpr int kTrials = 9;
    std::vector<double> samples;
    samples.reserve(kTrials);

    const auto calibrations = makeCalibrations(channelCount);
    const auto frames = makeFrames(channelCount, objectsPerFrame);
    const WorldBounds bounds{boundsEnabled, -1000.0, 1000.0, -1000.0, 1000.0};

    for (int trial = 0; trial < kTrials; ++trial) {
        AffineLocalToWorldTransform transform(calibrations, true, bounds);
        std::vector<domain::ObservationFrame> output;
        for (int warmup = 0; warmup < 50; ++warmup) {
            transform.transform(frames, output);
        }

        const auto begin = Clock::now();
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            transform.transform(frames, output);
        }
        const auto end = Clock::now();

        std::uint64_t checksum = output.size();
        for (const auto& frame : output) {
            checksum += frame.objects.size();
            for (const auto& object : frame.objects) {
                checksum ^= static_cast<std::uint64_t>(object.id);
            }
        }
        if (checksum == 0 && !output.empty()) {
            std::abort();
        }

        const double elapsedNs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        samples.push_back(elapsedNs / static_cast<double>(repetitions));
    }

    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

}  // namespace

int main() {
    struct Case {
        int channels;
        int objectsPerFrame;
        bool boundsEnabled;
        int repetitions;
    };
    const std::vector<Case> cases = {
        {4, 0, false, 100'000},
        {4, 1, false, 80'000},
        {4, 8, false, 30'000},
        {4, 32, false, 10'000},
        {4, 128, false, 2'500},
        {4, 32, true, 10'000},
        {64, 8, false, 2'500},
    };

    std::cout << "channels,objects_per_frame,total_objects,bounds_enabled,ns_per_call\n";
    std::cout << std::fixed << std::setprecision(2);
    for (const auto& benchmark : cases) {
        const double time = measureMedianNsPerCall(benchmark.channels, benchmark.objectsPerFrame,
                                                   benchmark.boundsEnabled, benchmark.repetitions);
        std::cout << benchmark.channels << ',' << benchmark.objectsPerFrame << ','
                  << benchmark.channels * benchmark.objectsPerFrame << ','
                  << (benchmark.boundsEnabled ? "true" : "false") << ',' << time << '\n';
    }
}
