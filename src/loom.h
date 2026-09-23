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

        template <class F>
        requires std::is_invocable_v<std::decay_t<F>&>
    void kickJobOnMain(
        F&& f,
        Counter** outCounter = nullptr,
        JobPriority priority = JobPriority::Normal
    ) {
        kickJob(std::forward<F>(f), outCounter, priority, ThreadAffinity::Main);
    }

    template <class Body>
    void parallelFor(
        std::uint32_t begin,
        std::uint32_t end,
        Body&& body,
        std::uint32_t grainSize = 0,
        JobPriority priority = JobPriority::Normal
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

    void kickAndWaitRange(
        std::uint32_t begin,
        std::uint32_t end,
        std::uint32_t chunk,
        JobEntry entry,
        void* baseArg,
        std::size_t argStride,
        JobPriority priority
    );
};

namespace detail {

// This function invokes a heap copied callable, then frees it.
template <class Fn>
void lambdaJobTrampoline(void* arg) {
    Fn* f = static_cast<Fn*>(arg);
    (*f)();
    delete f;
}

template <class Body>
struct ParallelForChunk {
    Body* body;
    std::uint32_t begin;
    std::uint32_t end;
};

template <class Body>
void parallelForTrampoline(void* arg) {
    auto* c = static_cast<ParallelForChunk<Body>*>(arg);
    for (std::uint32_t i = c->begin; i < c->end; ++i) {
        (*c->body)(i);
    }
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

template <class Body>
void JobSystem::parallelFor(
    std::uint32_t begin,
    std::uint32_t end,
    Body&& body,
    std::uint32_t grainSize,
    JobPriority priority
) {
    if (end <= begin) {
        return;
    }

    const std::uint32_t total = end - begin;
    if (grainSize == 0) {
        const std::uint32_t targetChunks = m_threadCount * 4u;
        grainSize = (total + targetChunks - 1) / (targetChunks == 0 ? 1 : targetChunks);
        if (grainSize == 0) {
            grainSize = 1;
        }
    }

    using Chunk = detail::ParallelForChunk<std::remove_reference_t<Body>>;
    const std::uint32_t numChunks = (total + grainSize - 1) / grainSize;

    Chunk* chunks = static_cast<Chunk*>(::operator new(sizeof(Chunk) * numChunks));

    auto bodyPtr = &body;
    for (std::uint32_t c = 0; c < numChunks; ++c) {
        const std::uint32_t cb = begin + c * grainSize;
        const std::uint32_t ce = (cb + grainSize < end) 
            ? cb + grainSize 
            : end;
        chunks[c] = Chunk{ bodyPtr, cb, ce };
    }

    kickAndWaitRange(
        0,
        numChunks,
        1,
        &detail::parallelForTrampoline<std::remove_reference_t<Body>>,
        chunks,
        sizeof(Chunk),
        priority
    );

    ::operator delete(chunks);
}

} // namespace loom