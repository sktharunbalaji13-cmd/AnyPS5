#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWINPUTTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWINPUTTRACE_HPP

// Mission 7: bounded, opt-in tracing of the draw's own inputs.
//
// The trace answers one question: does a draw that the guest issues carry real work, and does
// that work reach the graphics API? It records the guest's counts, the buffers the driver
// resolved for them, the raster state in force, and the arguments actually passed to Vulkan.
//
// Everything here is off by default and bounded: the detailed records are capped, the
// aggregate counters are plain atomics, and no event is written without the control being set.

#include "prx/libc/include/general/DiagTrace.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver {
namespace Graphics {
namespace DrawInputTrace {

// One vertex fetch as the driver resolved it: the guest's descriptor (address, stride, record
// count), the byte range the draw needs, and how the driver serves it.
struct VertexFetch {
    std::uint64_t address = 0;      // the guest address the descriptor names
    std::uint64_t bytes = 0;        // the byte range the draw needs
    std::uint64_t boundOffset = 0;  // the offset of the fetch in the bound buffer
    std::uint32_t stride = 0;
    std::uint32_t records = 0;
    std::uint32_t alignment = 0;
    std::uint32_t fetchIndex = 0;
    const char* source = "unavailable";  // import | copy | zero-padded | zeroed | unavailable
    bool valid = false;                   // the descriptor named a usable range
    // Mission 9: the declared attribute this fetch belongs to (the guest program's input
    // semantics), as the recompiler resolved it.
    std::uint32_t location = 0;
    std::uint32_t components = 0;
    std::uint32_t format = 0;
};

// The draw's identity and the shape of its work.
struct DrawIdentity {
    std::uint64_t id = 0;
    std::uint64_t batch = 0;
    std::uint64_t target = 0;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    bool recipe = false;
};

// The counters every draw feeds, whether or not it is sampled.
struct Counters {
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> indexed{0};
    std::atomic<std::uint64_t> direct{0};
    std::atomic<std::uint64_t> indirect{0};
    std::atomic<std::uint64_t> mesh{0};
    std::atomic<std::uint64_t> recipeDraws{0};
    std::atomic<std::uint64_t> freshDraws{0};
    std::atomic<std::uint64_t> zeroVertexCount{0};
    std::atomic<std::uint64_t> zeroIndexCount{0};
    std::atomic<std::uint64_t> zeroInstanceCount{0};
    std::atomic<std::uint64_t> noVertexBuffer{0};
    std::atomic<std::uint64_t> noVertexAttributes{0};
    std::atomic<std::uint64_t> vertexAttributeCount{0};
    std::atomic<std::uint64_t> zeroedVertexBuffer{0};
    std::atomic<std::uint64_t> invalidVertexFetch{0};
    std::atomic<std::uint64_t> vertexCountSum{0};
    std::atomic<std::uint64_t> indexCountSum{0};
    std::atomic<std::uint64_t> instanceCountSum{0};
    std::atomic<std::uint64_t> maxVertexCount{0};
    std::atomic<std::uint64_t> maxIndexCount{0};
    std::atomic<std::uint64_t> maxInstanceCount{0};
    std::atomic<std::uint64_t> detailed{0};
    std::atomic<std::uint64_t> suppressed{0};
};

inline Counters& Stats() {
    static Counters counters;
    return counters;
}

// The controls: APS5_DIAG_DRAW_INPUT (presence) switches the trace on,
// APS5_DIAG_DRAW_INPUT_MAX caps the detailed records (default 512, hard maximum 4096).
inline bool Enabled() {
    static const bool on = Aps5Diag::Enabled("APS5_DIAG_DRAW_INPUT");
    return on;
}

inline std::uint32_t DetailLimit() {
    static const std::uint32_t limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        Aps5Diag::EnvLimit("APS5_DIAG_DRAW_INPUT_MAX", 512), 4096));
    return limit;
}

// The draw id: a counter of this module, not of the process. Events name the module they come
// from, so two modules' ids are never compared as one sequence.
inline std::uint64_t NextId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// The sample selection. The rules are the ones the mission names: the first draws, the first
// draws of each distinct target, every suspicious draw, and a sparse set of later draws.
class Selection {
public:
    // Whether this draw is sampled in detail, and why.
    enum class Reason : std::uint8_t { None, First, Target, Suspicious, Later };

    bool Admit(std::uint64_t target, bool suspicious) {
        const auto limit = DetailLimit();
        const auto taken = Stats().detailed.load(std::memory_order_relaxed);
        if (taken >= limit) {
            Stats().suppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Reason reason = Reason::None;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto count = static_cast<std::uint64_t>(Stats().draws.load(std::memory_order_relaxed));
            const auto index = count > 0 ? count - 1 : 0;
            if (index < FirstDraws) {
                reason = Reason::First;
            } else if (suspicious) {
                reason = Reason::Suspicious;
            } else if (target != 0) {
                auto& count = targetCounts[target];
                if (count < PerTarget && knownTargets < MaxTargets) {
                    ++count;
                    if (count == 1) ++knownTargets;
                    reason = Reason::Target;
                }
            }
            if (reason == Reason::None && index % LaterStride == 0) reason = Reason::Later;
        }
        if (reason == Reason::None) return false;
        Stats().detailed.fetch_add(1, std::memory_order_relaxed);
        lastReason = reason;
        return true;
    }

    static constexpr std::uint64_t FirstDraws = 32;
    static constexpr std::uint64_t PerTarget = 4;
    static constexpr std::uint64_t MaxTargets = 8;
    static constexpr std::uint64_t LaterStride = 2000;

    const char* ReasonName() const {
        switch (lastReason) {
        case Reason::First: return "first";
        case Reason::Target: return "target";
        case Reason::Suspicious: return "suspicious";
        case Reason::Later: return "later";
        default: return "none";
        }
    }

private:
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint64_t> targetCounts;
    std::uint64_t knownTargets = 0;
    Reason lastReason = Reason::None;
};

// The one sampler of the process: the selection keeps the state of which targets it has
// already sampled.
inline Selection& Sampler() {
    static Selection selection;
    return selection;
}

// How often the aggregate report is emitted.
inline std::uint64_t AggregateStride = 1024;

// The aggregate report: emitted every AggregateStride draws so the counters are visible even
// when the detail cap has been reached.
inline void ReportAggregate(const char* stage) {
    const auto& counters = Stats();
    static std::atomic<std::uint64_t> lastReport{0};
    const auto draws = counters.draws.load(std::memory_order_relaxed);
    auto previous = lastReport.load(std::memory_order_relaxed);
    if (draws - previous < AggregateStride) return;
    if (!lastReport.compare_exchange_strong(previous, draws)) return;
    Aps5Diag::Event("draw-input",
                    "event=aggregate stage=%s draws=%llu indexed=%llu direct=%llu indirect=%llu mesh=%llu "
                    "recipe=%llu fresh=%llu zero_v=%llu zero_i=%llu zero_inst=%llu no_vbuf=%llu "
                    "no_vattrs=%llu vattr_sum=%llu zeroed_vbuf=%llu invalid_vfetch=%llu sum_v=%llu sum_i=%llu sum_inst=%llu "
                    "max_v=%llu max_i=%llu max_inst=%llu detailed=%llu suppressed=%llu",
                    stage,
                    static_cast<unsigned long long>(draws),
                    static_cast<unsigned long long>(counters.indexed.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.direct.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.indirect.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.mesh.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.recipeDraws.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.freshDraws.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.zeroVertexCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.zeroIndexCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.zeroInstanceCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.noVertexBuffer.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.noVertexAttributes.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.vertexAttributeCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.zeroedVertexBuffer.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.invalidVertexFetch.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.vertexCountSum.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.indexCountSum.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.instanceCountSum.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.maxVertexCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.maxIndexCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.maxInstanceCount.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.detailed.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.suppressed.load(std::memory_order_relaxed)));
    (void)counters;
}

} // namespace DrawInputTrace
} // namespace Graphics
} // namespace AgcDriver

#endif
