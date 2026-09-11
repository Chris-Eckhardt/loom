#include "cpu_affinity.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <ios>
#include <set>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

using loom::detail::CoreId;
using loom::detail::enumeratePhysicalCores;
using loom::detail::pinCurrentThreadToCore;

namespace {

#if defined(_WIN32) || defined(__linux__)
constexpr bool kPinningSupported = true;
#else
constexpr bool kPinningSupported = false;
#endif

bool hasExactlyOneBit(std::uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

#if defined(_WIN32) || defined(__linux__)

bool runningOnCore(const CoreId& core) {
#if defined(_WIN32)
    PROCESSOR_NUMBER pn = {};
    GetCurrentProcessorNumberEx(&pn);
    return pn.Group == core.group && (static_cast<std::uint64_t>(1) << pn.Number) == core.mask;
#else
    const int cpu = sched_getcpu();
    if (cpu < 0 || cpu >= 64) {
        return false;
    }
    return core.group == 0 && (static_cast<std::uint64_t>(1) << cpu) == core.mask;
#endif
}

bool waitUntilRunningOnCore(const CoreId& core) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (runningOnCore(core)) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

#endif  // _WIN32 || __linux__

struct PinOutcome {
    bool pinned = false;
    bool landed = false;
};

PinOutcome pinOnScratchThread(const CoreId& core) {
    PinOutcome out;
    std::thread t([&] {
        out.pinned = pinCurrentThreadToCore(core);
#if defined(_WIN32) || defined(__linux__)
        if (out.pinned) {
            out.landed = waitUntilRunningOnCore(core);
        }
#endif
    });
    t.join();
    return out;
}

TEST(CpuAffinityTest, EnumerationReturnsAtLeastOneCore) {
    EXPECT_FALSE(enumeratePhysicalCores().empty());
}

TEST(CpuAffinityTest, EveryCoreMaskSelectsExactlyOneProcessor) {
    const auto cores = enumeratePhysicalCores();
    ASSERT_FALSE(cores.empty());

    for (std::size_t i = 0; i < cores.size(); ++i) {
        SCOPED_TRACE("core index " + std::to_string(i));
        EXPECT_TRUE(hasExactlyOneBit(cores[i].mask)) << "mask 0x" << std::hex << cores[i].mask;
    }
}

TEST(CpuAffinityTest, CoreIdsAreUnique) {
    const auto cores = enumeratePhysicalCores();
    ASSERT_FALSE(cores.empty());

    std::set<std::pair<std::uint16_t, std::uint64_t>> unique;
    for (const CoreId& c : cores) {
        unique.emplace(c.group, c.mask);
    }
    EXPECT_EQ(unique.size(), cores.size());
}

TEST(CpuAffinityTest, CoreCountDoesNotExceedLogicalProcessors) {
    const auto cores = enumeratePhysicalCores();
    ASSERT_FALSE(cores.empty());

    const unsigned logical = std::thread::hardware_concurrency();
    if (logical == 0) {
        GTEST_SKIP() << "hardware_concurrency() is unavailable here";
    }
    for (const CoreId& c : cores) {
        if (c.group != 0) {
            GTEST_SKIP() << "multiple processor groups present";
        }
    }

    EXPECT_LE(cores.size(), static_cast<std::size_t>(logical));
}

TEST(CpuAffinityTest, PinsAThreadToEachEnumeratedCore) {
    if (!kPinningSupported) {
        GTEST_SKIP() << "thread pinning is not implemented on this platform";
    }

    const auto cores = enumeratePhysicalCores();
    ASSERT_FALSE(cores.empty());

    for (std::size_t i = 0; i < cores.size(); ++i) {
        SCOPED_TRACE("core index " + std::to_string(i));
        const PinOutcome out = pinOnScratchThread(cores[i]);
        EXPECT_TRUE(out.pinned) << "pinCurrentThreadToCore reported failure";
        if (out.pinned) {
            EXPECT_TRUE(out.landed) << "thread never ran on the requested core";
        }
    }
}

TEST(CpuAffinityTest, ReportsFailureForAnEmptyMask) {
    const PinOutcome out = pinOnScratchThread(CoreId{0, 0});
    EXPECT_FALSE(out.pinned);
}

TEST(CpuAffinityTest, UnsupportedPlatformReportsFailureRatherThanPretending) {
    if (kPinningSupported) {
        GTEST_SKIP() << "pinning is implemented on this platform";
    }

    const auto cores = enumeratePhysicalCores();
    ASSERT_FALSE(cores.empty());
    EXPECT_FALSE(pinOnScratchThread(cores.front()).pinned);
}

}  // namespace
