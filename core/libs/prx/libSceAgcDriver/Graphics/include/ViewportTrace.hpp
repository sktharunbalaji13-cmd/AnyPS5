#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VIEWPORTTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VIEWPORTTRACE_HPP

// Mission 8: bounded, opt-in tracing of the viewport's provenance.
//
// The viewport the driver hands to Vulkan is computed from six context registers (the clip
// viewport scale and offset). This trace records where those values come from, what they
// decode to, which branch the driver takes, and what finally reaches vkCmdSetViewport — so a
// zero-sized viewport can be told apart from a default, a denormal, a missing register and a
// reporting artefact.
//
// Everything is off by default and bounded: aggregate counters for every decode, detailed
// records for a bounded sample.

#include "prx/libc/include/general/DiagTrace.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace AgcDriver {
namespace Graphics {
namespace ViewportTrace {

// The clip viewport registers of one context: six DWORDs, the scale/offset pairs.
inline constexpr std::uint32_t ViewportRegisterBase = 0x10f;
inline constexpr std::uint32_t ViewportRegisterCount = 6;
inline constexpr std::uint32_t ViewportContextCount = 16;

// The scale registers (X, Y, Z) and the offset registers (X, Y, Z) of the clip viewport.
inline constexpr std::uint32_t ScaleX = 0x10f;
inline constexpr std::uint32_t OffsetX = 0x110;
inline constexpr std::uint32_t ScaleY = 0x111;
inline constexpr std::uint32_t OffsetY = 0x112;
inline constexpr std::uint32_t ScaleZ = 0x113;
inline constexpr std::uint32_t OffsetZ = 0x114;

// Whether an offset belongs to the clip viewport of any context.
inline bool IsViewportRegister(std::uint32_t offset) {
    if (offset < ViewportRegisterBase) return false;
    const auto index = offset - ViewportRegisterBase;
    return index < ViewportRegisterCount * ViewportContextCount;
}

// The context a viewport register belongs to (0 is the first context).
inline std::uint32_t ViewportContext(std::uint32_t offset) {
    return (offset - ViewportRegisterBase) / ViewportRegisterCount;
}

// Whether the register is a scale (even index) or an offset (odd index).
inline bool IsScaleRegister(std::uint32_t offset) {
    return ((offset - ViewportRegisterBase) % ViewportRegisterCount) % 2 == 0;
}

// The classification of a decoded register value: the distinction the mission asks for.
enum class Value : std::uint8_t {
    Zero,        // exactly zero
    Denormal,    // a subnormal float: a fixed-point value read as a float
    Normal,      // an ordinary finite value
    NonFinite,   // not finite (the driver rejects these)
};

inline const char* ValueName(Value value) {
    switch (value) {
    case Value::Zero: return "zero";
    case Value::Denormal: return "denormal";
    case Value::Normal: return "normal";
    case Value::NonFinite: return "non-finite";
    }
    return "unknown";
}

inline Value Classify(float value) {
    if (!std::isfinite(value)) return Value::NonFinite;
    if (value == 0.0f) return Value::Zero;
    if (std::fpclassify(value) == FP_SUBNORMAL) return Value::Denormal;
    return Value::Normal;
}

// The raw bits of a float, so a genuine zero can be told from an uninitialised or
// mis-decoded bit pattern.
inline std::uint32_t Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// The formatting of the viewport's dimensions, as text: the value and its raw bits. A test
// checks this formatting against known values.
inline std::string FormatFloat(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g/0x%08x", static_cast<double>(value), Bits(value));
    return buffer;
}

// The control: APS5_DIAG_VIEWPORT (presence) switches the trace on.
inline bool Enabled() {
    static const bool on = Aps5Diag::Enabled("APS5_DIAG_VIEWPORT");
    return on;
}

// The counters, one per observed fact.
struct Counters {
    std::atomic<std::uint64_t> decodes{0};
    std::atomic<std::uint64_t> packetWrites{0};
    std::atomic<std::uint64_t> snapshotWrites{0};
    std::atomic<std::uint64_t> collapsed{0};
    std::atomic<std::uint64_t> normal{0};
    std::atomic<std::uint64_t> zeroScale{0};
    std::atomic<std::uint64_t> denormalScale{0};
    std::atomic<std::uint64_t> defaultScale{0};
    std::atomic<std::uint64_t> zeroWidth{0};
    std::atomic<std::uint64_t> zeroHeight{0};
    std::atomic<std::uint64_t> detailed{0};
    std::atomic<std::uint64_t> suppressed{0};
};

inline Counters& Stats() {
    static Counters counters;
    return counters;
}

// The sample selection: the first draws, every draw whose viewport is degenerate (until the
// budget), and a sparse set of later draws.
class Selection {
public:
    enum class Reason : std::uint8_t { None, First, Degenerate, Later };

    bool Admit(bool degenerate) {
        const auto taken = Stats().detailed.load(std::memory_order_relaxed);
        if (taken >= DetailLimit()) {
            Stats().suppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Reason reason = Reason::None;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto count = Stats().decodes.load(std::memory_order_relaxed);
            const auto index = count > 0 ? count - 1 : 0;
            if (index < FirstDraws) {
                reason = Reason::First;
            } else if (degenerate) {
                reason = Reason::Degenerate;
            } else if (index % LaterStride == 0) {
                reason = Reason::Later;
            }
        }
        if (reason == Reason::None) return false;
        Stats().detailed.fetch_add(1, std::memory_order_relaxed);
        lastReason = reason;
        return true;
    }

    static constexpr std::uint64_t FirstDraws = 64;
    static constexpr std::uint64_t LaterStride = 1000;

    static std::uint64_t DetailLimit() {
        static const std::uint64_t limit = std::min<std::uint64_t>(
            Aps5Diag::EnvLimit("APS5_DIAG_VIEWPORT_MAX", 4096), 65536);
        return limit;
    }

    const char* ReasonName() const {
        switch (lastReason) {
        case Reason::First: return "first";
        case Reason::Degenerate: return "degenerate";
        case Reason::Later: return "later";
        default: return "none";
        }
    }

private:
    std::mutex mutex;
    Reason lastReason = Reason::None;
};

inline Selection& Sampler() {
    static Selection selection;
    return selection;
}

// The guest write: a packet or a shader-state restore wrote a clip viewport register.
inline void WriteEvent(const char* origin, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    if (!Enabled()) return;
    auto& counters = Stats();
    counters.packetWrites.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<std::uint64_t> written{0};
    const auto index = written.fetch_add(1, std::memory_order_relaxed);
    if (index >= 256) {
        counters.suppressed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float decoded = std::bit_cast<float>(value);
    Aps5Diag::Event("viewport",
                    "event=write origin=%s opcode=0x%x offset=0x%x context=%u kind=%s value=0x%08x decoded=%s class=%s",
                    origin, opcode, offset, ViewportContext(offset), IsScaleRegister(offset) ? "scale" : "offset",
                    value, FormatFloat(decoded).c_str(), ValueName(Classify(decoded)));
}

} // namespace ViewportTrace
} // namespace Graphics
} // namespace AgcDriver

#endif
