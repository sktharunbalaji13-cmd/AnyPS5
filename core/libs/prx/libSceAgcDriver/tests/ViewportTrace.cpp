// Mission 8: unit test for the viewport provenance trace (pure logic, no GPU).

#include "prx/libSceAgcDriver/Graphics/include/ViewportTrace.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace AgcDriver::Graphics::ViewportTrace;

    // The register map: which offsets are the clip viewport, of which context, and whether
    // they are a scale or an offset.
    Require(IsViewportRegister(0x10f), "the scale X register is a viewport register");
    Require(IsViewportRegister(0x114), "the offset Z register is a viewport register");
    Require(IsViewportRegister(0x10f + 6 * 15 + 5), "the last context's offset Z is a viewport register");
    Require(!IsViewportRegister(0x10e), "the register before the viewport is not one");
    Require(!IsViewportRegister(0x10f + 6 * 16), "the register past the last context is not one");
    Require(ViewportContext(0x10f) == 0, "the first context is context 0");
    Require(ViewportContext(0x10f + 6 * 3 + 1) == 3, "the fourth context is context 3");
    Require(IsScaleRegister(0x10f), "the X scale is a scale");
    Require(!IsScaleRegister(0x110), "the X offset is an offset");
    Require(IsScaleRegister(0x111), "the Y scale is a scale");

    // The value classification: a genuine zero, a subnormal (a fixed-point value read as a
    // float), an ordinary value and a non-finite one.
    Require(Classify(0.0f) == Value::Zero, "zero is classified as zero");
    Require(Classify(1.0f) == Value::Normal, "one is an ordinary value");
    Require(Classify(2160.0f) == Value::Normal, "a screen height is an ordinary value");
    Require(Classify(std::numeric_limits<float>::denorm_min()) == Value::Denormal,
            "the smallest subnormal is classified as a denormal");
    Require(Classify(1.4e-45f) == Value::Denormal, "a tiny subnormal is a denormal");
    Require(Classify(std::numeric_limits<float>::infinity()) == Value::NonFinite, "an infinity is not finite");
    Require(std::string(ValueName(Value::Zero)) == "zero", "the zero class has a name");
    Require(std::string(ValueName(Value::Denormal)) == "denormal", "the denormal class has a name");

    // The formatting carries the value and its raw bits, so a genuine zero is distinguishable
    // from a mis-decoded bit pattern.
    Require(FormatFloat(0.0f) == std::string("0/0x00000000"), "zero formats with its bits");
    Require(FormatFloat(1.0f) == std::string("1/0x3f800000"), "one formats with its bits");
    Require(FormatFloat(2160.0f) == std::string("2160/0x45070000"), "the screen height formats with its bits");
    Require(Bits(0.0f) == 0u, "the bits of zero are zero");
    Require(Bits(1.0f) == 0x3f800000u, "the bits of one are the IEEE pattern");

    // The sample selection: the first draws, the degenerate ones, and a later sample.
    auto& counters = Stats();
    counters.decodes.store(0, std::memory_order_relaxed);
    counters.detailed.store(0, std::memory_order_relaxed);
    Selection selection;
    std::uint64_t admitted = 0;
    for (std::uint64_t index = 0; index < Selection::FirstDraws; ++index) {
        counters.decodes.fetch_add(1, std::memory_order_relaxed);
        if (selection.Admit(false)) ++admitted;
    }
    Require(admitted == Selection::FirstDraws, "the first draws are sampled");
    Require(std::string(selection.ReasonName()) == "first", "the first draws are sampled as such");
    counters.decodes.fetch_add(1, std::memory_order_relaxed);
    Require(selection.Admit(true), "a degenerate viewport is always sampled");
    Require(std::string(selection.ReasonName()) == "degenerate", "the degenerate draws are sampled as such");

    // The detail cap: past it nothing is sampled and the suppression is counted.
    counters.detailed.store(Selection::DetailLimit(), std::memory_order_relaxed);
    const auto before = counters.suppressed.load(std::memory_order_relaxed);
    counters.decodes.fetch_add(1, std::memory_order_relaxed);
    Require(!selection.Admit(true), "the detail cap stops the sampling");
    Require(counters.suppressed.load(std::memory_order_relaxed) == before + 1, "the suppression is counted");

    std::printf("viewport trace tests passed\n");
    return 0;
}
