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

TEST(JobSystem, ParallelForCoversWholeRange) {
    runJS([](JobSystem& js) {
        constexpr std::uint32_t N = 200000;
        std::vector<std::uint8_t> touched(N, 0);
        std::atomic<long long> sum{ 0 };

        js.parallelFor(0, N, [&](std::uint32_t i) {
            touched[i] = 1;
            sum.fetch_add(i, std::memory_order_relaxed);
        });

        for (std::uint32_t i = 0; i < N; ++i) {
            ASSERT_EQ(touched[i], 1) << "index " << i << " was not visited";
        }
        const long long expected = static_cast<long long>(N) * (N - 1) / 2;
        EXPECT_EQ(sum.load(), expected);
    });
}

namespace {

struct FanCtx {
    JobSystem* js;
    std::atomic<int>* total;
};

void fanJob(void* arg) {
    auto* c = static_cast<FanCtx*>(arg);
    c->js->parallelFor(0, 1000, [&](std::uint32_t) {
        c->total->fetch_add(1, std::memory_order_relaxed);
    });
}

} // namespace

TEST(JobSystem, NestedJobsAndWaits) {
    runJS([](JobSystem& js) {
        std::atomic<int> total{ 0 };
        constexpr int fanCount = 8;

        FanCtx ctx{ &js, &total };
        std::vector<JobDecl> jobs(fanCount, JobDecl{ &fanJob, &ctx });

        Counter* c = nullptr;
        js.kickJobs(jobs.data(), fanCount, &c);
        js.waitForCounterAndFree(c);

        EXPECT_EQ(total.load(), fanCount * 1000);
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

TEST(JobSystem, LambdaKickJob) {
    runJS([](JobSystem& js) {
        std::atomic<int> n{ 0 };
        Counter* c = nullptr;
        js.kickJob([&] {
            for (int i = 0; i < 50; ++i) {
                n.fetch_add(1, std::memory_order_relaxed);
            }
        }, &c);
        js.waitForCounterAndFree(c);
        EXPECT_EQ(n.load(), 50);

        std::atomic<int> m{ 0 };
        Counter* c2 = nullptr;
        js.kickJobOnMain([&] { m.fetch_add(7, std::memory_order_relaxed); }, &c2);
        js.waitForCounterAndFree(c2);
        EXPECT_EQ(m.load(), 7);

        std::atomic<int> total{ 0 };
        Counter* c3 = nullptr;
        js.kickJob([&] {
            js.parallelFor(0, 1000, [&](std::uint32_t) {
                total.fetch_add(1, std::memory_order_relaxed);
            });
        }, &c3);
        js.waitForCounterAndFree(c3);
        EXPECT_EQ(total.load(), 1000);
    });
}

TEST(JobSystem, CorePinningEnabledStillCompletes) {
    JobSystemDesc desc;
    desc.pinThreadsToCores = true;
    runJS([](JobSystem& js) {
        std::atomic<long long> sum{ 0 };
        constexpr std::uint32_t N = 200000;
        js.parallelFor(0, N, [&](std::uint32_t i) {
            sum.fetch_add(i, std::memory_order_relaxed);
        });
        EXPECT_EQ(sum.load(), static_cast<long long>(N) * (N - 1) / 2);
    }, desc);
}

TEST(JobSystem, SingleThreadedConfigStillCompletes) {
    JobSystemDesc desc;
    desc.numWorkerThreads = 1;
    runJS([](JobSystem& js) {
        std::atomic<int> total{ 0 };
        FanCtx ctx{ &js, &total };
        std::vector<JobDecl> jobs(4, JobDecl{ &fanJob, &ctx });
        Counter* c = nullptr;
        js.kickJobs(jobs.data(), 4, &c);
        js.waitForCounterAndFree(c);
        EXPECT_EQ(total.load(), 4 * 1000);
    }, desc);
}

namespace {

struct ExtCtx {
    JobSystem* js;
    std::atomic<int>* ran;
    std::atomic<bool>* go;
    std::atomic<Counter*>* shared;
};

void extSubmitMain(void* p) {
    auto* c = static_cast<ExtCtx*>(p);
    c->go->store(true, std::memory_order_release);
    Counter* cc = nullptr;
    while ((cc = c->shared->load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }
    c->js->waitForCounterAndFree(cc);
    c->js->quit();
}

struct SigCtx {
    JobSystem* js;
    std::atomic<Counter*>* shared;
};

void extSignalMain(void* p) {
    auto* c = static_cast<SigCtx*>(p);
    Counter* cc = c->js->createCounter(1);
    c->shared->store(cc, std::memory_order_release);
    c->js->waitForCounter(cc);
    c->js->freeCounter(cc);
    c->js->quit();
}

void quitImmediately(void* p) { 
    static_cast<JobSystem*>(p)->quit(); 
}

} // namespace

TEST(JobSystem, SubmitExternalFromForeignThread) {
    JobSystem js;
    js.init();

    std::atomic<int>      ran{ 0 };
    std::atomic<bool>     go{ false };
    std::atomic<Counter*> shared{ nullptr };

    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::vector<JobDecl> jobs(500, JobDecl{ &incJob, &ran });
        Counter* c = nullptr;
        js.submitExternal(jobs.data(), 500, &c);
        shared.store(c, std::memory_order_release);
    });

    ExtCtx ctx{ &js, &ran, &go, &shared };
    js.run(JobDecl{ &extSubmitMain, &ctx });
    producer.join();
    js.shutdown();

    EXPECT_EQ(ran.load(), 500);
}

TEST(JobSystem, ExternalCounterSignalWakesFiber) {
    JobSystem js;
    js.init();

    std::atomic<Counter*> shared{ nullptr };

    std::thread signaler([&] {
        Counter* cc = nullptr;
        while ((cc = shared.load(std::memory_order_acquire)) == nullptr) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        js.signalCounter(cc);
    });

    SigCtx ctx{ &js, &shared };
    js.run(JobDecl{ &extSignalMain, &ctx });
    signaler.join();
    js.shutdown();
    SUCCEED();
}

TEST(JobSystem, CoreReservationExposesCores) {
    const auto all = JobSystem::physicalCores();
    if (all.size() < 2) {
        GTEST_SKIP() << "needs >= 2 physical cores";
    }

    JobSystem js;
    JobSystemDesc desc;
    desc.pinThreadsToCores = true;
    desc.reservedCores = 1;
    js.init(desc);

    const auto reserved = js.reservedCores();
    ASSERT_EQ(reserved.size(), 1u);

    EXPECT_EQ(js.threadCount(), static_cast<unsigned>(all.size()) - 1);
    EXPECT_TRUE(JobSystem::pinThreadToCore(reserved[0]));

    js.run(JobDecl{ &quitImmediately, &js });
    js.shutdown();
}