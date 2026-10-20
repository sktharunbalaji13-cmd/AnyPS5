#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TARGETCAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TARGETCAPTURE_HPP

// Mission 6: bounded, read-only render-target pixel capture.
//
// The copy is recorded into the SAME command buffer as the draw it samples, so the capture
// observes the target exactly as the draw left it, and the data is read only after that
// command batch's fence has signalled (the batch completion action). Nothing here changes a
// runtime decision: the draw stays recorded and asynchronous, the target stays resident, and
// the draw-cache recipe path stays enabled.
//
// Controls (all off by default):
//   APS5_DIAG_TARGET_CAPTURE  presence: enables the capture path
//   APS5_DIAG_TARGET_AT      comma separated seconds; at most one capture per target per time
//   APS5_DIAG_TARGET_MAX     total capture limit (default 6, hard maximum 6)

#include "prx/libc/include/general/DiagTrace.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AgcDriver {
namespace Graphics {
namespace TargetCapture {

struct Request {
    std::uint64_t targetId = 0;
    std::uint64_t drawId = 0;
    std::uint64_t batch = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t guestFormat = 0;
    std::uint32_t vkFormat = 0;
    std::uint32_t bytesPerPixel = 0;
    double requestedAt = 0.0;
    // The region of the target the capture took (Mission 9).
    std::uint32_t regionX = 0;
    std::uint32_t regionY = 0;
    std::uint32_t regionWidth = 0;
    std::uint32_t regionHeight = 0;
};

// The file layout written by a capture:
//   offset 0   : u32 width
//   offset 4   : u32 height
//   offset 8   : u32 VkFormat of the image (the storage format)
//   offset 12  : u32 bytes per pixel
//   offset 16  : the raw texels, tightly packed, top-down, in the image's channel order
inline void WriteFile(const std::string& name, const Request& request, const unsigned char* pixels, std::size_t bytes) {
    std::FILE* file = std::fopen(name.c_str(), "wb");
    if (file == nullptr) {
        Aps5Diag::Failure("target-capture", "open failed name=%s", name.c_str());
        return;
    }
    const std::uint32_t header[4] = {request.width, request.height, request.vkFormat, request.bytesPerPixel};
    std::fwrite(header, sizeof(header), 1, file);
    if (bytes != 0) std::fwrite(pixels, 1, bytes, file);
    std::fclose(file);
}

// The largest target the capture may take, in pixels (width * height). Zero means no limit,
// which is the default and keeps Mission 6's behaviour exactly. A target larger than the limit
// is skipped and the skip is reported, so "no capture" is never silent.
inline std::uint64_t MaxPixels() {
    static const std::uint64_t limit = Aps5Diag::EnvLimit("APS5_DIAG_TARGET_MAX_PIXELS", 0);
    return limit;
}

// How many targets are captured at each scheduled time (default 2, hard maximum 6). The due
// targets are taken in ascending size, so a smaller target comes before a full-size one.
inline std::uint32_t PerTimeLimit() {
    static const std::uint32_t limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        Aps5Diag::EnvLimit("APS5_DIAG_TARGET_PER_TIME", 2), 6));
    return limit;
}

// A region of a target: the origin and the size, in pixels.
struct Region {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    std::uint64_t Pixels() const { return static_cast<std::uint64_t>(width) * height; }
    bool Empty() const { return width == 0 || height == 0; }
};

// The region the capture takes. The explicit region is "x,y,width,height"; the centred square
// is "size,size" at the target's centre; without either the whole target is captured.
inline std::optional<Region> ParseRegion(const char* text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    std::vector<std::uint32_t> values;
    const std::string value(text);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string::npos ? value.size() : comma;
        const std::string item = value.substr(start, end - start);
        char* parsed = nullptr;
        const unsigned long number = std::strtoul(item.c_str(), &parsed, 10);
        if (parsed == nullptr || parsed == item.c_str() || *parsed != '\0') return std::nullopt;
        values.push_back(static_cast<std::uint32_t>(number));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (values.size() == 4) return Region{values[0], values[1], values[2], values[3]};
    if (values.size() == 2) return Region{0, 0, values[0], values[1]};
    return std::nullopt;
}

// The region the capture takes for a target of the given size: the requested region clamped to
// the target, or a centred square of the requested side, or the whole target.
inline Region CaptureRegion(std::uint32_t width, std::uint32_t height, const char* explicitRegion, std::uint32_t side) {
    if (const auto requested = ParseRegion(explicitRegion)) {
        Region region = *requested;
        if (region.x >= width || region.y >= height) return Region{};
        region.width = std::min(region.width, width - region.x);
        region.height = std::min(region.height, height - region.y);
        return region;
    }
    if (side != 0 && width != 0 && height != 0) {
        const auto extent = std::min(side, std::min(width, height));
        return Region{(width - extent) / 2, (height - extent) / 2, extent, extent};
    }
    return Region{0, 0, width, height};
}

// Whether a target of this size may be captured under the given pixel limit. A limit of zero
// means no limit, which is the default.
inline bool WithinPixelLimit(std::uint32_t width, std::uint32_t height, std::uint64_t limit) {
    if (limit == 0) return true;
    return static_cast<std::uint64_t>(width) * height <= limit;
}

// The capture times (seconds) parsed from a comma-separated list: the positive numbers in
// ascending order, each one once. A malformed entry is ignored rather than aborting the title.
inline std::vector<double> ParseTimes(const char* text) {
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
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

class Schedule {
public:
    // The process-wide schedule, built from the environment. It is created once.
    static Schedule& Get() {
        static Schedule* const instance = new Schedule();
        return *instance;
    }

    // A schedule with the given capture times and cap.
    explicit Schedule(std::vector<double> captureTimes, std::uint32_t cap)
        : enabled(true), times(std::move(captureTimes)), maxCaptures(cap) {}

    bool Enabled() const { return enabled; }

    // True when a capture of this target is due at `elapsedSeconds` and every cap allows it.
    bool ShouldCapture(std::uint64_t targetId, double elapsedSeconds) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled || times.empty()) return false;
        if (captures >= maxCaptures) return false;
        const auto perTarget = perTargetCounts.find(targetId);
        if (perTarget != perTargetCounts.end() && perTarget->second >= times.size()) return false;
        // The capture is due once the elapsed time has reached the next scheduled point that
        // this target has not been sampled for yet.
        std::size_t taken = perTarget == perTargetCounts.end() ? 0 : perTarget->second;
        if (taken >= times.size()) return false;
        if (elapsedSeconds < times[taken]) return false;
        return true;
    }

    void MarkCaptured(std::uint64_t targetId) {
        std::lock_guard<std::mutex> lock(mutex);
        ++captures;
        ++perTargetCounts[targetId];
    }

    std::uint64_t Captures() const {
        std::lock_guard<std::mutex> lock(mutex);
        return captures;
    }

    std::uint32_t MaxCaptures() const { return maxCaptures; }

private:
    Schedule() {
        enabled = Aps5Diag::Enabled("APS5_DIAG_TARGET_CAPTURE");
        times = ParseTimes(std::getenv("APS5_DIAG_TARGET_AT"));
        const std::uint64_t requested = Aps5Diag::EnvLimit("APS5_DIAG_TARGET_MAX", 6);
        maxCaptures = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, 6));
    }

    mutable std::mutex mutex;
    bool enabled = false;
    std::vector<double> times;
    std::unordered_map<std::uint64_t, std::size_t> perTargetCounts;
    std::uint64_t captures = 0;
    std::uint32_t maxCaptures = 6;
};

} // namespace TargetCapture
} // namespace Graphics
} // namespace AgcDriver

#endif
