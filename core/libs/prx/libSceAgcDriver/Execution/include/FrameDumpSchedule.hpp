#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_FRAMEDUMPSCHEDULE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_FRAMEDUMPSCHEDULE_HPP

// Mission 4: selection of the moments at which a renderer-output frame is captured.
//
// APS5_DUMP_FRAMES selects how many frame files are written and APS5_DUMP_FRAMES_EVERY the
// spacing in presents, but neither can select a *time*. This header holds the pure logic
// for an opt-in time list, so it can be unit tested without a GPU.
//
// The list is a comma separated sequence of seconds after the start of the renderer.
// Malformed entries are ignored; an unset or unusable list means "no time selection", and
// the caller then keeps the existing count/spacing behaviour unchanged.
//
// The capture plan below is the whole decision: which presents are captured, what the
// capture is called and what happens to the file index when a present is dropped. The
// renderer asks the plan and hands a reserved index back when the present does not happen.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace AgcDriver {

// Parses "10,40,65" into {10,40,65}. Entries that are not strictly positive numbers are
// ignored. Returns an empty vector when nothing usable was given.
inline std::vector<double> ParseDumpTimes(const char* text) {
    std::vector<double> times;
    if (text == nullptr || *text == '\0') return times;
    const std::string value(text);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string::npos ? value.size() : comma;
        const std::string item = value.substr(start, end - start);
        char* parsed = nullptr;
        const double seconds = std::strtod(item.c_str(), &parsed);
        if (parsed != nullptr && parsed != item.c_str() && seconds > 0.0) times.push_back(seconds);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    std::sort(times.begin(), times.end());
    return times;
}

// True when the next scheduled capture point has been reached. nextIndex is advanced so
// that each scheduled point fires once and in order.
inline bool DumpDue(const std::vector<double>& schedule, double elapsedSeconds, std::size_t& nextIndex) {
    if (schedule.empty()) return false;
    if (nextIndex >= schedule.size()) return false;
    if (elapsedSeconds < schedule[nextIndex]) return false;
    ++nextIndex;
    return true;
}

namespace detail {
// The start of the renderer. The capture times are counted from it and not from the first
// presented frame: the game may take seconds to render its first frame (the Ghost run
// presented its first frame 8 s after the start), and a schedule counted from that frame
// would reach every capture point that much too late.
inline const std::chrono::steady_clock::time_point kRunStart = std::chrono::steady_clock::now();
}

// The seconds since the renderer started.
inline double RunElapsedSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - detail::kRunStart).count();
}

// The capture plan of a run: how many files it may write, which presents it captures and
// the index every capture writes.
//
// The renderer reserves a capture before it records the readback, and commits it once the
// present is on its way. A present that is dropped (or that fails) releases the reserved
// index, so the numbering stays contiguous and the limit still counts the files that were
// written.
struct CapturePlan {
    int limit = 0;          // APS5_DUMP_FRAMES: the number of files the run may write
    int every = 1;          // APS5_DUMP_FRAMES_EVERY: the count rule's spacing in presents
    bool cpu = false;       // APS5_NO_GPU_DUMP: the frame is decoded on the CPU, not read back
    std::vector<double> at; // APS5_DUMP_FRAMES_AT: the capture times, in seconds
    std::size_t nextAt = 0; // the capture time that is still to come
    std::size_t reservedNextAt = 0; // the capture time the reserved capture consumed
    std::uint64_t presents = 0; // the presents the plan has seen
    int dumped = 0;         // the captures taken: the index the next capture writes
    bool started = false;   // a present has been seen
    bool reserved = false;  // an index is reserved for the present in progress
    bool reservedFirst = false; // the reserved capture is the run's first frame

    bool Enabled() const noexcept { return limit > 0; }

    // Reserves the capture for the present in progress. On true the capture is written as
    // Index(); the reservation is given back with Release() or consumed with Commit().
    bool Reserve(double elapsedSeconds) {
        if (limit <= 0 || dumped >= limit) return false;
        ++presents;
        const bool first = !started;
        started = true;
        const auto nextAtBefore = nextAt;
        const bool count = every > 0 && (presents % static_cast<std::uint64_t>(every)) == 0;
        const bool time = !at.empty() && DumpDue(at, elapsedSeconds, nextAt);
        // A list of capture times replaces the count rule; without one the count rule decides.
        const bool due = at.empty() ? count : time;
        // The run's first presented frame is always captured when the times decide. A game
        // that stalls before its first capture time would otherwise leave no evidence at all
        // of what the renderer produced, which is the one thing the dumps are for.
        if (!due && !(first && !at.empty())) return false;
        reserved = true;
        reservedFirst = first;
        reservedNextAt = nextAtBefore;
        return true;
    }

    // The index the reserved capture writes.
    int Index() const noexcept { return dumped; }

    // The capture is on its way to a file: the index is consumed.
    void Commit() noexcept {
        if (!reserved) return;
        ++dumped;
        reserved = false;
        reservedFirst = false;
    }

    // The present was dropped: the reserved index and the capture time it consumed are given
    // back, so that the dropped moment is captured by the next present.
    void Release() noexcept {
        if (!reserved) return;
        reserved = false;
        nextAt = reservedNextAt;
        if (reservedFirst) started = false; // the run's first frame is still owed
        reservedFirst = false;
    }
};

// The plan of a run, from the environment.
inline CapturePlan CapturePlanFromEnvironment() {
    CapturePlan plan;
    const char* limit = std::getenv("APS5_DUMP_FRAMES");
    plan.limit = limit != nullptr ? std::atoi(limit) : 0;
    const char* every = std::getenv("APS5_DUMP_FRAMES_EVERY");
    plan.every = every != nullptr ? std::max(std::atoi(every), 1) : 1;
    plan.cpu = std::getenv("APS5_NO_GPU_DUMP") != nullptr;
    plan.at = ParseDumpTimes(std::getenv("APS5_DUMP_FRAMES_AT"));
    return plan;
}

} // namespace AgcDriver

#endif
