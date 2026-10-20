#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PUSHTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PUSHTRACE_HPP

// Mission 10: a timestamped, bounded trace of the push-constant lifecycle.
//
// The push data the guest's program carries is assembled on the CPU, optionally patched by the
// driver, uploaded with vkCmdPushConstants and then consumed by the draw that follows. This
// module observes those existing stages; it never replaces, duplicates or alters the payload.
//
// Every event carries the diagnostic channel's own prefix (the UTC wall clock, the monotonic
// elapsed time, the thread id and the sequence number) plus the fields below: the monotonic
// nanoseconds, the elapsed milliseconds from the same origin, a correlation id that links the
// stages of one block, the shader identity, the target, the batch and the exact bytes.

#include "prx/libc/include/general/DiagTrace.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace AgcDriver {
namespace Graphics {
namespace PushTrace {

// The control: APS5_DIAG_PUSH_TRACE (presence) switches the trace on.
inline bool Enabled() {
    static const bool on = Aps5Diag::Enabled("APS5_DIAG_PUSH_TRACE");
    return on;
}

// The identities the trace follows: a comma-separated list of the programs' pipeline
// identities (the SPIR-V content hash in hex). The default is the two programs Mission 9
// measured; an identity outside the list is counted, not traced.
inline const std::vector<std::string>& TracedIdentities() {
    static const std::vector<std::string> identities = [] {
        std::vector<std::string> result;
        const char* text = std::getenv("APS5_DIAG_PUSH_TRACE_IDS");
        if (text == nullptr || *text == '\0') {
            result.push_back("dbcdd752b39dc3421aa7a26d998f06f7");
            result.push_back("9bc3e517b32e7bdd4dba94778e654067");
            return result;
        }
        const std::string value(text);
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t comma = value.find(',', start);
            const std::size_t end = comma == std::string::npos ? value.size() : comma;
            const std::string item = value.substr(start, end - start);
            if (!item.empty()) result.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return result;
    }();
    return identities;
}

inline bool IsTraced(const std::string& identity) {
    const auto& identities = TracedIdentities();
    return std::find(identities.begin(), identities.end(), identity) != identities.end();
}

// The identity of a stage's program: the pipeline identity's two words in hex.
inline std::string IdentityText(std::uint64_t low, std::uint64_t high) {
    char buffer[40];
    // The same order the draw-shader event uses (the low word first), so the identity strings
    // of the two diagnostics match.
    std::snprintf(buffer, sizeof(buffer), "%016llx%016llx", static_cast<unsigned long long>(low),
                  static_cast<unsigned long long>(high));
    return buffer;
}

// The monotonic clock the trace uses: one origin for the whole process, the same one the
// diagnostic channel's elapsed time comes from.
inline std::chrono::steady_clock::time_point Origin() {
    static const auto origin = std::chrono::steady_clock::now();
    return origin;
}

inline std::uint64_t MonoNanoseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - Origin()).count());
}

inline std::uint64_t ElapsedMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - Origin()).count());
}

// The exact bytes: the hexadecimal form, the 32-bit words, the float views and a stable
// fingerprint. The words and the floats are *views* of the same bytes, never a reinterpretation
// of the source type.
inline std::string Hex(const std::uint8_t* data, std::size_t size) {
    std::string result;
    result.reserve(size * 2);
    static const char* digits = "0123456789abcdef";
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(digits[data[index] >> 4]);
        result.push_back(digits[data[index] & 0xfu]);
    }
    return result;
}

inline std::string Words(const std::uint8_t* data, std::size_t size) {
    std::string result;
    char buffer[16];
    for (std::size_t index = 0; index + 4 <= size; index += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, data + index, sizeof(word));
        if (!result.empty()) result.push_back(',');
        std::snprintf(buffer, sizeof(buffer), "0x%08x", word);
        result += buffer;
    }
    return result;
}

inline std::string Floats(const std::uint8_t* data, std::size_t size) {
    std::string result;
    char buffer[40];
    for (std::size_t index = 0; index + 4 <= size; index += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, data + index, sizeof(word));
        const float value = std::bit_cast<float>(word);
        if (!result.empty()) result.push_back(',');
        std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        result += buffer;
    }
    return result;
}

// FNV-1a 64, the hash the draw path already uses for its resource keys.
inline std::uint64_t Fingerprint(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

// The sample selection: a bounded number of samples per traced identity.
class Sampler {
public:
    enum class Reason : std::uint8_t { None, First, Bucket, Changed };

    // Whether this draw's block is emitted, and why.
    bool Admit(const std::string& identity, std::uint64_t fingerprint, std::uint64_t elapsedMs, Reason& reason) {
        const std::lock_guard<std::mutex> lock(mutex);
        auto& state = states[identity];
        if (!state.seen) {
            state.seen = true;
            // The first sample records the value, so the same value later is not a change.
            state.fingerprint = fingerprint;
            ++state.scheduled;
            reason = Reason::First;
            return true;
        }
        if (state.fingerprint != fingerprint && state.changed < ChangedLimit) {
            state.fingerprint = fingerprint;
            ++state.changed;
            reason = Reason::Changed;
            return true;
        }
        const auto bucket = elapsedMs / BucketMilliseconds;
        if (bucket != state.bucket && state.scheduled < ScheduledLimit) {
            state.bucket = bucket;
            ++state.scheduled;
            reason = Reason::Bucket;
            return true;
        }
        reason = Reason::None;
        ++state.suppressed;
        return false;
    }

    // The counters, for the summary.
    struct Counts {
        std::uint64_t scheduled = 0;
        std::uint64_t changed = 0;
        std::uint64_t suppressed = 0;
    };

    Counts CountsOf(const std::string& identity) {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto found = states.find(identity);
        if (found == states.end()) return Counts{};
        return Counts{found->second.scheduled, found->second.changed, found->second.suppressed};
    }

    static constexpr std::uint64_t ScheduledLimit = 40;
    static constexpr std::uint64_t ChangedLimit = 16;
    static constexpr std::uint64_t BucketMilliseconds = 5000;

private:
    struct State {
        bool seen = false;
        std::uint64_t bucket = 0;
        std::uint64_t fingerprint = 0;
        std::uint64_t scheduled = 0;
        std::uint64_t changed = 0;
        std::uint64_t suppressed = 0;
    };

    std::mutex mutex;
    std::map<std::string, State> states;
};

// The draw the current push block belongs to, set by the draw path before the push is
// recorded. It is per thread, so it costs nothing on the path itself.
struct DrawContext {
    std::uint64_t target = 0;
    std::uint64_t batch = 0;
    std::uint64_t drawId = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 0;
};

inline DrawContext& CurrentDraw() {
    thread_local DrawContext context;
    return context;
}

// The sampler instance.
inline Sampler& SamplerInstance() {
    static Sampler sampler;
    return sampler;
}

// The correlation: one block's stages are linked by a correlation id. The pending table is
// bounded and every unmatched stage is counted and reported.
class Correlator {
public:
    // The pending record of the calling thread.
    struct Pending {
        bool active = false;
        std::uint64_t correlation = 0;
        std::string identity;
        std::uint32_t offset = 0;
        std::vector<std::uint8_t> bytes;
        std::uint64_t fingerprint = 0;
        std::uint64_t createdMs = 0;
        bool uploaded = false;
    };

    static Pending& Current() {
        thread_local Pending pending;
        return pending;
    }

    static std::uint64_t NextCorrelation() {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    // The unmatched (expired or never completed) correlations.
    static std::uint64_t Unmatched() {
        static std::atomic<std::uint64_t> unmatched{0};
        return unmatched.load(std::memory_order_relaxed);
    }
};

} // namespace PushTrace
} // namespace Graphics
} // namespace AgcDriver

#endif
