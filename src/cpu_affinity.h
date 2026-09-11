#pragma once

#include <cstdint>
#include <vector>


namespace loom::detail {

struct CoreId {
    std::uint16_t group = 0;
    std::uint64_t mask  = 0;
};

std::vector<CoreId> enumeratePhysicalCores();

// Returns true only if the calling thread is now restricted to `core`.
// An empty mask, or a platform without affinity support, returns false.
bool pinCurrentThreadToCore(const CoreId& core);

} // namespace loom::detail
