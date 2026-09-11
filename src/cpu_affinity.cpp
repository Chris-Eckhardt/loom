#if defined(__linux__)
// Must precede any libc header for CPU_SET / pthread_setaffinity_np.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "cpu_affinity.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>


namespace loom::detail {

std::vector<CoreId> enumeratePhysicalCores() {
    std::vector<CoreId> cores;

    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) 
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER 
        || length == 0
    ) {
        return cores;
    }

    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &length)
    ) {
        return cores;
    }

    unsigned char* ptr = buffer.data();
    unsigned char* end = buffer.data() + length;

    while (ptr < end) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);

        if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1) {
            const GROUP_AFFINITY& ga = info->Processor.GroupMask[0];

            if (ga.Mask != 0) {
                const KAFFINITY lowest = ga.Mask & (~ga.Mask + 1);
                CoreId c;
                c.group = ga.Group;
                c.mask  = static_cast<std::uint64_t>(lowest);
                cores.push_back(c);
            }
        }

        ptr += info->Size;
    }
    return cores;
}

bool pinCurrentThreadToCore(const CoreId& core) {
    // SetThreadGroupAffinity succeeds for an empty mask and leaves the thread
    // unpinned, which would report success for a no-op. Reject it so the
    // return value means the same thing on every platform.
    if (core.mask == 0) {
        return false;
    }

    GROUP_AFFINITY ga = {};
    ga.Group = static_cast<WORD>(core.group);
    ga.Mask  = static_cast<KAFFINITY>(core.mask);
    return SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
}

} // namespace loom::detail

#else // POSIX

#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace loom::detail {

std::vector<CoreId> enumeratePhysicalCores() {
    std::vector<CoreId> cores;
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    if (n < 1) {
        n = 1;
    }

    for (long i = 0; i < n && i < 64; ++i) {
        CoreId c;
        c.group = 0;
        c.mask  = static_cast<std::uint64_t>(1) << i;
        cores.push_back(c);
    }

    return cores;
}

bool pinCurrentThreadToCore(const CoreId& core) {
    if (core.mask == 0) {
        return false;
    }

#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);

    for (int i = 0; i < 64; ++i) {
        if (core.mask & (static_cast<std::uint64_t>(1) << i)) {
            CPU_SET(i, &set);
        }
    }

    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core;
    return false; // pinning unsupported
#endif
}

} // namespace loom::detail

#endif
