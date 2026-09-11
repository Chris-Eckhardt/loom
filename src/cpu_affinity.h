#pragma once

#include <cstdint>
#include <vector>


namespace loom::detail {

struct CoreId {
    std::uint16_t group = 0;
    std::uint64_t mask  = 0;
};

std::vector<CoreId> enumeratePhysicalCores();
bool pinCurrentThreadToCore(const CoreId& core);

} // namespace loom::detail
