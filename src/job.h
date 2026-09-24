#pragma once

#include <cstddef>
#include <cstdint>

namespace loom {

using JobEntry = void (*)(void* arg);

struct JobDecl {
    JobEntry entry = nullptr;
    void* arg = nullptr;

    JobDecl() = default;
    JobDecl(JobEntry e, void* a)
        : entry(e)
        , arg(a) {}
};

enum class JobPriority : std::uint8_t {
    High = 0,
    Normal = 1,
    Low = 2,
};

enum class ThreadAffinity : std::uint8_t {
    Any = 0,
    Main = 1,
};

struct CpuCore {
    std::uint16_t group = 0;
    std::uint64_t mask = 0;
};

struct JobSystemDesc {
    unsigned numWorkerThreads = 0;
    unsigned numFibers = 128;
    std::size_t fiberStackSize = 512 * 1024;
    bool pinThreadsToCores = false;
    unsigned reservedCores = 0;
};

} // namespace loom
