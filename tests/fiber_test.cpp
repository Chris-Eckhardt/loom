#include "fiber.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <vector>

using loom::detail::convertFiberToThread;
using loom::detail::convertThreadToFiber;
using loom::detail::createFiber;
using loom::detail::deleteFiber;
using loom::detail::FiberHandle;
using loom::detail::switchToFiber;

namespace {

constexpr std::size_t kStackSize = 256 * 1024;

class FiberTest : public ::testing::Test {
protected:
    void SetUp() override {
        main_ = convertThreadToFiber();
        ASSERT_NE(main_, nullptr);
    }

    void TearDown() override {
        convertFiberToThread();
        main_ = nullptr;
    }

    FiberHandle main_ = nullptr;
};

struct RoundTrip {
    FiberHandle main = nullptr;
    int         ran  = 0;
};

void roundTripEntry(void* arg) {
    auto* c = static_cast<RoundTrip*>(arg);
    c->ran += 1;
    switchToFiber(c->main);
}

TEST_F(FiberTest, EntryRunsOnceOnFirstSwitch) {
    RoundTrip c{main_, 0};
    FiberHandle f = createFiber(kStackSize, roundTripEntry, &c);
    ASSERT_NE(f, nullptr);

    EXPECT_EQ(c.ran, 0) << "createFiber must not run the entry point";
    switchToFiber(f);
    EXPECT_EQ(c.ran, 1);

    deleteFiber(f);
}

struct ArgCheck {
    FiberHandle main = nullptr;
    void*       seen = nullptr;
};

void argCheckEntry(void* arg) {
    auto* c = static_cast<ArgCheck*>(arg);
    c->seen = arg;
    switchToFiber(c->main);
}

TEST_F(FiberTest, EntryReceivesExactArgPointer) {
    ArgCheck c{main_, nullptr};
    FiberHandle f = createFiber(kStackSize, argCheckEntry, &c);
    ASSERT_NE(f, nullptr);

    switchToFiber(f);
    EXPECT_EQ(c.seen, static_cast<void*>(&c));

    deleteFiber(f);
}

struct PingPong {
    FiberHandle       main       = nullptr;
    int               iterations = 0;
    std::vector<int>* log        = nullptr;
};

void pingPongEntry(void* arg) {
    auto* c = static_cast<PingPong*>(arg);
    for (int i = 0; i < c->iterations; ++i) {
        c->log->push_back(i);
        switchToFiber(c->main);
    }
    switchToFiber(c->main);
}

TEST_F(FiberTest, PingPongResumesWhereItLeftOff) {
    constexpr int kIterations = 8;

    std::vector<int> log;
    PingPong c{main_, kIterations, &log};
    FiberHandle f = createFiber(kStackSize, pingPongEntry, &c);
    ASSERT_NE(f, nullptr);

    for (int i = 0; i < kIterations; ++i) {
        switchToFiber(f);
        log.push_back(1000 + i);
    }

    std::vector<int> expected;
    for (int i = 0; i < kIterations; ++i) {
        expected.push_back(i);
        expected.push_back(1000 + i);
    }
    EXPECT_EQ(log, expected);

    deleteFiber(f);
}

struct Chain {
    FiberHandle      main = nullptr;
    FiberHandle      b    = nullptr;
    FiberHandle      c    = nullptr;
    std::vector<int> order;
};

void chainEntryA(void* arg) {
    auto* s = static_cast<Chain*>(arg);
    s->order.push_back(1);
    switchToFiber(s->b);
    switchToFiber(s->main);
}

void chainEntryB(void* arg) {
    auto* s = static_cast<Chain*>(arg);
    s->order.push_back(2);
    switchToFiber(s->c);
    switchToFiber(s->main);
}

void chainEntryC(void* arg) {
    auto* s = static_cast<Chain*>(arg);
    s->order.push_back(3);
    switchToFiber(s->main);
}

TEST_F(FiberTest, ChainedSwitchesReturnControlToMain) {
    Chain s;
    s.main = main_;

    FiberHandle a = createFiber(kStackSize, chainEntryA, &s);
    s.b = createFiber(kStackSize, chainEntryB, &s);
    s.c = createFiber(kStackSize, chainEntryC, &s);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(s.b, nullptr);
    ASSERT_NE(s.c, nullptr);

    switchToFiber(a);
    EXPECT_EQ(s.order, (std::vector<int>{1, 2, 3}));

    deleteFiber(s.c);
    deleteFiber(s.b);
    deleteFiber(a);
}

constexpr unsigned kBurnDepth = 64;  // ~64 KiB of frames on a 256 KiB stack

// `volatile` keeps the padding from being optimised away, so each frame
// really does consume ~1 KiB of the fiber's stack.
unsigned long long burnStack(unsigned depth) {
    volatile unsigned char frame[1024];
    frame[0]    = static_cast<unsigned char>(depth);
    frame[1023] = 1u;

    if (depth == 0) {
        return frame[0];
    }
    return static_cast<unsigned long long>(frame[0]) + frame[1023] + burnStack(depth - 1);
}

struct Deep {
    FiberHandle        main   = nullptr;
    unsigned long long result = 0;
};

void deepEntry(void* arg) {
    auto* c = static_cast<Deep*>(arg);
    c->result = burnStack(kBurnDepth);
    switchToFiber(c->main);
}

TEST_F(FiberTest, DeepRecursionStaysWithinRequestedStack) {
    // sum over d in [1, kBurnDepth] of (d + 1)
    constexpr unsigned long long kExpected = (kBurnDepth * (kBurnDepth + 1ull)) / 2 + kBurnDepth;

    Deep c{main_, 0};
    FiberHandle f = createFiber(kStackSize, deepEntry, &c);
    ASSERT_NE(f, nullptr);

    switchToFiber(f);
    EXPECT_EQ(c.result, kExpected);

    deleteFiber(f);
}

struct Counter {
    FiberHandle main = nullptr;
    int         id   = 0;
    int*        seen = nullptr;
};

void counterEntry(void* arg) {
    auto* c = static_cast<Counter*>(arg);
    *c->seen += c->id;
    switchToFiber(c->main);
}

TEST_F(FiberTest, ManyLiveFibersHaveDistinctHandles) {
    constexpr int kCount = 64;

    int seen = 0;
    std::vector<Counter> ctx(kCount);
    std::vector<FiberHandle> fibers;
    fibers.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        ctx[i] = Counter{main_, i + 1, &seen};
        FiberHandle f = createFiber(kStackSize, counterEntry, &ctx[i]);
        ASSERT_NE(f, nullptr) << "fiber " << i;
        fibers.push_back(f);
    }

    std::set<FiberHandle> unique(fibers.begin(), fibers.end());
    EXPECT_EQ(unique.size(), fibers.size());

    for (FiberHandle f : fibers) {
        switchToFiber(f);
    }
    EXPECT_EQ(seen, kCount * (kCount + 1) / 2);

    for (FiberHandle f : fibers) {
        deleteFiber(f);
    }
}

}  // namespace
