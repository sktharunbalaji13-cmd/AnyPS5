// Mission 8: unit test for the pipeline-statistics probe's slot lifecycle and the reporting
// of a result's availability (no GPU, no fabricated counters).

#include "prx/libSceAgcDriver/Graphics/include/PipelineStatsProbe.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    using AgcDriver::Graphics::PipelineStatsProbe::Availability;
    using AgcDriver::Graphics::PipelineStatsProbe::AvailabilityName;
    using AgcDriver::Graphics::PipelineStatsProbe::AvailabilityOf;

    // The availability of a result is named, never turned into a zero counter.
    Require(AvailabilityOf(0) == Availability::ValidResult, "VK_SUCCESS is a valid result");
    Require(AvailabilityOf(1) == Availability::NotReady, "VK_NOT_READY is not ready");
    Require(AvailabilityOf(-9) == Availability::Unsupported, "VK_FEATURE_NOT_PRESENT is unsupported");
    Require(AvailabilityOf(-3) == Availability::Invalid, "another error is invalid");
    Require(std::string(AvailabilityName(Availability::ValidResult)) == "VALID_RESULT", "the valid state is named");
    Require(std::string(AvailabilityName(Availability::NotReady)) == "NOT_READY", "the not-ready state is named");
    Require(std::string(AvailabilityName(Availability::Unsupported)) == "UNSUPPORTED", "the unsupported state is named");
    Require(std::string(AvailabilityName(Availability::Invalid)) == "INVALID", "the invalid state is named");

    // The requested counters: the ones the mission names, and only those.
    const auto flags = AgcDriver::Graphics::PipelineStatsProbe::Flags();
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT) != 0, "input assembly vertices are requested");
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT) != 0, "input assembly primitives are requested");
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT) != 0, "vertex shader invocations are requested");
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT) != 0, "clipping invocations are requested");
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT) != 0, "clipping primitives are requested");
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT) != 0, "fragment shader invocations are requested");
    // The geometry-shader counters are asked for only on a device that has a geometry shader:
    // a statistic the device cannot produce leaves the whole query's result unavailable.
    Require((flags & VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT) == 0,
            "the geometry shader counters are not requested without a geometry shader");
    const auto geometryFlags = AgcDriver::Graphics::PipelineStatsProbe::Flags(true);
    Require((geometryFlags & VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT) != 0,
            "the geometry shader invocations are requested with a geometry shader");
    Require((geometryFlags & VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT) != 0,
            "the geometry shader primitives are requested with a geometry shader");
    Require((geometryFlags & VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT) != 0,
            "the fragment shader invocations stay requested with a geometry shader");

    // The counter names match the count the probe reads.
    using AgcDriver::Graphics::PipelineStatsProbe::CounterCount;
    using AgcDriver::Graphics::PipelineStatsProbe::CounterName;
    Require(CounterCount == 8, "eight counters are read");
    Require(std::string(CounterName(0)) == "input_assembly_vertices", "the first counter is named");
    Require(std::string(CounterName(7)) == "fragment_shader_invocations", "the last counter is named");
    Require(std::string(CounterName(CounterCount)) == "unknown", "a counter past the end has no name");

    // The slot lifecycle: a slot is reserved while its result may be pending, and released
    // only afterwards. The probe never has more slots in flight than it has slots.
    auto& pool = AgcDriver::Graphics::PipelineStatsProbe::Pool::Get();
    std::set<int> held;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto slot = pool.Reserve();
        if (slot < 0) {
            Require(held.size() == AgcDriver::Graphics::PipelineStatsProbe::Pool::Slots,
                    "the pool refuses a slot only when every slot is in flight");
            break;
        }
        Require(held.insert(slot).second, "a reserved slot is not handed out twice");
        Require(static_cast<std::size_t>(slot) < AgcDriver::Graphics::PipelineStatsProbe::Pool::Slots,
                "the slot index is inside the pool");
    }
    for (const auto slot : held) pool.Release(slot);
    const auto after = pool.Reserve();
    Require(after >= 0, "a released slot can be reserved again");
    pool.Release(after);

    // The begin time of a slot is recorded and read back.
    AgcDriver::Graphics::PipelineStatsProbe::NoteBegin(0, 12345);
    Require(AgcDriver::Graphics::PipelineStatsProbe::BeginTime(0) == 12345, "the begin time is recorded");
    Require(AgcDriver::Graphics::PipelineStatsProbe::BeginTime(-1) == 0, "an invalid slot has no begin time");

    std::printf("pipeline statistics probe tests passed\n");
    return 0;
}
