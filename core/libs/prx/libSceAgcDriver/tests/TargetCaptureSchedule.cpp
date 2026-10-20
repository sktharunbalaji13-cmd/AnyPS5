// Mission 6: unit test for the render-target capture schedule (pure logic, no GPU).

#include "prx/libSceAgcDriver/Graphics/include/TargetCapture.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    using AgcDriver::Graphics::TargetCapture::ParseTimes;
    using AgcDriver::Graphics::TargetCapture::Schedule;

    // The time list: the positive numbers in ascending order, each one once, the malformed
    // entries ignored rather than fatal.
    Require(ParseTimes(nullptr).empty(), "an unset time list is empty");
    Require(ParseTimes("").empty(), "an empty time list is empty");
    Require(ParseTimes("abc").empty(), "a non-numeric time list is empty");
    const auto times = ParseTimes("40,65");
    Require(times.size() == 2, "two entries are parsed");
    Require(times[0] == 40.0 && times[1] == 65.0, "the values are kept in order");
    const auto mixed = ParseTimes("65,10,oops,-5,0,40");
    Require(mixed.size() == 3, "invalid and non-positive entries are dropped");
    Require(mixed[0] == 10.0 && mixed[1] == 40.0 && mixed[2] == 65.0, "the remaining entries are sorted");
    Require(ParseTimes("20,20").size() == 1, "a repeated time is kept once");

    // A capture is due once the elapsed time has reached the next point that this target has
    // not been sampled for yet, and every target keeps its own count.
    Schedule schedule(ParseTimes("40,65"), 6);
    Require(schedule.Enabled(), "the schedule is enabled");
    Require(!schedule.ShouldCapture(7, 0.0), "nothing is due before the first point");
    Require(!schedule.ShouldCapture(7, 39.9), "nothing is due just before the first point");
    Require(schedule.ShouldCapture(7, 40.0), "the first point is due at its time");
    Require(schedule.ShouldCapture(7, 40.0), "a point stays due until it is marked");
    schedule.MarkCaptured(7);
    Require(schedule.Captures() == 1, "the capture is counted");
    Require(!schedule.ShouldCapture(7, 40.1), "the same target waits for its next point");
    Require(schedule.ShouldCapture(7, 65.0), "the target's next point is due at its time");
    Require(schedule.ShouldCapture(8, 65.0), "another target has its own count");
    schedule.MarkCaptured(7);
    schedule.MarkCaptured(8);
    Require(schedule.Captures() == 3, "every capture is counted");

    // The cap bounds the number of captures in the whole run.
    Schedule capped(ParseTimes("1"), 1);
    Require(capped.ShouldCapture(1, 1.0), "the first capture is allowed");
    capped.MarkCaptured(1);
    Require(!capped.ShouldCapture(2, 2.0), "the cap stops further captures");

    // The pixel limit: a target larger than the limit is not captured, and a limit of zero
    // means no limit at all.
    using AgcDriver::Graphics::TargetCapture::WithinPixelLimit;
    Require(WithinPixelLimit(3840, 2160, 0), "a limit of zero means no limit");
    Require(WithinPixelLimit(3840, 2160, 8u * 1024u * 1024u), "a target under the limit is captured");
    Require(!WithinPixelLimit(3840, 2160, 2u * 1024u * 1024u), "a target over the limit is not captured");
    Require(WithinPixelLimit(1024, 1024, 2u * 1024u * 1024u), "a smaller target is captured");
    Require(!WithinPixelLimit(768, 768, 1024u * 1024u / 2u), "the limit is in pixels, not in a side");

    // The bounded region: the explicit "x,y,width,height", the centred square, the clamp to
    // the target, and the whole target when nothing is requested.
    using AgcDriver::Graphics::TargetCapture::CaptureRegion;
    using AgcDriver::Graphics::TargetCapture::ParseRegion;
    const auto explicitRegion = ParseRegion("10,20,64,64");
    Require(explicitRegion.has_value(), "an explicit region parses");
    Require(explicitRegion->x == 10 && explicitRegion->y == 20 && explicitRegion->width == 64 && explicitRegion->height == 64,
            "the explicit region keeps its values");
    Require(!ParseRegion(nullptr).has_value(), "an unset region is absent");
    Require(!ParseRegion("").has_value(), "an empty region is absent");
    Require(!ParseRegion("10,20,30").has_value(), "a three-value region is absent");
    Require(!ParseRegion("a,b,c,d").has_value(), "a non-numeric region is absent");
    const auto whole = CaptureRegion(3840, 2160, nullptr, 0);
    Require(whole.width == 3840 && whole.height == 2160, "without a request the whole target is captured");
    const auto centred = CaptureRegion(3840, 2160, nullptr, 128);
    Require(centred.width == 128 && centred.height == 128, "the centred square has the requested side");
    Require(centred.x == (3840 - 128) / 2 && centred.y == (2160 - 128) / 2, "the centred square is centred");
    const auto clamped = CaptureRegion(3840, 2160, "3800,2100,640,640", 0);
    Require(clamped.x == 3800 && clamped.y == 2100, "the clamped region keeps its origin");
    Require(clamped.width == 40 && clamped.height == 60, "the clamped region is cut to the target");
    const auto outside = CaptureRegion(3840, 2160, "4000,0,64,64", 0);
    Require(outside.Empty(), "a region outside the target is empty");
    const auto small = CaptureRegion(64, 64, nullptr, 128);
    Require(small.width == 64 && small.height == 64, "the centred square is cut to a small target");
    Require(CaptureRegion(3840, 2160, nullptr, 128).Pixels() == 128ull * 128ull, "the region's pixel count");

    // A schedule without a time never captures.
    Schedule empty({}, 6);
    Require(!empty.ShouldCapture(1, 100.0), "a schedule without a time never captures");

    std::printf("target capture schedule tests passed\n");
    return 0;
}
