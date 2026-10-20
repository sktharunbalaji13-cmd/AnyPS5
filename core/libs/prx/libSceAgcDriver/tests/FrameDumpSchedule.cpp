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

    std::printf("frame dump schedule tests passed\n");
    return 0;
}
