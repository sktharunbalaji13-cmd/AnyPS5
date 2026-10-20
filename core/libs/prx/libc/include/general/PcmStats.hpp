#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_PCMSTATS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_PCMSTATS_HPP

// Mission 6: bounded statistics for a PCM buffer whose format is known.
//
// The statistics are computed on a copy of the values in registers only; the buffer is never
// modified and nothing is clipped. Interleaved samples are counted as individual samples
// (frames x channels); the peak and RMS are normalised to 0..1 for signed 16-bit PCM
// (divided by 32768) and are the absolute values for 32-bit float PCM.
//
// Only formats whose sample encoding is known are accepted: signed 16-bit PCM and 32-bit
// float PCM. A different encoding is reported as unsupported rather than guessed at.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Aps5Pcm {

struct Stats {
    std::uint64_t frames = 0;
    std::uint32_t channels = 0;
    bool isFloat = false;
    bool supported = false;
    bool nonFinite = false;   // a float sample was NaN or infinite
    double peak = 0.0;        // largest absolute sample, 0..1
    double rms = 0.0;          // root mean square, 0..1
    std::uint64_t sampleCount = 0;
    std::uint64_t zeroSamples = 0;
};

// Computes the statistics of `bytes` bytes of interleaved PCM.
//   channels:  the channel count (0 means "unknown, treated as 1")
//   isFloat:   true for 32-bit float PCM, false for signed 16-bit PCM
// Returns a Stats with supported == false when the buffer is not a whole number of frames.
inline Stats Compute(const void* data, std::size_t bytes, std::uint32_t channels, bool isFloat) {
    Stats stats;
    stats.channels = channels == 0 ? 1 : channels;
    stats.isFloat = isFloat;
    const std::size_t sampleBytes = isFloat ? sizeof(float) : sizeof(std::int16_t);
    if (data == nullptr || bytes == 0 || (bytes % sampleBytes) != 0) return stats;
    stats.supported = true;
    const std::size_t samples = bytes / sampleBytes;
    stats.sampleCount = samples;
    stats.frames = samples / stats.channels;
    double sumSquares = 0.0;
    if (isFloat) {
        const auto* values = static_cast<const float*>(data);
        for (std::size_t index = 0; index < samples; ++index) {
            const float value = values[index];
            if (!std::isfinite(value)) {
                stats.nonFinite = true;
                continue;
            }
            const double magnitude = std::fabs(static_cast<double>(value));
            if (magnitude > stats.peak) stats.peak = magnitude;
            sumSquares += magnitude * magnitude;
            if (value == 0.0f) stats.zeroSamples += 1;
        }
    } else {
        const auto* values = static_cast<const std::int16_t*>(data);
        for (std::size_t index = 0; index < samples; ++index) {
            const double value = static_cast<double>(values[index]) / 32768.0;
            const double magnitude = std::fabs(value);
            if (magnitude > stats.peak) stats.peak = magnitude;
            sumSquares += value * value;
            if (values[index] == 0) stats.zeroSamples += 1;
        }
    }
    if (samples != 0) stats.rms = std::sqrt(sumSquares / static_cast<double>(samples));
    return stats;
}

} // namespace Aps5Pcm

#endif
