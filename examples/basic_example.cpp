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

void gameMain(void* arg) {
    auto& js = *static_cast<loom::JobSystem*>(arg);

    constexpr int kJobs = 256;
    std::vector<loom::JobDecl> batch(kJobs, loom::JobDecl{ &unitOfWork, nullptr });

    loom::Counter* counter = nullptr;
    js.kickJobs(batch.data(), kJobs, &counter, loom::JobPriority::High);
    js.waitForCounterAndFree(counter);
    std::printf("kickJobs: %d units of work complete\n", gWorkDone.load());

    js.quit();
}

} // namespace

int main() {
    loom::JobSystem js;
    js.init();
    js.run(loom::JobDecl{ &gameMain, &js });
    js.shutdown();
    std::printf("shutdown complete\n");
    return 0;
}
