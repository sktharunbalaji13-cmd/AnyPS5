#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_DIAGTRACE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GENERAL_DIAGTRACE_HPP

// Opt-in, bounded diagnostic tracing used by the Mission 3 instrumentation.
//
// Nothing here runs unless the caller gates it, so the default emulator behaviour and log
// output are unchanged. The channel is deliberately separate from LogMacros.hpp: it adds a
// wall-clock timestamp, a monotonic elapsed time, a thread id and a sequence number while
// leaving every existing log line byte for byte identical.
//
// Controls (all default off / conservative):
//   APS5_DIAG_TIMESTAMPS    add wall clock, elapsed time and thread id to each line
//   APS5_DIAG_EVENTS        cap on detailed events written (default 20000)
//   APS5_DIAG_BYTES         cap on detailed bytes written (default 32 MiB)
//   APS5_DIAG_SUMMARY_SECS  interval of the periodic summary line (default 10)
//   APS5_DIAG_FAILURES      cap on failure lines (default 4096); counted after that
//
// Suppression is never silent: both caps count what they drop and report the counts in the
// periodic summary and in a final summary at exit.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Aps5Diag {

inline unsigned long ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<unsigned long>(::syscall(SYS_gettid));
#endif
}

// Whether the named diagnostic control is set. The answer is per name: the controls are
// independent of each other, so one of them being set never enables another. The call sites
// cache the result in a function-local static, so this runs once per call site.
inline bool Enabled(const char* name) {
    static std::mutex mutex;
    static std::unordered_map<std::string, bool> values;
    const std::lock_guard<std::mutex> lock(mutex);
    const auto found = values.find(name);
    if (found != values.end()) return found->second;
    const bool value = std::getenv(name) != nullptr;
    values.emplace(name, value);
    return value;
}

inline std::uint64_t EnvLimit(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || (end != nullptr && *end != '\0')) return fallback;
    return value == 0 ? fallback : static_cast<std::uint64_t>(value);
}

inline std::uint64_t ElapsedMicros() {
    using Clock = std::chrono::steady_clock;
    static const auto started = Clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
}

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
inline void CivilFromDays(std::int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3u : static_cast<unsigned>(-9));
    year = static_cast<int>(y + (month <= 2));
}

struct ClockAnchor {
    std::int64_t unixMicros;
    std::uint64_t steadyMicros;
};

// Wall clock anchored once to the steady clock so per-event formatting is integer math only.
inline const ClockAnchor& Anchor() {
    static const ClockAnchor anchor = [] {
        const auto wall = std::chrono::system_clock::now().time_since_epoch();
        ClockAnchor value{};
        value.unixMicros = std::chrono::duration_cast<std::chrono::microseconds>(wall).count();
        value.steadyMicros = ElapsedMicros();
        return value;
    }();
    return anchor;
}

inline int FormatWall(char* out, std::size_t capacity) {
    const ClockAnchor& anchor = Anchor();
    const std::int64_t micros =
        anchor.unixMicros + static_cast<std::int64_t>(ElapsedMicros() - anchor.steadyMicros);
    const std::int64_t seconds = micros / 1000000;
    const std::int64_t fraction = micros % 1000000;
    const std::int64_t days = seconds / 86400;
    const std::int64_t rest = seconds % 86400;
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDays(days, year, month, day);
    return std::snprintf(out, capacity, "%04d-%02u-%02uT%02lld:%02lld:%02lld.%06lldZ", year, month, day,
                         static_cast<long long>(rest / 3600), static_cast<long long>((rest / 60) % 60),
                         static_cast<long long>(rest % 60), static_cast<long long>(fraction));
}

struct FileAggregate {
    std::uint64_t opens = 0;
    std::uint64_t reads = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t lastActivityMicros = 0;
};

struct OpenInfo {
    int id = 0;
    std::string guestPath;
};

struct State {
    std::mutex mutex;
    std::atomic<std::uint64_t> eventsWritten{0};
    std::atomic<std::uint64_t> bytesWritten{0};
    std::atomic<std::uint64_t> suppressedEvents{0};
    std::atomic<std::uint64_t> suppressedBytes{0};
    std::atomic<std::uint64_t> failuresWritten{0};
    std::atomic<std::uint64_t> suppressedFailures{0};
    std::atomic<std::uint64_t> sequence{0};
    std::uint64_t eventLimit = 0;
    std::uint64_t byteLimit = 0;
    std::uint64_t failureLimit = 0;
    std::uint64_t summarySeconds = 10;
    bool timestamps = false;
    bool initialised = false;
    std::unordered_map<int, OpenInfo> openPaths;
    std::unordered_map<std::string, FileAggregate> files;
    std::uint64_t cappedPaths = 0;
    std::uint64_t lastSummaryMicros = 0;
    std::atomic<bool> summaryStarted{false};
};

inline State& Get() {
    static State* const state = new State();
    return *state;
}

inline void WriteRaw(const char* text, std::size_t length) {
    if (length == 0) return;
    std::fwrite(text, 1, length, stderr);
    std::fflush(stderr);
}

inline void SummaryLineLocked(State& state, const char* reason);

inline void EnsureInit(State& state) {
    if (state.initialised) return;
    state.timestamps = Enabled("APS5_DIAG_TIMESTAMPS");
    state.eventLimit = EnvLimit("APS5_DIAG_EVENTS", 20000);
    state.byteLimit = EnvLimit("APS5_DIAG_BYTES", 33554432);
    state.failureLimit = EnvLimit("APS5_DIAG_FAILURES", 4096);
    state.summarySeconds = EnvLimit("APS5_DIAG_SUMMARY_SECS", 10);
    state.lastSummaryMicros = ElapsedMicros();
    state.initialised = true;
    static const bool installed = [] {
        std::atexit(+[] {
            State& s = Get();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.initialised) return;
            SummaryLineLocked(s, "final");
        });
        return true;
    }();
    (void)installed;
}

inline void SummaryThread() {
    State& state = Get();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(static_cast<long long>(state.summarySeconds)));
        std::lock_guard<std::mutex> lock(state.mutex);
        SummaryLineLocked(state, "periodic");
    }
}

inline void StartSummaryThread() {
    State& state = Get();
    if (state.summaryStarted.exchange(true)) return;
    std::thread(SummaryThread).detach();
}

inline void WritePrefix(State& state, bool failure, const char* category, char* prefix, std::size_t capacity) {
    const std::uint64_t sequence = state.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (state.timestamps) {
        char wall[40];
        FormatWall(wall, sizeof(wall));
        const std::uint64_t micros = ElapsedMicros();
        std::snprintf(prefix, capacity, "[diag %s%s +%llu.%06llu s %s tid=%lu seq=%llu] ", category,
                      failure ? " FAIL" : "", static_cast<unsigned long long>(micros / 1000000),
                      static_cast<unsigned long long>(micros % 1000000), wall, ThreadId(),
                      static_cast<unsigned long long>(sequence));
    } else {
        std::snprintf(prefix, capacity, "[diag %s%s seq=%llu] ", category, failure ? " FAIL" : "",
                      static_cast<unsigned long long>(sequence));
    }
}

inline void EmitText(State& state, bool failure, const char* category, const char* text, std::size_t length) {
    char prefix[192];
    WritePrefix(state, failure, category, prefix, sizeof(prefix));
    const std::size_t prefixLength = std::strlen(prefix);
    if (failure) {
        if (state.failuresWritten.load(std::memory_order_relaxed) >= state.failureLimit) {
            state.suppressedFailures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } else if (state.eventsWritten.load(std::memory_order_relaxed) >= state.eventLimit ||
               state.bytesWritten.load(std::memory_order_relaxed) + length + prefixLength > state.byteLimit) {
        state.suppressedEvents.fetch_add(1, std::memory_order_relaxed);
        state.suppressedBytes.fetch_add(length, std::memory_order_relaxed);
        return;
    }
    WriteRaw(prefix, prefixLength);
    WriteRaw(text, length);
    WriteRaw("\n", 1);
    if (failure) {
        state.failuresWritten.fetch_add(1, std::memory_order_relaxed);
    } else {
        state.eventsWritten.fetch_add(1, std::memory_order_relaxed);
        state.bytesWritten.fetch_add(length + prefixLength + 1, std::memory_order_relaxed);
    }
}

inline void Emit(State& state, bool failure, const char* category, const char* format, va_list arguments) {
    char small[1024];
    va_list copy;
    va_copy(copy, arguments);
    const int length = std::vsnprintf(small, sizeof(small), format, arguments);
    if (length < 0) {
        va_end(copy);
        return;
    }
    if (length < static_cast<int>(sizeof(small))) {
        EmitText(state, failure, category, small, static_cast<std::size_t>(length));
        va_end(copy);
        return;
    }
    std::vector<char> large(static_cast<std::size_t>(length) + 1);
    std::vsnprintf(large.data(), large.size(), format, copy);
    va_end(copy);
    EmitText(state, failure, category, large.data(), static_cast<std::size_t>(length));
}

[[gnu::format(printf, 2, 3)]]
inline void Event(const char* category, const char* format, ...) {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    EnsureInit(state);
    va_list arguments;
    va_start(arguments, format);
    Emit(state, false, category, format, arguments);
    va_end(arguments);
    StartSummaryThread();
}

// A failure line is preserved past the detailed-event cap, bounded by APS5_DIAG_FAILURES.
[[gnu::format(printf, 2, 3)]]
inline void Failure(const char* category, const char* format, ...) {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    EnsureInit(state);
    va_list arguments;
    va_start(arguments, format);
    Emit(state, true, category, format, arguments);
    va_end(arguments);
    StartSummaryThread();
}

// ---- file correlation ---------------------------------------------------------------

inline int RegisterOpen(const std::string& guestPath) {
    State& state = Get();
    static std::atomic<int> nextId{0};
    const int id = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> lock(state.mutex);
    EnsureInit(state);
    if (state.files.size() < 4096) {
        FileAggregate& aggregate = state.files[guestPath];
        aggregate.opens += 1;
        aggregate.lastActivityMicros = ElapsedMicros();
    } else {
        state.cappedPaths += 1;
    }
    return id;
}

inline void RegisterDescriptor(int descriptor, int id, const std::string& guestPath) {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.openPaths.size() < 8192) state.openPaths[descriptor] = OpenInfo{id, guestPath};
}

inline void ForgetDescriptor(int descriptor) {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.openPaths.erase(descriptor);
}

inline OpenInfo DescriptorInfo(int descriptor) {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.openPaths.find(descriptor);
    return found == state.openPaths.end() ? OpenInfo{} : found->second;
}

inline void AccountRead(const std::string& guestPath, std::uint64_t bytes, bool error) {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.files.find(guestPath);
    if (found == state.files.end()) {
        state.cappedPaths += 1;
        return;
    }
    found->second.reads += 1;
    found->second.bytes += bytes;
    if (error) found->second.errors += 1;
    found->second.lastActivityMicros = ElapsedMicros();
}

// Totals plus the busiest files. Does not modify the table.
inline void SummaryLineLocked(State& state, const char* reason) {
    const std::uint64_t now = ElapsedMicros();
    state.lastSummaryMicros = now;
    char head[640];
    std::snprintf(
        head, sizeof(head),
        "[diag summary %s +%llu.%06llu s tid=%lu] events=%llu bytes=%llu suppressed_events=%llu "
        "suppressed_bytes=%llu failures=%llu suppressed_failures=%llu files=%zu capped_paths=%llu\n",
        reason, static_cast<unsigned long long>(now / 1000000), static_cast<unsigned long long>(now % 1000000),
        ThreadId(), static_cast<unsigned long long>(state.eventsWritten.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.bytesWritten.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.suppressedEvents.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.suppressedBytes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.failuresWritten.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.suppressedFailures.load(std::memory_order_relaxed)),
        state.files.size(), static_cast<unsigned long long>(state.cappedPaths));
    WriteRaw(head, std::strlen(head));

    std::vector<const std::pair<const std::string, FileAggregate>*> best;
    for (const auto& entry : state.files) {
        if (entry.second.reads == 0) continue;
        best.push_back(&entry);
        for (std::size_t index = best.size(); index > 1; --index) {
            if (best[index - 1]->second.bytes > best[index - 2]->second.bytes)
                std::swap(best[index - 1], best[index - 2]);
            else
                break;
        }
        if (best.size() > 8) best.pop_back();
    }
    for (const auto* entry : best) {
        char line[1200];
        const int length = std::snprintf(
            line, sizeof(line),
            "[diag summary file] reads=%llu bytes=%llu opens=%llu errors=%llu idle_ms=%llu path=%s\n",
            static_cast<unsigned long long>(entry->second.reads),
            static_cast<unsigned long long>(entry->second.bytes),
            static_cast<unsigned long long>(entry->second.opens),
            static_cast<unsigned long long>(entry->second.errors),
            static_cast<unsigned long long>((now - entry->second.lastActivityMicros) / 1000),
            entry->first.c_str());
        if (length > 0) WriteRaw(line, static_cast<std::size_t>(length));
    }
}

} // namespace Aps5Diag

#endif
