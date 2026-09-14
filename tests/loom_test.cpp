#include "loom.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using namespace loom;

namespace {

struct MainCtx {
    JobSystem* js;
    std::function<void(JobSystem&)>   body;
};

void mainTrampoline(void* arg) {
    auto* ctx = static_cast<MainCtx*>(arg);
    ctx->body(*ctx->js);
    ctx->js->quit();
}

void runJS(std::function<void(JobSystem&)> body, JobSystemDesc desc = {}) {
    JobSystem js;
    js.init(desc);
    MainCtx ctx{ &js, std::move(body) };
    js.run(JobDecl{ &mainTrampoline, &ctx });
    js.shutdown();
}

void incJob(void* arg) {
    static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

void sleepIncJob(void* arg) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST(JobSystem, KickBatchAndWait) {
    runJS([](JobSystem& js) {
        std::atomic<int> n{ 0 };
        constexpr int N = 2000;
        std::vector<JobDecl> jobs(N, JobDecl{ &incJob, &n });

        Counter* c = nullptr;
        js.kickJobs(jobs.data(), N, &c);
        js.waitForCounterAndFree(c);

        EXPECT_EQ(n.load(), N);
        });
}

TEST(JobSystem, WaitSuspendsAndResumes) {
    runJS([](JobSystem& js) {
        std::atomic<int> n{ 0 };
        constexpr int N = 64;
        std::vector<JobDecl> jobs(N, JobDecl{ &sleepIncJob, &n });

        Counter* c = nullptr;
        js.kickJobs(jobs.data(), N, &c, JobPriority::High);
        js.waitForCounterAndFree(c);

        EXPECT_EQ(n.load(), N);
    });
}

namespace {
struct PinCtx {
    std::thread::id expected;
    JobSystem* js;
    std::atomic<int>* mismatches;
};

void pinnedCheckJob(void* arg) {
    auto* c = static_cast<PinCtx*>(arg);
    if (std::this_thread::get_id() != c->expected) {
        c->mismatches->fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int> n{ 0 };
    std::vector<JobDecl> kids(32, JobDecl{ &sleepIncJob, &n });
    Counter* cc = nullptr;
    c->js->kickJobs(kids.data(), 32, &cc);
    c->js->waitForCounterAndFree(cc);

    if (std::this_thread::get_id() != c->expected) {
        c->mismatches->fetch_add(1, std::memory_order_relaxed);
    }
}
} // namespace

TEST(JobSystem, MainAffinityStaysOnRunThreadAcrossWaits) {
    const std::thread::id runThread = std::this_thread::get_id();
    std::atomic<int> mismatches{ 0 };

    runJS([&](JobSystem& js) {
        PinCtx ctx{ runThread, &js, &mismatches };
        std::vector<JobDecl> jobs(8, JobDecl{ &pinnedCheckJob, &ctx });
        Counter* c = nullptr;
        js.kickJobs(
            jobs.data(),
            static_cast<unsigned>(jobs.size()),
            &c,
            JobPriority::Normal,
            ThreadAffinity::Main
        );
        js.waitForCounterAndFree(c);
    });

    EXPECT_EQ(mismatches.load(), 0);
}

TEST(JobSystem, WorkStealingLargeFanoutGrowsDeque) {
    runJS([](JobSystem& js) {
        std::atomic<int> n{ 0 };
        constexpr int N = 5000;
        std::vector<JobDecl> jobs(N, JobDecl{ &incJob, &n });
        Counter* c = nullptr;
        js.kickJobs(jobs.data(), N, &c);
        js.waitForCounterAndFree(c);
        EXPECT_EQ(n.load(), N);
    });
}