// Minimal demonstration of the fiber job system: a "frame" that kicks a batch
// of work, waits on it, then runs a parallel-for — the shape of real engine use.

#include "loom.h"

#include <cstdio>

namespace {

void gameMain(void* arg) {
    auto& loom = *static_cast<loom::JobSystem*>(arg);

}

} // namespace

int main() {
    loom::JobSystem system;
    system.init();
    system.shutdown();
    std::printf("shutdown complete\n");
    return 0;
}
