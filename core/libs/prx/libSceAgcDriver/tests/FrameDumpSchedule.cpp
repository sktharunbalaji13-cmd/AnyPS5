// Mission 4: unit test for the frame-dump time schedule (pure logic, no GPU).

#include "prx/libSceAgcDriver/Execution/include/FrameDumpSchedule.hpp"

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
    using AgcDriver::DumpDue;
    using AgcDriver::ParseDumpTimes;

    Require(ParseDumpTimes(nullptr).empty(), "an unset schedule is empty");
    Require(ParseDumpTimes("").empty(), "an empty schedule is empty");
    Require(ParseDumpTimes("   ").empty(), "a blank schedule is empty");
    Require(ParseDumpTimes("abc").empty(), "a non-numeric schedule is empty");

    const auto times = ParseDumpTimes("10,40,65,90,120,175");
    Require(times.size() == 6, "six entries are parsed");
    Require(times[0] == 10.0 && times[5] == 175.0, "the values are kept in order");

    const auto mixed = ParseDumpTimes("65,10,oops,-5,0,40");
    Require(mixed.size() == 3, "invalid and non-positive entries are dropped");
    Require(mixed[0] == 10.0 && mixed[1] == 40.0 && mixed[2] == 65.0, "the remaining entries are sorted");

    // Selection: each scheduled point fires once, in order, and only after its time.
    std::size_t index = 0;
    Require(!DumpDue(times, 0.0, index), "nothing is due before the first point");
    Require(!DumpDue(times, 9.9, index), "nothing is due just before the first point");
    Require(DumpDue(times, 10.0, index), "the first point fires at its time");
    Require(!DumpDue(times, 10.1, index), "the first point does not fire twice");
    Require(!DumpDue(times, 39.9, index), "the second point waits for its time");
    Require(DumpDue(times, 40.0, index), "the second point fires at its time");
    Require(DumpDue(times, 65.0, index), "a later point fires late rather than not at all");
    Require(index == 3, "the index advanced once per fired point");

    // Once the list is exhausted nothing more is due.
    for (double t = 65.0; t < 200.0; t += 1.0) DumpDue(times, t, index);
    Require(index == times.size(), "the index ends at the end of the list");
    Require(!DumpDue(times, 500.0, index), "no capture is due after the last point");

    // An empty schedule never fires, which is what keeps the existing behaviour intact.
    std::size_t emptyIndex = 0;
    Require(!DumpDue({}, 10.0, emptyIndex), "an empty schedule never fires");

    // The capture plan: which presents are captured, what the capture is called and what
    // happens to the index when a present is dropped.
    {
        using AgcDriver::CapturePlan;

        // Without a limit nothing is captured at all.
        CapturePlan plan;
        Require(!plan.Enabled(), "a plan without a limit is disabled");
        Require(!plan.Reserve(0.0), "a plan without a limit captures nothing");

        // The count rule: every present, up to the limit.
        plan = CapturePlan{};
        plan.limit = 4;
        Require(plan.Reserve(0.0), "the first present is captured");
        Require(plan.Index() == 0, "the first capture is frame 0");
        plan.Commit();
        Require(plan.Reserve(0.1), "the second present is captured");
        plan.Commit();
        Require(plan.Reserve(0.2), "the third present is captured");
        plan.Commit();
        Require(plan.Reserve(0.3), "the fourth present is captured");
        plan.Commit();
        Require(!plan.Reserve(0.4), "the limit stops the captures");
        Require(plan.Index() == 4, "the index counts the captures");

        // The spacing in presents.
        plan = CapturePlan{};
        plan.limit = 10;
        plan.every = 3;
        Require(!plan.Reserve(0.0), "the first present is not captured with every=3");
        Require(!plan.Reserve(0.1), "the second present is not captured with every=3");
        Require(plan.Reserve(0.2), "the third present is captured with every=3");
        plan.Commit();

        // A dropped present gives its index back: the numbering stays contiguous and the
        // limit still counts the files that were written.
        plan = CapturePlan{};
        plan.limit = 2;
        Require(plan.Reserve(0.0), "the capture is reserved");
        const int reserved = plan.Index();
        plan.Release();
        Require(plan.Index() == reserved, "a dropped present keeps the index");
        Require(plan.Reserve(0.1), "the next present is captured");
        Require(plan.Index() == reserved, "the dropped present did not consume an index");
        plan.Commit();
        Require(plan.Index() == reserved + 1, "the committed capture consumed the index");
        Require(plan.Reserve(0.2), "the limit still allows a second capture");
        plan.Commit();
        Require(!plan.Reserve(0.3), "the limit counts the captures that were written");

        // A list of capture times replaces the count rule, but the run's first frame is always
        // captured: a game that stalls before its first capture time still leaves evidence.
        plan = CapturePlan{};
        plan.limit = 40;
        plan.at = ParseDumpTimes("8,9,10");
        Require(plan.Reserve(1.0), "the run's first frame is captured");
        Require(plan.Index() == 0, "the first frame is frame 0");
        plan.Commit();
        Require(!plan.Reserve(2.0), "a present before the first capture time is not captured");
        Require(!plan.Reserve(7.9), "a present just before the first capture time is not captured");
        Require(plan.Reserve(8.0), "the present at the first capture time is captured");
        Require(plan.Index() == 1, "the scheduled capture is the next file");
        plan.Commit();
        Require(!plan.Reserve(8.5), "a capture time fires once");
        Require(plan.Reserve(9.0), "the present at the second capture time is captured");
        plan.Commit();
        Require(plan.Reserve(10.0), "the present at the third capture time is captured");
        plan.Commit();
        Require(!plan.Reserve(11.0), "no capture time is left");

        // A dropped first frame is still owed: the next present captures it.
        plan = CapturePlan{};
        plan.limit = 40;
        plan.at = ParseDumpTimes("8");
        Require(plan.Reserve(1.0), "the run's first frame is captured");
        plan.Release();
        Require(plan.Reserve(2.0), "the dropped first frame is captured by the next present");
        Require(plan.Index() == 0, "the dropped first frame kept the index");
        plan.Commit();
        Require(!plan.Reserve(3.0), "the first frame is not captured twice");

        // A dropped scheduled capture gives the capture time back: the moment is captured by
        // the next present instead of being lost.
        plan = CapturePlan{};
        plan.limit = 40;
        plan.at = ParseDumpTimes("8");
        Require(plan.Reserve(1.0), "the run's first frame is captured");
        plan.Commit();
        Require(plan.Reserve(8.0), "the scheduled capture is reserved");
        plan.Release();
        Require(plan.Reserve(8.1), "the dropped scheduled capture is captured by the next present");
        Require(plan.Index() == 1, "the dropped scheduled capture kept its index");
    }

    std::printf("frame dump schedule tests passed\n");
    return 0;
}
