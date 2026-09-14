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

    void kickJobOnMain(
        JobDecl job,
        Counter** outCounter = nullptr,
        JobPriority priority = JobPriority::Normal
    ) {
        kickJobs(&job, 1, outCounter, priority, ThreadAffinity::Main);
    }

    template <class F>
        requires std::is_invocable_v<std::decay_t<F>&>
    void kickJob(
        F&& f,
        Counter** outCounter = nullptr,
        JobPriority priority = JobPriority::Normal,
        ThreadAffinity affinity = ThreadAffinity::Any
    );

    void waitForCounter(Counter* counter, unsigned value = 0);
    void freeCounter(Counter* counter);

    void waitForCounterAndFree(
        Counter* counter,
        unsigned value = 0
    ) {
        waitForCounter(counter, value);
        freeCounter(counter);
    }

    static std::vector<CpuCore> physicalCores();

    unsigned threadCount() const noexcept { 
        return m_threadCount; 
    }

    struct Impl;

private:
    Impl* m_impl = nullptr;
    unsigned m_threadCount = 0;
};

namespace detail {

// This function invokes a heap copied callable, then frees it.
template <class Fn>
void lambdaJobTrampoline(void* arg) {
    Fn* f = static_cast<Fn*>(arg);
    (*f)();
    delete f;
}

} // namespace detail

// A lambda friendly overload of kickJob. It creates a function of the new type on the heap,
// then passes it to kickjobs. The lambdaHobTrampoline manages the lifetime of the heap object.
template <class F>
    requires std::is_invocable_v<std::decay_t<F>&>
void JobSystem::kickJob(
    F&& f,
    Counter** outCounter,
    JobPriority priority,
    ThreadAffinity affinity
) {
    using Fn = std::decay_t<F>;
    JobDecl decl{ &detail::lambdaJobTrampoline<Fn>, new Fn(std::forward<F>(f)) };
    kickJobs(&decl, 1, outCounter, priority, affinity);
}

} // namespace loom