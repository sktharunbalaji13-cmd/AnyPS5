#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"

#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAgcDriverSubmitAcb(uint32_t queue, const Packet* packet) {
    if (queue < 0x20 || queue >= 0x58) {
        throw std::runtime_error(std::string(__func__) + ": unsupported compute queue");
    }
    static const bool trace = std::getenv("APS5_TRACE_SUBMITS") != nullptr;
    if (trace) {
        static std::atomic<std::uint64_t> count{0};
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "[submit-acb] %llu at %lld ms queue=0x%x caller=%p\n", static_cast<unsigned long long>(++count), static_cast<long long>(nowMs), queue, __builtin_return_address(0));
    }
    AgcDriver::Submit(packet, queue);
    return 0;
}

int APS5_VABI sceAgcDriverSubmitMultiAcbs(uint32_t queue, uint32_t* const* acbs, const uint32_t* sizes_in_dwords, uint32_t count) {
    if (count == 0) return 0;
    if (queue < 0x20 || queue >= 0x58) {
        throw std::runtime_error(std::string(__func__) + ": unsupported compute queue");
    }
    return AgcDriver::SubmitMany(acbs, sizes_in_dwords, count, queue);
}

}
