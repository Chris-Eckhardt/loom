#pragma once

#include <cstddef>


namespace loom::detail {

using FiberHandle = void*;
using FiberFunc = void (*)(void* arg);

FiberHandle convertThreadToFiber() noexcept;
void convertFiberToThread() noexcept;
FiberHandle createFiber(std::size_t stackSize, FiberFunc entry, void* arg);
void deleteFiber(FiberHandle fiber) noexcept;
void switchToFiber(FiberHandle fiber) noexcept;

} // namespace loom::detail
