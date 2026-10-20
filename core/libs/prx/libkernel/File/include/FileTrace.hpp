#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_INCLUDE_FILETRACE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_INCLUDE_FILETRACE_HPP

// Mission 3 file-I/O diagnostics (opt-in, APS5_DIAG_IO).
//
// These helpers are observation only: they never change a return value, a descriptor, an
// offset or a branch. When APS5_DIAG_IO is unset every helper returns immediately after one
// cached boolean test, so the default behaviour and log output are unchanged.
//
// The correlation identifier is the open instance. The fd layer has no open-file object, so
// the map is kept in Aps5Diag and is best effort for descriptors duplicated with dup: a
// duplicated descriptor inherits the path of its source. Descriptor reuse after close is
// handled by forgetting the entry on a successful close; a mapping that was never
// registered logs with id=0 and the path shown as empty.

#include "prx/libc/include/general/DiagTrace.hpp"

#include <cstdint>
#include <string>

namespace FileTrace {

inline bool Enabled() {
    static const bool on = Aps5Diag::Enabled("APS5_DIAG_IO");
    return on;
}

inline void Open(const char* guestPath, const std::string& hostPath, int result, bool isFile) {
    if (!Enabled()) return;
    const char* path = guestPath != nullptr ? guestPath : "<null>";
    if (result < 0) {
        Aps5Diag::Failure("io", "open path=%s result=0x%08X", path, static_cast<unsigned>(result));
        return;
    }
    if (!isFile) {
        Aps5Diag::Event("io", "open path=%s fd=%d kind=nonfile", path, result);
        return;
    }
    const int id = Aps5Diag::RegisterOpen(path);
    Aps5Diag::RegisterDescriptor(result, id, path);
    Aps5Diag::Event("io", "open id=%d fd=%d path=%s host=%s", id, result, path, hostPath.c_str());
}

inline void Read(const char* op, int descriptor, std::uint64_t requested, std::int64_t result, std::int64_t offset) {
    if (!Enabled()) return;
    const Aps5Diag::OpenInfo info = Aps5Diag::DescriptorInfo(descriptor);
    const bool failed = result < 0;
    const std::uint64_t returned = failed ? 0 : static_cast<std::uint64_t>(result);
    if (!info.guestPath.empty()) Aps5Diag::AccountRead(info.guestPath, returned, failed);
    if (failed) {
        Aps5Diag::Failure("io", "%s id=%d fd=%d path=%s offset=%lld requested=%llu result=0x%08X", op, info.id,
                          descriptor, info.guestPath.c_str(), static_cast<long long>(offset),
                          static_cast<unsigned long long>(requested), static_cast<unsigned>(result));
        return;
    }
    const bool shortRead = returned < requested;
    Aps5Diag::Event("io", "%s id=%d fd=%d path=%s offset=%lld requested=%llu returned=%llu%s", op, info.id,
                    descriptor, info.guestPath.c_str(), static_cast<long long>(offset),
                    static_cast<unsigned long long>(requested), static_cast<unsigned long long>(returned),
                    shortRead ? " short=1" : "");
}

inline void Close(int descriptor, int result) {
    if (!Enabled()) return;
    const Aps5Diag::OpenInfo info = Aps5Diag::DescriptorInfo(descriptor);
    if (info.id != 0) {
        Aps5Diag::Event("io", "close id=%d fd=%d path=%s result=%d", info.id, descriptor, info.guestPath.c_str(),
                        result);
    }
    if (result == 0) Aps5Diag::ForgetDescriptor(descriptor);
}

inline void Seek(int descriptor, int whence, std::int64_t offset, std::int64_t result, bool failed) {
    if (!Enabled()) return;
    const Aps5Diag::OpenInfo info = Aps5Diag::DescriptorInfo(descriptor);
    if (info.id == 0) return;
    Aps5Diag::Event("io", "lseek id=%d fd=%d path=%s whence=%d offset=%lld result=%lld%s", info.id, descriptor,
                    info.guestPath.c_str(), whence, static_cast<long long>(offset), static_cast<long long>(result),
                    failed ? " failed=1" : "");
}

// Command-buffer (APR) reads have no descriptor; the path is the correlation key.
inline void AprRead(const char* hostPath, std::int64_t offset, std::uint64_t size, std::int64_t returned,
                    bool failed) {
    if (!Enabled()) return;
    if (failed) {
        Aps5Diag::Failure("io", "apr-read path=%s offset=%lld requested=%llu result=0x%08X", hostPath,
                          static_cast<long long>(offset), static_cast<unsigned long long>(size),
                          static_cast<unsigned>(returned));
        return;
    }
    Aps5Diag::AccountRead(hostPath != nullptr ? hostPath : "<null>", static_cast<std::uint64_t>(returned), false);
    Aps5Diag::Event("io", "apr-read path=%s offset=%lld requested=%llu returned=%llu%s", hostPath,
                    static_cast<long long>(offset), static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(returned),
                    static_cast<std::uint64_t>(returned) < size ? " short=1" : "");
}

} // namespace FileTrace

#endif
