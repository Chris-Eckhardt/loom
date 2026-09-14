#include "loom.h"

#include <cstdio>
#include <atomic>

namespace {

std::atomic<int> gWorkDone{ 0 };

void unitOfWork(void* /*arg*/) {
    volatile double x = 0.0;
    for (int i = 0; i < 1000; ++i) {
        x += i * 0.5;
    }
    gWorkDone.fetch_add(1, std::memory_order_relaxed);
}

void workForMainThread(void* /*arg*/) {
    std::printf("this job is running on the main thread\n");
}

void gameMain(void* arg) {
    auto& js = *static_cast<loom::JobSystem*>(arg);

    std::printf("gameMain running with %u scheduler threads\n", js.threadCount());

    constexpr int kJobs = 256;
    std::vector<loom::JobDecl> batch(kJobs, loom::JobDecl{ &unitOfWork, nullptr });

    // Kick N jobs on worker threads
    {
        loom::Counter* counter = nullptr;
        js.kickJobs(batch.data(), kJobs, &counter, loom::JobPriority::High);
        js.waitForCounterAndFree(counter);
        std::printf("kickJobs: %d units of work done\n", gWorkDone.load());
    }

    // kick a job on the main thread
    {
        loom::Counter* counter = nullptr;
        js.kickJobOnMain(loom::JobDecl{ &workForMainThread, nullptr }, &counter);
        js.waitForCounterAndFree(counter);
    }

    // kick a lambda
    {
        std::atomic<uint8_t> num = { 0 };
        loom::Counter* counter = nullptr;
        js.kickJob([&]{
            num.fetch_add(1);
        }, &counter);
        js.waitForCounterAndFree(counter);
        std::printf("kick lambda, expect: 1, observed: %d\n", num.load());
    }

    js.quit();
}

} // namespace

int main() {
    loom::JobSystem js;
    js.init();
    auto cores = js.physicalCores();
    std::printf("number of physical cores: %llu\n", cores.size());
    js.run(loom::JobDecl{ &gameMain, &js });
    js.shutdown();
    std::printf("shutdown complete\n");
    return 0;
}
