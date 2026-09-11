#include "fiber.h"

#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>


namespace loom::detail {

namespace {

struct FiberBox {
    FiberFunc fn;
    void*     arg;
};

VOID WINAPI fiberTrampoline(LPVOID param) {
    auto* box = static_cast<FiberBox*>(param);
    box->fn(box->arg);
}

} // namespace

FiberHandle convertThreadToFiber() noexcept {
    return ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
}

void convertFiberToThread() noexcept {
    ConvertFiberToThread();
}

FiberHandle createFiber(
    std::size_t stackSize, 
    FiberFunc entry, 
    void* arg
) {
    auto* box = new FiberBox{entry, arg};
    LPVOID fiber = CreateFiberEx(
        0, 
        stackSize,
        FIBER_FLAG_FLOAT_SWITCH,
        fiberTrampoline,
        box
    );

    if (fiber == nullptr) {
        delete box;
        throw std::bad_alloc();
    }

    return fiber;
}

void deleteFiber(FiberHandle fiber) noexcept {
    if (fiber != nullptr) {
        DeleteFiber(fiber);
    }
}

void switchToFiber(FiberHandle fiber) noexcept {
    SwitchToFiber(fiber);
}

} // namespace loom::detail

#else // POSIX: ucontext

#include <ucontext.h>

namespace loom::detail {

namespace {

struct UContextFiber {
    ucontext_t ctx;
    void*      stack     = nullptr;
    std::size_t stackSize = 0;
    FiberFunc  fn        = nullptr;
    void*      arg       = nullptr;
    bool       ownsStack = false;
};

thread_local UContextFiber* tCurrent = nullptr;

void ucontextTrampoline(unsigned hi, unsigned lo) {
    auto packed = (static_cast<std::uintptr_t>(hi) << 32) |
                  static_cast<std::uintptr_t>(lo);
    auto* self = reinterpret_cast<UContextFiber*>(packed);
    self->fn(self->arg);
}

} // namespace

FiberHandle convertThreadToFiber() noexcept {
    auto* f = new (std::nothrow) UContextFiber();
    if (f == nullptr) return nullptr;
    getcontext(&f->ctx);
    tCurrent = f;
    return f;
}

void convertFiberToThread() noexcept {
    delete tCurrent;
    tCurrent = nullptr;
}

FiberHandle createFiber(
    std::size_t stackSize,
    FiberFunc entry, 
    void* arg
) {
    auto* f = new UContextFiber();
    f->stack     = std::malloc(stackSize);
    if (f->stack == nullptr) {
        delete f;
        throw std::bad_alloc();
    }
    f->stackSize = stackSize;
    f->fn        = entry;
    f->arg       = arg;
    f->ownsStack = true;

    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp   = f->stack;
    f->ctx.uc_stack.ss_size = stackSize;
    f->ctx.uc_link          = nullptr;

    auto packed = reinterpret_cast<std::uintptr_t>(f);
    auto hi = static_cast<unsigned>(packed >> 32);
    auto lo = static_cast<unsigned>(packed & 0xFFFFFFFFu);
    makecontext(&f->ctx, reinterpret_cast<void (*)()>(ucontextTrampoline), 2, hi, lo);
    return f;
}

void deleteFiber(FiberHandle fiber) noexcept {
    auto* f = static_cast<UContextFiber*>(fiber);
    if (f == nullptr) return;
    if (f->ownsStack) std::free(f->stack);
    delete f;
}

void switchToFiber(FiberHandle fiber) noexcept {
    auto* target = static_cast<UContextFiber*>(fiber);
    auto* from   = tCurrent;
    tCurrent = target;
    swapcontext(&from->ctx, &target->ctx);
    tCurrent = from;
}

} // namespace loom::detail

#endif
