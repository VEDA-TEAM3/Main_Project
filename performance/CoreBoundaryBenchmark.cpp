#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkFrame {
    int channel = 0;
    std::array<double, 16> payload{};
};

using Frames = std::vector<BenchmarkFrame>;

std::uintptr_t checksum = 0;

#if defined(__GNUC__) || defined(__clang__)
#define VEDA_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define VEDA_NOINLINE __declspec(noinline)
#else
#define VEDA_NOINLINE
#endif

VEDA_NOINLINE void consume(const Frames& frames) {
    checksum ^= reinterpret_cast<std::uintptr_t>(frames.data());
    checksum += frames.size();
}

VEDA_NOINLINE void processByValue(Frames frames) {
    consume(frames);
}

VEDA_NOINLINE void processByReference(const Frames& frames) {
    consume(frames);
}

Frames makeFrames(std::size_t frameCount) {
    Frames frames;
    frames.resize(frameCount);
    for (std::size_t index = 0; index < frameCount; ++index)
        frames[index].channel = static_cast<int>(index);
    return frames;
}

double measureMedianNsPerCall(bool optimized, std::size_t frameCount, int repetitions) {
    constexpr int kTrials = 11;
    std::vector<double> samples;
    samples.reserve(kTrials);

    std::function<void(Frames)> callback;
    if (optimized) {
        callback = [](Frames frames) { processByReference(frames); };
    } else {
        callback = [](Frames frames) { processByValue(std::move(frames)); };
    }

    for (int trial = 0; trial < kTrials; ++trial) {
        for (int warmup = 0; warmup < 1'000; ++warmup)
            callback(makeFrames(frameCount));

        const auto begin = Clock::now();
        for (int repetition = 0; repetition < repetitions; ++repetition)
            callback(makeFrames(frameCount));
        const auto end = Clock::now();

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
        std::size_t frames;
        int repetitions;
    };
    const std::vector<Case> cases = {
        {1, 1'000'000},
        {4, 1'000'000},
        {16, 500'000},
        {64, 200'000},
    };

    std::cout << "frames,current_ns,reference_ns\n";
    std::cout << std::fixed << std::setprecision(2);
    for (const auto& benchmark : cases) {
        const double current = measureMedianNsPerCall(false, benchmark.frames, benchmark.repetitions);
        const double reference = measureMedianNsPerCall(true, benchmark.frames, benchmark.repetitions);
        std::cout << benchmark.frames << ',' << current << ',' << reference << '\n';
    }

    if (checksum == 0)
        std::cerr << "";
}
