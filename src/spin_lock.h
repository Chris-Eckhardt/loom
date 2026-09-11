#pragma once

#include <atomic>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif


namespace loom::detail {

inline void cpuPause() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    // No-op for unsupported architectures. Fall through to the caller's yield.
#endif
}

class SpinLock {
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        unsigned spins = 0;
        while (m_flag.test_and_set(std::memory_order_acquire)) {
            if (++spins < kSpinLimit) {
                cpuPause();
            } else {
                std::this_thread::yield();
                spins = 0;
            }
        }
    }

    bool tryLock() noexcept {
        return !m_flag.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept {
        m_flag.clear(std::memory_order_release);
    }

private:
    static constexpr unsigned kSpinLimit = 1024;
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept : m_lock(lock) { m_lock.lock(); }
    ~SpinLockGuard() { m_lock.unlock(); }
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& m_lock;
};

} // namespace loom::detail