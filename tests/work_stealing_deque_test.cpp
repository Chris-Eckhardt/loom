#include "work_stealing_deque.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using loom::detail::WorkStealingDeque;

namespace {

void record(std::vector<std::atomic<int>>& seen, std::atomic<int>& garbage, int v) {
    if (v < 0 || v >= static_cast<int>(seen.size())) {
        garbage.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    seen[v].fetch_add(1, std::memory_order_relaxed);
}

unsigned thiefCount() {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 4) {
        n = 4;
    }
    if (n > 8) {
        n = 8;
    }
    return n - 1;  // the owner thread occupies one core
}

TEST(WorkStealingDequeTest, PopOnEmptyReturnsFalse) {
    WorkStealingDeque<int> deque;
    int out = -1;
    EXPECT_FALSE(deque.pop(out));
}

TEST(WorkStealingDequeTest, StealOnEmptyReturnsFalse) {
    WorkStealingDeque<int> deque;
    int out = -1;
    EXPECT_FALSE(deque.steal(out));
}

TEST(WorkStealingDequeTest, PopDoesNotModifyOutOnFailure) {
    WorkStealingDeque<int> deque;
    int out = 12345;
    ASSERT_FALSE(deque.pop(out));
    EXPECT_EQ(out, 12345);
}

TEST(WorkStealingDequeTest, PopReturnsItemsInLifoOrder) {
    WorkStealingDeque<int> deque;
    for (int i = 0; i < 10; ++i) {
        deque.push(i);
    }
    for (int i = 9; i >= 0; --i) {
        int out = -1;
        ASSERT_TRUE(deque.pop(out)) << "at i=" << i;
        EXPECT_EQ(out, i);
    }
    int out = -1;
    EXPECT_FALSE(deque.pop(out));
}

TEST(WorkStealingDequeTest, StealReturnsItemsInFifoOrder) {
    WorkStealingDeque<int> deque;
    for (int i = 0; i < 10; ++i) {
        deque.push(i);
    }
    for (int i = 0; i < 10; ++i) {
        int out = -1;
        ASSERT_TRUE(deque.steal(out)) << "at i=" << i;
        EXPECT_EQ(out, i);
    }
    int out = -1;
    EXPECT_FALSE(deque.steal(out));
}

TEST(WorkStealingDequeTest, PopAndStealMeetInTheMiddle) {
    WorkStealingDeque<int> deque;
    for (int i = 0; i < 6; ++i) {
        deque.push(i);
    }

    int out = -1;
    ASSERT_TRUE(deque.steal(out));
    EXPECT_EQ(out, 0);
    ASSERT_TRUE(deque.pop(out));
    EXPECT_EQ(out, 5);
    ASSERT_TRUE(deque.steal(out));
    EXPECT_EQ(out, 1);
    ASSERT_TRUE(deque.pop(out));
    EXPECT_EQ(out, 4);
    ASSERT_TRUE(deque.steal(out));
    EXPECT_EQ(out, 2);
    ASSERT_TRUE(deque.pop(out));
    EXPECT_EQ(out, 3);

    EXPECT_FALSE(deque.pop(out));
    EXPECT_FALSE(deque.steal(out));
}

TEST(WorkStealingDequeTest, RemainsUsableAfterDrainingToEmpty) {
    WorkStealingDeque<int> deque;
    int out = -1;

    for (int cycle = 0; cycle < 5; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            deque.push(cycle * 100 + i);
        }
        for (int i = 3; i >= 0; --i) {
            ASSERT_TRUE(deque.pop(out));
            ASSERT_EQ(out, cycle * 100 + i);
        }
        ASSERT_FALSE(deque.pop(out)) << "cycle " << cycle;
    }
}

TEST(WorkStealingDequeTest, GrowsBeyondInitialCapacityPreservingLifoOrder) {
    constexpr int kItems = 10000;
    WorkStealingDeque<int> deque(4);

    for (int i = 0; i < kItems; ++i) {
        deque.push(i);
    }
    for (int i = kItems - 1; i >= 0; --i) {
        int out = -1;
        ASSERT_TRUE(deque.pop(out)) << "at i=" << i;
        ASSERT_EQ(out, i);
    }
}

TEST(WorkStealingDequeTest, GrowsBeyondInitialCapacityPreservingFifoOrder) {
    constexpr int kItems = 10000;
    WorkStealingDeque<int> deque(4);

    for (int i = 0; i < kItems; ++i) {
        deque.push(i);
    }
    for (int i = 0; i < kItems; ++i) {
        int out = -1;
        ASSERT_TRUE(deque.steal(out)) << "at i=" << i;
        ASSERT_EQ(out, i);
    }
}

TEST(WorkStealingDequeTest, IndicesWrapCorrectlyOverManyPushPopCycles) {
    WorkStealingDeque<int> deque(4);
    int out = -1;

    for (int i = 0; i < 100000; ++i) {
        deque.push(i);
        ASSERT_TRUE(deque.pop(out)) << "at i=" << i;
        ASSERT_EQ(out, i);
    }
}

TEST(WorkStealingDequeTest, NonPowerOfTwoCapacityIsRoundedUp) {
    WorkStealingDeque<int> deque(100);
    for (int i = 0; i < 128; ++i) {
        deque.push(i);
    }
    for (int i = 127; i >= 0; --i) {
        int out = -1;
        ASSERT_TRUE(deque.pop(out));
        ASSERT_EQ(out, i);
    }
}

TEST(WorkStealingDequeTest, DegenerateCapacityStillWorks) {
    for (std::int64_t cap : {std::int64_t{0}, std::int64_t{1}, std::int64_t{2}}) {
        WorkStealingDeque<int> deque(cap);
        for (int i = 0; i < 100; ++i) {
            deque.push(i);
        }
        for (int i = 99; i >= 0; --i) {
            int out = -1;
            ASSERT_TRUE(deque.pop(out)) << "cap=" << cap << " i=" << i;
            ASSERT_EQ(out, i) << "cap=" << cap;
        }
    }
}

struct Task {
    int  id      = 0;
    int  payload = 0;
    bool flag    = false;

    bool operator==(const Task& o) const {
        return id == o.id && payload == o.payload && flag == o.flag;
    }
};

TEST(WorkStealingDequeTest, CarriesTriviallyCopyableStructs) {
    WorkStealingDeque<Task> deque(4);
    for (int i = 0; i < 500; ++i) {
        deque.push(Task{i, i * 7, i % 2 == 0});
    }
    for (int i = 499; i >= 0; --i) {
        Task out;
        ASSERT_TRUE(deque.pop(out));
        ASSERT_EQ(out, (Task{i, i * 7, i % 2 == 0})) << "at i=" << i;
    }
}

TEST(WorkStealingDequeTest, EveryItemIsConsumedExactlyOnceUnderStealing) {
    constexpr int kItems = 100000;
    const unsigned thieves = thiefCount();

    WorkStealingDeque<int> deque(4);
    std::vector<std::atomic<int>> seen(kItems);
    std::atomic<int> consumed{0};
    std::atomic<int> stolen{0};
    std::atomic<int> garbage{0};
    std::atomic<bool> producing{true};

    std::vector<std::thread> workers;
    workers.reserve(thieves);
    for (unsigned t = 0; t < thieves; ++t) {
        workers.emplace_back([&] {
            int v = 0;
            while (producing.load(std::memory_order_acquire)) {
                if (deque.steal(v)) {
                    record(seen, garbage, v);
                    stolen.fetch_add(1, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    int v = 0;
    for (int i = 0; i < kItems; ++i) {
        deque.push(i);
        if (i % 3 == 0 && deque.pop(v)) {
            record(seen, garbage, v);
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    producing.store(false, std::memory_order_release);

    for (std::thread& w : workers) {
        w.join();
    }
    while (deque.pop(v)) {
        seen[v].fetch_add(1, std::memory_order_relaxed);
        consumed.fetch_add(1, std::memory_order_relaxed);
    }

    EXPECT_EQ(garbage.load(), 0) << "deque returned out-of-range values";
    EXPECT_EQ(consumed.load(), kItems);
    for (int i = 0; i < kItems; ++i) {
        ASSERT_EQ(seen[i].load(), 1) << "value " << i << " was consumed "
                                     << seen[i].load() << " times";
    }

    EXPECT_GT(stolen.load(), kItems / 100)
        << "thieves stole " << stolen.load() << " of " << kItems
        << " items; the concurrent path was barely exercised";
}

TEST(WorkStealingDequeTest, ThievesDrainEverythingWhenOwnerOnlyPushes) {
    constexpr int kItems = 100000;
    const unsigned thieves = thiefCount();

    WorkStealingDeque<int> deque(8);
    std::vector<std::atomic<int>> seen(kItems);
    std::atomic<int> consumed{0};
    std::atomic<int> garbage{0};
    std::atomic<bool> producing{true};

    std::vector<std::thread> workers;
    workers.reserve(thieves);
    for (unsigned t = 0; t < thieves; ++t) {
        workers.emplace_back([&] {
            int v = 0;
            while (producing.load(std::memory_order_acquire) ||
                   consumed.load(std::memory_order_relaxed) < kItems) {
                if (deque.steal(v)) {
                    record(seen, garbage, v);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (int i = 0; i < kItems; ++i) {
        deque.push(i);
    }
    producing.store(false, std::memory_order_release);

    for (std::thread& w : workers) {
        w.join();
    }

    EXPECT_EQ(garbage.load(), 0) << "deque returned out-of-range values";
    EXPECT_EQ(consumed.load(), kItems);
    for (int i = 0; i < kItems; ++i) {
        ASSERT_EQ(seen[i].load(), 1) << "value " << i;
    }
}

TEST(WorkStealingDequeTest, PopAndStealCannotBothTakeTheLastItem) {
    constexpr int kRounds = 20000;

    WorkStealingDeque<int> deque(8);
    std::vector<std::atomic<int>> seen(kRounds);
    std::atomic<int> gate{-1};
    std::atomic<int> thiefDone{-1};
    std::atomic<int> ownerWins{0};
    std::atomic<int> thiefWins{0};
    std::atomic<int> garbage{0};

    std::thread thief([&] {
        for (int r = 0; r < kRounds; ++r) {
            while (gate.load(std::memory_order_acquire) != r) {
                std::this_thread::yield();
            }
            int v = 0;
            if (deque.steal(v)) {
                record(seen, garbage, v);
                thiefWins.fetch_add(1, std::memory_order_relaxed);
            }
            thiefDone.store(r, std::memory_order_release);
        }
    });

    for (int r = 0; r < kRounds; ++r) {
        deque.push(r);
        gate.store(r, std::memory_order_release);

        int v = 0;
        if (deque.pop(v)) {
            ASSERT_EQ(v, r) << "owner popped the wrong value in round " << r;
            record(seen, garbage, v);
            ownerWins.fetch_add(1, std::memory_order_relaxed);
        }

        while (thiefDone.load(std::memory_order_acquire) != r) {
            std::this_thread::yield();
        }
    }
    thief.join();

    EXPECT_EQ(garbage.load(), 0) << "deque returned out-of-range values";
    EXPECT_EQ(ownerWins.load() + thiefWins.load(), kRounds);
    for (int r = 0; r < kRounds; ++r) {
        ASSERT_EQ(seen[r].load(), 1) << "round " << r << " was taken " << seen[r].load() << " times";
    }

    int leftover = 0;
    EXPECT_FALSE(deque.pop(leftover)) << "deque should be empty";
}

}  // namespace
