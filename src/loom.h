#pragma once

#include "job.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace loom {

class Counter;

class JobSystem {
public:
    JobSystem() = default;
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void init(const JobSystemDesc& desc = {});
    void shutdown();

    void run(JobDecl mainJob);
    void quit() noexcept;

    void kickJobs(
        const JobDecl* jobs,
        unsigned count,
        Counter** outCounter = nullptr,
        JobPriority priority = JobPriority::Normal,
        ThreadAffinity affinity = ThreadAffinity::Any
    );

    void kickJob(
        JobDecl job,
        Counter** outCounter = nullptr,
        JobPriority priority = JobPriority::Normal,
        ThreadAffinity affinity = ThreadAffinity::Any
    ) {
        kickJobs(&job, 1, outCounter, priority, affinity);
    }

    void waitForCounter(Counter* counter, unsigned value = 0);
    void freeCounter(Counter* counter);

    void waitForCounterAndFree(
        Counter* counter,
        unsigned value = 0
    ) {
        waitForCounter(counter, value);
        freeCounter(counter);
    }

    struct Impl;

private:
    Impl* m_impl = nullptr;
    unsigned m_threadCount = 0;
};

} // namespace loom