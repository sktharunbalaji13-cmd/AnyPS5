#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINESTATSPROBE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINESTATSPROBE_HPP

// Mission 7: bounded, opt-in GPU pipeline statistics.
//
// The probe answers one question the draw-input trace cannot: does the GPU carry the draw's
// vertices through input assembly, the vertex shader, clipping and the fragment shader? The
// counters are the device's own; nothing here fabricates a value.
//
// The whole capability is opt-in: the device feature is requested only when
// APS5_DIAG_PIPELINE_STATS=1, and the pool exists only then. A query never waits: the results
// are read in the recorder's completion action, after the batch's fence has signalled.

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/general/DiagTrace.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace AgcDriver {
namespace Graphics {
namespace PipelineStatsProbe {

// Whether the probe is switched on. The device feature is requested under the same variable,
// so the pool is only created when the device really has the feature.
inline bool Enabled() {
    static const bool on = Aps5Diag::Enabled("APS5_DIAG_PIPELINE_STATS");
    return on;
}

// The counters the probe asks for.
//
// A statistic the device does not support is *undefined*, and a driver that cannot produce it
// may never make the query's results available at all: Mission 8's first run asked for the
// geometry-shader counters on a device without a geometry shader, and the result never arrived
// (a waiting read hung the completion worker). The counters are therefore chosen from what the
// device actually has.
inline VkQueryPipelineStatisticFlags Flags(bool geometryShader) {
    auto flags = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    if (geometryShader) {
        flags |= VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
                 VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT;
    }
    return flags;
}

// The counters the probe asks for when the device's geometry-shader support is unknown.
inline VkQueryPipelineStatisticFlags Flags() {
    return Flags(false);
}

// The counters the probe reads, in the order of Flags().
inline constexpr std::size_t CounterCount = 8;

inline const char* CounterName(std::size_t index) {
    static const char* names[CounterCount] = {
        "input_assembly_vertices", "input_assembly_primitives", "vertex_shader_invocations",
        "geometry_shader_invocations", "geometry_shader_primitives", "clipping_invocations",
        "clipping_primitives", "fragment_shader_invocations"};
    return index < CounterCount ? names[index] : "unknown";
}

// The pool: a fixed number of slots, created once. A slot is reserved while its query is in
// flight and released when the result has been read, so a slot is never reused too early.
class Pool {
public:
    // Creates the pool if needed. Returns false when the device did not enable the feature or
    // the pool could not be created; the reason is reported once.
    bool Ensure(const Context& context, bool geometryShader = false) {
        if (!context.pipelineStatisticsQuery) {
            ReportUnsupported("the device was not created with pipelineStatisticsQuery");
            return false;
        }
        const std::lock_guard<std::mutex> lock(mutex);
        if (pool != VK_NULL_HANDLE) return true;
        const auto create = context.Function<PFN_vkCreateQueryPool>("vkCreateQueryPool");
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = static_cast<std::uint32_t>(Slots);
        info.pipelineStatistics = Flags(geometryShader);
        geometry = geometryShader;
        if (create(context.device, &info, nullptr, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            ReportUnsupported("the query pool could not be created");
            return false;
        }
        Aps5Diag::Event("pipeline-stats", "event=pool-created slots=%zu counters=%zu geometry=%d flags=0x%x", Slots, CounterCount,
                         geometry ? 1 : 0, static_cast<unsigned>(info.pipelineStatistics));
        return true;
    }

    static constexpr std::size_t Slots = 8;

    // Reserves a free slot, or returns -1 when every slot is in flight.
    int Reserve() {
        const std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t slot = 0; slot < Slots; ++slot) {
            if ((freeSlots & (1ull << slot)) != 0) {
                freeSlots &= ~(1ull << slot);
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

    void Release(int slot) {
        if (slot < 0) return;
        const std::lock_guard<std::mutex> lock(mutex);
        freeSlots |= 1ull << slot;
    }

    VkQueryPool Handle() const { return pool; }

    static Pool& Get() {
        static Pool pool;
        return pool;
    }

private:
    void ReportUnsupported(const char* reason) {
        static std::atomic<bool> reported{false};
        if (reported.exchange(true)) return;
        Aps5Diag::Failure("pipeline-stats", "event=unsupported reason=%s", reason);
    }

    std::mutex mutex;
    VkQueryPool pool = VK_NULL_HANDLE;
    bool geometry = false;
    std::uint64_t freeSlots = (1ull << Slots) - 1;
};

// The elapsed time at which each slot's query was begun, so a result event can report the
// query's own timeline.
inline std::array<std::uint64_t, Pool::Slots>& BeginTimes() {
    static std::array<std::uint64_t, Pool::Slots> begins{};
    return begins;
}

inline std::mutex& BeginMutex() {
    static std::mutex mutex;
    return mutex;
}

inline void NoteBegin(int slot, std::uint64_t micros) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= Pool::Slots) return;
    const std::lock_guard<std::mutex> lock(BeginMutex());
    BeginTimes()[static_cast<std::size_t>(slot)] = micros;
}

inline std::uint64_t BeginTime(int slot) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= Pool::Slots) return 0;
    const std::lock_guard<std::mutex> lock(BeginMutex());
    return BeginTimes()[static_cast<std::size_t>(slot)];
}

// The availability of a query result. A result that is not available is never reported as a
// zero counter: the state names it instead.
enum class Availability : std::uint8_t { ValidResult, NotReady, Unsupported, Invalid };

inline const char* AvailabilityName(Availability availability) {
    switch (availability) {
    case Availability::ValidResult: return "VALID_RESULT";
    case Availability::NotReady: return "NOT_READY";
    case Availability::Unsupported: return "UNSUPPORTED";
    case Availability::Invalid: return "INVALID";
    }
    return "UNKNOWN";
}

// The availability of a vkGetQueryPoolResults return code.
inline Availability AvailabilityOf(int status) {
    if (status == 0) return Availability::ValidResult;          // VK_SUCCESS
    if (status == 1) return Availability::NotReady;               // VK_NOT_READY
    if (status == -9) return Availability::Unsupported;           // VK_FEATURE_NOT_PRESENT
    return Availability::Invalid;
}

// The number of queries the probe has taken and the number it could not take.
inline std::atomic<std::uint64_t>& QueriesTaken() {
    static std::atomic<std::uint64_t> taken{0};
    return taken;
}

inline std::atomic<std::uint64_t>& QueriesSkipped() {
    static std::atomic<std::uint64_t> skipped{0};
    return skipped;
}

} // namespace PipelineStatsProbe
} // namespace Graphics
} // namespace AgcDriver

#endif
