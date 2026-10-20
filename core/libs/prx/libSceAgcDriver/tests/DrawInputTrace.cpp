// Mission 7: unit test for the draw-input trace's sample selection and caps (pure logic).

#include "prx/libSceAgcDriver/Graphics/include/DrawInputTrace.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    using AgcDriver::Graphics::DrawInputTrace::Counters;
    using AgcDriver::Graphics::DrawInputTrace::Selection;
    using AgcDriver::Graphics::DrawInputTrace::Stats;

    // The selection admits the first draws, the first draws of a target, every suspicious draw
    // and a sparse set of later ones.
    auto& counters = Stats();
    Selection selection;
    std::uint64_t admitted = 0;
    for (std::uint64_t index = 0; index < Selection::FirstDraws; ++index) {
        counters.draws.fetch_add(1, std::memory_order_relaxed);
        if (selection.Admit(0, false)) ++admitted;
    }
    Require(admitted == Selection::FirstDraws, "the first draws are sampled");
    Require(std::string(selection.ReasonName()) == "first", "the first draws are sampled as such");

    // A draw after the first ones is not sampled unless it is suspicious or into a new target.
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    Require(!selection.Admit(0, false), "an ordinary later draw without a target is not sampled");
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    Require(selection.Admit(11, false), "the first draw of a target is sampled");
    Require(std::string(selection.ReasonName()) == "target", "the target draws are sampled as such");
    for (std::uint64_t index = 1; index < Selection::PerTarget; ++index) {
        counters.draws.fetch_add(1, std::memory_order_relaxed);
        Require(selection.Admit(11, false), "the target's draws are sampled up to the per-target cap");
    }
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    Require(!selection.Admit(11, false), "the per-target cap holds");
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    Require(selection.Admit(11, true), "a suspicious draw is always sampled");
    Require(std::string(selection.ReasonName()) == "suspicious", "the suspicious draws are sampled as such");

    // The detail cap: past it nothing is sampled and the suppression is counted.
    counters.detailed.store(AgcDriver::Graphics::DrawInputTrace::DetailLimit(), std::memory_order_relaxed);
    const auto before = counters.suppressed.load(std::memory_order_relaxed);
    counters.draws.fetch_add(1, std::memory_order_relaxed);
    Require(!selection.Admit(0, true), "the detail cap stops the sampling");
    Require(counters.suppressed.load(std::memory_order_relaxed) == before + 1, "the suppression is counted");

    // The counters are plain values a caller can read.
    Counters empty;
    Require(empty.draws.load() == 0, "a fresh counter set is zero");

    std::printf("draw input trace tests passed\n");
    return 0;
}
