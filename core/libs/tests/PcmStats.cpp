// Mission 6: unit test for the bounded PCM statistics helper.

#include "prx/libc/include/general/PcmStats.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

bool Near(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

} // namespace

int main() {
    // All-zero signed 16-bit PCM.
    {
        std::vector<std::int16_t> samples(64, 0);
        const auto stats = Aps5Pcm::Compute(samples.data(), samples.size() * sizeof(std::int16_t), 2, false);
        Require(stats.supported, "a whole buffer of s16 is supported");
        Require(stats.sampleCount == 64, "every sample is counted");
        Require(stats.frames == 32, "frames follow the channel count");
        Require(stats.zeroSamples == 64, "every sample is zero");
        Require(stats.peak == 0.0 && stats.rms == 0.0, "silence has no peak and no rms");
        Require(!stats.isFloat, "the format is reported");
    }

    // A known signed 16-bit pattern: 0, 16384, -16384, 32767.
    {
        const std::vector<std::int16_t> samples = {0, 16384, -16384, 32767};
        const auto stats = Aps5Pcm::Compute(samples.data(), samples.size() * sizeof(std::int16_t), 1, false);
        Require(stats.supported, "a whole buffer of s16 is supported");
        Require(Near(stats.peak, 32767.0 / 32768.0, 1e-6), "the peak is the largest magnitude");
        Require(stats.zeroSamples == 1, "only the zero sample is counted as zero");
        const double expected = std::sqrt((0.0 + 0.25 + 0.25 + (32767.0 / 32768.0) * (32767.0 / 32768.0)) / 4.0);
        Require(Near(stats.rms, expected, 1e-6), "the rms matches the expected value");
    }

    // 32-bit float PCM.
    {
        const std::vector<float> samples = {0.5f, -0.5f, 0.25f, 0.0f};
        const auto stats = Aps5Pcm::Compute(samples.data(), samples.size() * sizeof(float), 2, true);
        Require(stats.supported, "a whole buffer of f32 is supported");
        Require(stats.isFloat, "the format is reported");
        Require(Near(stats.peak, 0.5, 1e-6), "the peak is the largest magnitude");
        Require(Near(stats.rms, std::sqrt((0.25 + 0.25 + 0.0625 + 0.0) / 4.0), 1e-6), "the rms matches");
        Require(stats.zeroSamples == 1, "only the zero sample is counted as zero");
        Require(stats.frames == 2, "frames follow the channel count");
    }

    // A non-finite float sample is reported, not silently mixed into the statistics.
    {
        const std::vector<float> samples = {1.0f, std::numeric_limits<float>::quiet_NaN()};
        const auto stats = Aps5Pcm::Compute(samples.data(), samples.size() * sizeof(float), 1, true);
        Require(stats.supported, "a whole buffer of f32 is supported");
        Require(stats.nonFinite, "a non-finite sample is reported");
        Require(Near(stats.peak, 1.0, 1e-6), "the finite samples still give the peak");
    }

    // A buffer that is not a whole number of samples is refused.
    {
        const std::vector<std::uint8_t> bytes(7, 0);
        const auto stats = Aps5Pcm::Compute(bytes.data(), bytes.size(), 1, false);
        Require(!stats.supported, "a partial sample buffer is refused");
        const auto none = Aps5Pcm::Compute(nullptr, 0, 1, false);
        Require(!none.supported, "an empty buffer is refused");
    }

    std::printf("pcm stats tests passed\n");
    return 0;
}
