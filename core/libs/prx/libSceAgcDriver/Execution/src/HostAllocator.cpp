#ifdef _WIN32

#define PSAPI_VERSION 2

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <malloc.h>
#include <new>
#include <windows.h>
#include <psapi.h>

namespace {

constexpr std::size_t Granule = 16;
constexpr std::size_t LargestCached = 1024;
constexpr std::size_t Classes = LargestCached / Granule;

constexpr std::uint16_t Depth(std::size_t sizeClass) {
    return sizeClass < 256 / Granule ? 64 : 16;
}

struct Cache {
    void* heads[Classes];
    std::uint16_t counts[Classes];
};

constexpr std::uint16_t KeptLists = 8;

struct Depot {
    SRWLOCK lock = SRWLOCK_INIT;
    void* lists = nullptr;
    std::uint16_t count = 0;
};

Depot depots[Classes];

bool KeepList(std::size_t sizeClass, void* list) noexcept {
    auto& depot = depots[sizeClass];
    AcquireSRWLockExclusive(&depot.lock);
    const bool kept = depot.count < KeptLists;
    if (kept) {
        static_cast<void**>(list)[1] = depot.lists;
        depot.lists = list;
        ++depot.count;
    }
    ReleaseSRWLockExclusive(&depot.lock);
    return kept;
}

void* TakeList(std::size_t sizeClass) noexcept {
    auto& depot = depots[sizeClass];
    AcquireSRWLockExclusive(&depot.lock);
    void* list = depot.lists;
    if (list != nullptr) {
        depot.lists = static_cast<void**>(list)[1];
        --depot.count;
    }
    ReleaseSRWLockExclusive(&depot.lock);
    return list;
}

Cache* const Closed = reinterpret_cast<Cache*>(std::uintptr_t{1});

constexpr unsigned long TlsSlotsOffset = 0x1480;
constexpr DWORD DirectSlots = 64;

constexpr DWORD Unset = TLS_OUT_OF_INDEXES;
constexpr DWORD Failed = TLS_OUT_OF_INDEXES - 1;
std::atomic<DWORD> tlsSlot{Unset};
std::atomic<DWORD> flsSlot{FLS_OUT_OF_INDEXES};

void Drain(Cache* cache) noexcept {
    for (std::size_t sizeClass = 0; sizeClass < Classes; ++sizeClass) {
        while (void* block = cache->heads[sizeClass]) {
            cache->heads[sizeClass] = *static_cast<void**>(block);
            std::free(block);
        }
        cache->counts[sizeClass] = 0;
    }
}

void WINAPI ThreadEnded(void* data) {
    auto* cache = static_cast<Cache*>(data);
    if (cache == nullptr || cache == Closed) return;
    const auto slot = tlsSlot.load(std::memory_order_acquire);
    if (slot < Failed) TlsSetValue(slot, Closed);
    Drain(cache);
    std::free(cache);
}

DWORD Slot() noexcept {
    auto slot = tlsSlot.load(std::memory_order_acquire);
    if (slot != Unset) return slot;
    const DWORD error = GetLastError();
    const DWORD fls = FlsAlloc(ThreadEnded);
    DWORD tls = fls == FLS_OUT_OF_INDEXES ? TLS_OUT_OF_INDEXES : TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES || tls >= Failed) {
        if (tls != TLS_OUT_OF_INDEXES) TlsFree(tls);
        if (fls != FLS_OUT_OF_INDEXES) FlsFree(fls);
        tls = Failed;
    }
    DWORD expected = Unset;
    if (tlsSlot.compare_exchange_strong(expected, tls, std::memory_order_acq_rel)) {
        if (tls != Failed) flsSlot.store(fls, std::memory_order_release);
        SetLastError(error);
        return tls;
    }
    if (tls != Failed) {
        TlsFree(tls);
        FlsFree(fls);
    }
    SetLastError(error);
    return expected;
}

Cache* SlotValue(DWORD slot) noexcept {
    if (slot < DirectSlots) return reinterpret_cast<Cache*>(__readgsqword(TlsSlotsOffset + slot * sizeof(void*)));
    const DWORD error = GetLastError();
    auto* value = static_cast<Cache*>(TlsGetValue(slot));
    SetLastError(error);
    return value;
}

Cache* CreateCache(DWORD slot) noexcept {
    const auto fls = flsSlot.load(std::memory_order_acquire);
    if (fls == FLS_OUT_OF_INDEXES) return nullptr;
    auto* cache = static_cast<Cache*>(std::calloc(1, sizeof(Cache)));
    if (cache == nullptr) return nullptr;
    const DWORD error = GetLastError();
    if (!FlsSetValue(fls, cache) || !TlsSetValue(slot, cache)) {
        FlsSetValue(fls, nullptr);
        std::free(cache);
        cache = nullptr;
    }
    SetLastError(error);
    return cache;
}

Cache* CurrentCache() noexcept {
    const auto slot = Slot();
    if (slot == Failed) return nullptr;
    auto* cache = SlotValue(slot);
    if (cache == Closed) return nullptr;
    return cache != nullptr ? cache : CreateCache(slot);
}

void* TryAllocate(std::size_t bytes) noexcept {
    if (bytes == 0) bytes = 1;
    if (bytes <= LargestCached) {
        const auto sizeClass = (bytes - 1) / Granule;
        if (auto* cache = CurrentCache()) {
            if (cache->heads[sizeClass] == nullptr) {
                if (void* list = TakeList(sizeClass)) {
                    cache->heads[sizeClass] = list;
                    cache->counts[sizeClass] = Depth(sizeClass);
                }
            }
            if (void* block = cache->heads[sizeClass]) {
                cache->heads[sizeClass] = *static_cast<void**>(block);
                --cache->counts[sizeClass];
                return block;
            }
        }
        return std::malloc((sizeClass + 1) * Granule);
    }
    return std::malloc(bytes);
}

std::atomic<std::uint64_t> hostedBytes{0};
std::atomic<int> exhaustionReports{0};

void* Allocate(std::size_t bytes);

void ReportExhaustion(std::size_t bytes) noexcept {
    if (exhaustionReports.exchange(1, std::memory_order_relaxed) != 0) return;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    const bool counted = K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0;
    PERFORMANCE_INFORMATION system{};
    system.cb = sizeof(system);
    const bool measured = K32GetPerformanceInfo(&system, sizeof(system)) != 0;
    std::fprintf(stderr, "[alloc] HostAllocator::Allocate failed for %zu bytes (0x%zx): allocations over %zu bytes hold %llu MiB; process private %llu MiB, working set %llu MiB, peak private %llu MiB; system commit %llu of %llu MiB, physical %llu MiB with %llu MiB available\n",
        bytes, bytes, LargestCached, static_cast<unsigned long long>(hostedBytes.load(std::memory_order_relaxed) >> 20u),
        counted ? static_cast<unsigned long long>(counters.PrivateUsage >> 20u) : 0ull,
        counted ? static_cast<unsigned long long>(counters.WorkingSetSize >> 20u) : 0ull,
        counted ? static_cast<unsigned long long>(counters.PeakPagefileUsage >> 20u) : 0ull,
        measured ? static_cast<unsigned long long>(system.CommitTotal * system.PageSize >> 20u) : 0ull,
        measured ? static_cast<unsigned long long>(system.CommitLimit * system.PageSize >> 20u) : 0ull,
        measured ? static_cast<unsigned long long>(system.PhysicalTotal * system.PageSize >> 20u) : 0ull,
        measured ? static_cast<unsigned long long>(system.PhysicalAvailable * system.PageSize >> 20u) : 0ull);
    void* frames[32]{};
    const auto depth = static_cast<std::size_t>(RtlCaptureStackBackTrace(2, 32, frames, nullptr));
    std::fprintf(stderr, "[alloc] caller chain from operator new at %p, called from %p:", reinterpret_cast<void*>(&Allocate), __builtin_return_address(1));
    for (std::size_t index = 0; index < depth; ++index) std::fprintf(stderr, " %p", frames[index]);
    std::fprintf(stderr, "\n");
}

void* Allocate(std::size_t bytes) {
    for (;;) {
        if (void* block = TryAllocate(bytes)) {
            if (bytes > LargestCached) hostedBytes.fetch_add(bytes, std::memory_order_relaxed);
            return block;
        }
        const auto handler = std::get_new_handler();
        if (handler == nullptr) {
            ReportExhaustion(bytes);
            throw std::bad_alloc();
        }
        ReportExhaustion(bytes);
        handler();
    }
}

void Release(void* block, std::size_t bytes = 0) noexcept {
    if (block == nullptr) return;
    const auto usable = bytes != 0 && bytes % Granule == 0 ? bytes : _msize(block);
    if (auto* cache = CurrentCache()) {
        if (usable >= Granule && usable < LargestCached + Granule) {
            const auto sizeClass = usable / Granule - 1;
            if (cache->counts[sizeClass] == Depth(sizeClass) && KeepList(sizeClass, cache->heads[sizeClass])) {
                cache->heads[sizeClass] = nullptr;
                cache->counts[sizeClass] = 0;
            }
            if (cache->counts[sizeClass] < Depth(sizeClass)) {
                *static_cast<void**>(block) = cache->heads[sizeClass];
                cache->heads[sizeClass] = block;
                ++cache->counts[sizeClass];
                return;
            }
        }
    }
    if (usable > LargestCached) hostedBytes.fetch_sub(usable, std::memory_order_relaxed);
    std::free(block);
}

}

void* operator new(std::size_t bytes) { return Allocate(bytes); }
void* operator new[](std::size_t bytes) { return Allocate(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return Allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* block) noexcept { Release(block); }
void operator delete[](void* block) noexcept { Release(block); }
void operator delete(void* block, std::size_t bytes) noexcept { Release(block, bytes); }
void operator delete[](void* block, std::size_t bytes) noexcept { Release(block, bytes); }
void operator delete(void* block, const std::nothrow_t&) noexcept { Release(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { Release(block); }

#endif
