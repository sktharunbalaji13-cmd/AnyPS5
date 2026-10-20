#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_FRAMEDUMPSCHEDULE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_FRAMEDUMPSCHEDULE_HPP

// Mission 4: selection of the moments at which a renderer-output frame is captured.
//
// APS5_DUMP_FRAMES selects how many frame files are written and APS5_DUMP_FRAMES_EVERY the
// spacing in presents, but neither can select a *time*. This header holds the pure logic
// for an opt-in time list, so it can be unit tested without a GPU.
//
// The list is a comma separated sequence of seconds after the start of the capture
// schedule. Malformed entries are ignored; an unset or unusable list means "no time
// selection", and the caller then keeps the existing count/spacing behaviour unchanged.

#include <algorithm>
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

} // namespace AgcDriver

#endif
