// Minimal demonstration of the fiber job system: a "frame" that kicks a batch
// of work, waits on it, then runs a parallel-for — the shape of real engine use.

#include "loom.h"

#include <cstdio>

namespace {

void gameMain(void* arg) {
    auto& js = *static_cast<loom::JobSystem*>(arg);

    // do work here

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
