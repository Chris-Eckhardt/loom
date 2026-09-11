#pragma once

#include <cstddef>
#include <cstdint>

namespace loom {

struct JobSystemDesc {
    unsigned numWorkerThreads = 0;
    unsigned numFibers = 128;
    std::size_t fiberStackSize = 512 * 1024;
    bool pinThreadsToCores = false;
};

} // namespace loom
