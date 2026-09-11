#include "spin_lock.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

using loom::detail::cpuPause;
using loom::detail::SpinLock;
using loom::detail::SpinLockGuard;

namespace {

unsigned workerCount() {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 4) {
        n = 4;
    }
    if (n > 16) {
        n = 16;
    }
    return n;
}

bool tryLockFromAnotherThread(SpinLock& lock) {
    bool acquired = false;
    std::thread t([&] {
        acquired = lock.tryLock();
        if (acquired) {
            lock.unlock();
        }
    });
    t.join();
    return acquired;
}

TEST(SpinLockTest, TryLockSucceedsOnAFreshLock) {
    SpinLock lock;
    EXPECT_TRUE(lock.tryLock());
    lock.unlock();
}

TEST(SpinLockTest, TryLockFailsWhileHeld) {
    SpinLock lock;
    lock.lock();
    EXPECT_FALSE(tryLockFromAnotherThread(lock));
    lock.unlock();
}

TEST(SpinLockTest, TryLockIsNotRecursive) {
    SpinLock lock;
    lock.lock();
    EXPECT_FALSE(lock.tryLock()) << "the holding thread must not re-enter";
    lock.unlock();
}

TEST(SpinLockTest, UnlockMakesTheLockAvailableAgain) {
    SpinLock lock;
    lock.lock();
    ASSERT_FALSE(tryLockFromAnotherThread(lock));
    lock.unlock();
    EXPECT_TRUE(tryLockFromAnotherThread(lock));
}

TEST(SpinLockTest, LockUnlockCyclesRepeatedly) {
    SpinLock lock;
    for (int i = 0; i < 1000; ++i) {
        lock.lock();
        lock.unlock();
    }
    EXPECT_TRUE(lock.tryLock());
    lock.unlock();
}

TEST(SpinLockTest, TryLockFailsRepeatedlyWhileHeldThenSucceeds) {
    SpinLock lock;
    lock.lock();
    for (int i = 0; i < 100; ++i) {
        ASSERT_FALSE(tryLockFromAnotherThread(lock)) << "attempt " << i;
    }
    lock.unlock();
    EXPECT_TRUE(tryLockFromAnotherThread(lock));
}

TEST(SpinLockGuardTest, AcquiresOnConstructionAndReleasesOnScopeExit) {
    SpinLock lock;
    {
        SpinLockGuard guard(lock);
        EXPECT_FALSE(tryLockFromAnotherThread(lock));
    }
    EXPECT_TRUE(tryLockFromAnotherThread(lock));
}

TEST(SpinLockGuardTest, ReleasesWhileUnwinding) {
    SpinLock lock;
    struct Boom {};

    try {
        SpinLockGuard guard(lock);
        ASSERT_FALSE(tryLockFromAnotherThread(lock));
        throw Boom{};
    } catch (const Boom&) {
    }

    EXPECT_TRUE(tryLockFromAnotherThread(lock))
        << "guard did not release during stack unwinding";
}

TEST(SpinLockTest, MutualExclusionUnderContention) {
    constexpr int kIncrements = 10000;
    const unsigned workers = workerCount();

    SpinLock lock;
    long long counter = 0;  // deliberately not atomic

    std::atomic<int> occupancy{0};
    std::atomic<int> overlaps{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int k = 0; k < kIncrements; ++k) {
                lock.lock();
                if (occupancy.fetch_add(1, std::memory_order_relaxed) != 0) {
                    overlaps.fetch_add(1, std::memory_order_relaxed);
                }
                ++counter;
                occupancy.fetch_sub(1, std::memory_order_relaxed);
                lock.unlock();
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(overlaps.load(), 0) << "two threads were inside the critical section";
    EXPECT_EQ(counter, static_cast<long long>(workers) * kIncrements);
}

TEST(SpinLockGuardTest, ProvidesMutualExclusionUnderContention) {
    constexpr int kIncrements = 5000;
    const unsigned workers = workerCount();

    SpinLock lock;
    long long counter = 0;
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int k = 0; k < kIncrements; ++k) {
                SpinLockGuard guard(lock);
                ++counter;
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(counter, static_cast<long long>(workers) * kIncrements);
}

TEST(SpinLockTest, WritesBeforeUnlockAreVisibleToTheNextAcquirer) {
    constexpr int kPayload = 256;
    constexpr int kRounds  = 200;

    for (int round = 0; round < kRounds; ++round) {
        SpinLock lock;
        std::vector<int> payload;
        bool ready = false;

        std::thread producer([&] {
            lock.lock();
            payload.resize(kPayload);
            for (int i = 0; i < kPayload; ++i) {
                payload[i] = i + 1;
            }
            ready = true;
            lock.unlock();
        });

        std::vector<int> seen;
        for (;;) {
            lock.lock();
            const bool done = ready;
            if (done) {
                seen = payload;
            }
            lock.unlock();
            if (done) {
                break;
            }
            std::this_thread::yield();
        }
        producer.join();

        ASSERT_EQ(seen.size(), static_cast<std::size_t>(kPayload)) << "round " << round;
        for (int i = 0; i < kPayload; ++i) {
            ASSERT_EQ(seen[i], i + 1) << "round " << round << ", index " << i;
        }
    }
}

TEST(SpinLockTest, WaitersAcquireAfterAHoldLongerThanTheSpinLimit) {
    const unsigned workers = workerCount();

    SpinLock lock;
    std::atomic<unsigned> acquired{0};
    std::atomic<unsigned> waiting{0};

    lock.lock();

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            waiting.fetch_add(1, std::memory_order_release);
            lock.lock();
            acquired.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
        });
    }

    while (waiting.load(std::memory_order_acquire) < workers) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(acquired.load(), 0u) << "a waiter entered while the lock was held";

    lock.unlock();
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(acquired.load(), workers);
}

TEST(CpuPauseTest, IsCallableOnThisArchitecture) {
    for (int i = 0; i < 1000; ++i) {
        cpuPause();
    }
    SUCCEED();
}

}  // namespace
