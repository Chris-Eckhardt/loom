<div align="center">
<pre>
 _     _____  ________  ___
| |   |  _  ||  _  |  \/  |
| |   | | | || | | | .  . |
| |   | | | || | | | |\/| |
| |___\ \_/ /\ \_/ / |  | |
\_____/\___/  \___/\_|  |_/
</pre>
<em>a fiber-based job system</em>
</div>

## Overview

A fiber based job system written in c++20 geared toward use in a game engine.

Work is expressed as a job (function pointer + void*). Jobs run on a pool of fibers carried by one worker thread per core. Completion is tracked with atomic counters. Waiting on a counter suspends the current fiber and frees its thread to run other jobs. Once the counter drains, the fiber is resumed, possibly on a different thread. For the standard function pointer API, there is no per-job heap allocation and no OS level blocking on wait.

### API

For a working example see `examples/basic_examples.cpp`

```cpp
#include "loom.h"

void gameMain(void* arg) {
    auto& js = *static_cast<loom::JobSystem*>(arg);

    // Kick a batch and wait on it.
    loom::JobDecl jobs[64] = /* ... { &fn, &data } ... */;
    loom::Counter* c = nullptr;
    js.kickJobs(jobs, 64, &c, loom::JobPriority::High);
    js.waitForCounterAndFree(c); // suspends this fiber until done

    // Main-thread-only work (window pump, present, platform UI): pin it so it
    // runs on the run() thread and never migrates, even across a wait.
    loom::Counter* p = nullptr;
    js.kickJobOnMain({ &present, nullptr }, &p);
    js.waitForCounterAndFree(p);

    js.quit(); // makes run() return on the main thread
}

int main() {
    loom::JobSystem js;
    js.init();
    js.run(loom::JobDecl{ &gameMain, &js });
    js.shutdown();
    return 0;
}
```

## Building

```bash
cmake -S . -B build
cmake --build build
```

It is recommended to use the loom::JobDecl API of kickJobs on hotpaths because the lambda overload has a some heap allocation overhead.

## Future planned work

- parallelFor(...)
- reserved threads and a way to receiving work from them

