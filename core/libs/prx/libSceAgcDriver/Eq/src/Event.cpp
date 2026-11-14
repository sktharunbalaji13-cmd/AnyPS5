#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"

namespace {

constexpr int16_t EvfiltGraphicsCore = -14;

struct Registration {
    KernelEqueue eq;
    int id;
};

AgcDriver::Mutex g_mutex;
std::vector<Registration> g_registrations;

}

bool AgcDriverDeliverEopInterrupt(std::uint32_t queue) {
    std::vector<Registration> registrations;
    {
        std::lock_guard lock(g_mutex);
        registrations = g_registrations;
    }
    static const bool traceEop = std::getenv("APS5_TRACE_EOP") != nullptr;
    if (traceEop) {
        static std::atomic<std::uint64_t> calls{0};
        const auto n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20 || (n % 10) == 0) std::fprintf(stderr, "[eop] call %llu queue 0x%x registrations %zu\n", static_cast<unsigned long long>(n), queue, registrations.size());
    }
    bool delivered = false;
    for (const auto& registration : registrations) {
        if (registration.id == static_cast<int>(queue)) {
            EqueueTriggerEvent_nid_postfix(registration.eq, static_cast<uintptr_t>(registration.id), EvfiltGraphicsCore, nullptr);
            delivered = true;
        }
    }

    if (traceEop) {
        static std::atomic<std::uint64_t> deliveredCount{0};
        static std::atomic<std::uint64_t> undeliveredCount{0};
        if (delivered) deliveredCount.fetch_add(1, std::memory_order_relaxed);
        else {
            const auto n = undeliveredCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 5) {
                std::fprintf(stderr, "[eop] undelivered queue 0x%x ids:", queue);
                for (const auto& registration : registrations) std::fprintf(stderr, " %d", registration.id);
                std::fprintf(stderr, "\n");
            }
        }
    }
    return delivered;
}

extern "C" {

int APS5_VABI sceAgcDriverAddEqEvent(KernelEqueue eq, int id, void* udata) {
    if (eq == 0 || !EqueuePin_nid_postfix(eq)) {
        throw std::runtime_error(std::string(__func__) + ": invalid event queue");
    }
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EvfiltGraphicsCore;
    event.event.flags = EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    event.filter.triggerFunc = [](KernelEqueueEvent* e, void*) {
        e->event.data = e->triggered ? e->event.data + 1 : 1;
        e->triggered = true;
    };
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        e->triggered = false;
        e->event.data = 0;
    };
    const int result = EqueueAddEvent_nid_postfix(eq, event);
    if (result != EQUEUE_OK) {
        throw std::runtime_error(std::string(__func__) + ": event queue rejected the event");
    }
    std::lock_guard lock(g_mutex);
    if (std::none_of(g_registrations.begin(), g_registrations.end(), [&](const Registration& r) { return r.eq == eq && r.id == id; })) {
        g_registrations.push_back({eq, id});
    }
    return 0;
}

int APS5_VABI sceAgcDriverDeleteEqEvent(KernelEqueue eq, int id) {
    {
        std::lock_guard lock(g_mutex);
        std::erase_if(g_registrations, [&](const Registration& r) { return r.eq == eq && r.id == id; });
    }
    if (EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EvfiltGraphicsCore) != EQUEUE_OK) {
        throw std::runtime_error(std::string(__func__) + ": event is not registered");
    }
    return 0;
}

}
