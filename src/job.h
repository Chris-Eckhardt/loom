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

struct JobSystemDesc {
    unsigned numWorkerThreads = 0;
    unsigned numFibers = 128;
    std::size_t fiberStackSize = 512 * 1024;
    bool pinThreadsToCores = false;
};

} // namespace loom
