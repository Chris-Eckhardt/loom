#include "loom.h"

#include "cpu_affinity.h"
#include "fiber.h"
#include "spin_lock.h"
#include "work_stealing_deque.h"

#include <cassert>
#include <thread>
#include <vector>
#include <deque>

namespace loom {

using detail::FiberHandle;
using detail::SpinLock;
using detail::SpinLockGuard;

namespace {

constexpr std::uint16_t kInvalidFiber = 0xFFFFu;

enum class PrevAction : std::uint8_t { None, ToPool, ToWaitList };

struct WorkerTls {
    FiberHandle threadFiber = nullptr;
    std::uint16_t currentFiber = kInvalidFiber;
    std::uint16_t previousFiber = kInvalidFiber;
    PrevAction prevAction = PrevAction::None;
    Counter* waitCounter = nullptr;
    unsigned waitValue = 0;
    bool isMainThread = false;
    unsigned threadIndex = 0;
    std::uint32_t rngState = 0x9e3779b9u;
};

thread_local WorkerTls* tTls = nullptr;

inline std::uint32_t nextRand(WorkerTls* t) {
    std::uint32_t x = t->rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t->rngState = x;
    return x;
}

} // namespace

class Counter {
public:
    static constexpr int kMaxWaiters = 12;

    struct Waiter {
        std::uint16_t fiber = kInvalidFiber;
        unsigned      target = 0;
        bool          used = false;
    };

    std::atomic<unsigned> value{ 0 };
    SpinLock              lock;
    Waiter                waiters[kMaxWaiters];
    std::uint16_t         poolIndex = 0;
};

struct JobSystem::Impl {
    JobSystemDesc desc;

    FiberHandle* fibers = nullptr;
    unsigned fiberCount = 0;

    SpinLock freeFiberLock;
    std::vector<std::uint16_t> freeFibers;

    SpinLock readyFiberLock;
    std::vector<std::uint16_t> readyFibers;
    std::vector<std::uint16_t> mainReadyFibers;

    std::vector<std::uint8_t> fiberPinnedToMain;

    static constexpr unsigned kMaxCounters = 8192;
    Counter* counters = nullptr;
    SpinLock counterLock;
    std::vector<std::uint16_t> freeCounters;

    struct QueuedJob {
        JobEntry entry = nullptr;
        void* arg = nullptr;
        Counter* counter = nullptr;
        ThreadAffinity affinity = ThreadAffinity::Any;
    };

    struct ThreadDeques {
        detail::WorkStealingDeque<QueuedJob> pri[3];
    };
    unsigned threadCount = 0;
    std::unique_ptr<ThreadDeques[]> threadDeques;

    SpinLock mainJobLock;
    std::deque<QueuedJob> mainQueues[3];

    std::vector<std::thread> workers;
    std::atomic<bool> quit{ false };

    bool pinning = false;
    std::vector<detail::CoreId> cores;
    unsigned schedulerCoreCount = 0;
};

namespace {

void pushFreeFiber(JobSystem::Impl* impl, std::uint16_t idx) {
    SpinLockGuard g(impl->freeFiberLock);
    impl->freeFibers.push_back(idx);
}

std::uint16_t acquireFreeFiber(JobSystem::Impl* impl) {
    for (;;) {
        {
            SpinLockGuard g(impl->freeFiberLock);
            if (!impl->freeFibers.empty()) {
                std::uint16_t idx = impl->freeFibers.back();
                impl->freeFibers.pop_back();
                return idx;
            }
        }

        if (impl->quit.load(std::memory_order_acquire)) {
            return kInvalidFiber;
        }

        detail::cpuPause();
        std::this_thread::yield();
    }
}

void pushReadyFiber(
    JobSystem::Impl* impl,
    std::uint16_t idx,
    bool toMain
) {
    SpinLockGuard g(impl->readyFiberLock);

    if (toMain) {
        impl->mainReadyFibers.push_back(idx);
    }
    else {
        impl->readyFibers.push_back(idx);
    }
}

std::uint16_t popReadyFiber(
    JobSystem::Impl* impl,
    bool isMain
) {
    SpinLockGuard g(impl->readyFiberLock);

    if (isMain && !impl->mainReadyFibers.empty()) {
        std::uint16_t idx = impl->mainReadyFibers.back();
        impl->mainReadyFibers.pop_back();
        return idx;
    }

    if (!impl->readyFibers.empty()) {
        std::uint16_t idx = impl->readyFibers.back();
        impl->readyFibers.pop_back();
        return idx;
    }

    return kInvalidFiber;
}

bool popJob(
    JobSystem::Impl* impl,
    JobSystem::Impl::QueuedJob& out,
    WorkerTls* tls
) {
    if (tls->isMainThread) {
        SpinLockGuard g(impl->mainJobLock);
        for (int p = 0; p < 3; ++p) {
            if (!impl->mainQueues[p].empty()) {
                out = impl->mainQueues[p].front();
                impl->mainQueues[p].pop_front();
                return true;
            }
        }
    }

    const unsigned self = tls->threadIndex;
    for (int p = 0; p < 3; ++p) {
        if (impl->threadDeques[self].pri[p].pop(out)) {
            return true;
        }
    }

    const unsigned n = impl->threadCount;
    if (n > 1) {
        const unsigned start = nextRand(tls) % n;
        for (unsigned k = 0; k < n; ++k) {
            const unsigned v = (start + k) % n;
            if (v == self) {
                continue;
            }
            for (int p = 0; p < 3; ++p) {
                if (impl->threadDeques[v].pri[p].steal(out)) {
                    return true;
                }
            }
        }
    }

    return false;
}

void counterAddWaiter(
    JobSystem::Impl* impl,
    Counter* c,
    std::uint16_t fiber,
    unsigned target
) {
    const bool toMain = impl->fiberPinnedToMain[fiber] != 0;
    SpinLockGuard g(c->lock);

    if (c->value.load(std::memory_order_acquire) <= target) {
        pushReadyFiber(impl, fiber, toMain);
        return;
    }

    for (auto& w : c->waiters) {
        if (!w.used) {
            w.used = true;
            w.fiber = fiber;
            w.target = target;
            return;
        }
    }

    assert(false && "Counter waiter slots exhausted");
    pushReadyFiber(impl, fiber, toMain);
}

void counterDecrement(
    JobSystem::Impl* impl,
    Counter* c
) {
    const unsigned newVal = c->value.fetch_sub(1, std::memory_order_acq_rel) - 1;
    SpinLockGuard g(c->lock);
    for (auto& w : c->waiters) {
        if (w.used && newVal <= w.target) {
            w.used = false;
            const bool toMain = impl->fiberPinnedToMain[w.fiber] != 0;
            pushReadyFiber(impl, w.fiber, toMain);
        }
    }
}

void cleanupPreviousFiber(JobSystem::Impl* impl) {
    WorkerTls* t = tTls;

    switch (t->prevAction) {
    case PrevAction::None:
        return;
    case PrevAction::ToPool:
        pushFreeFiber(impl, t->previousFiber);
        break;
    case PrevAction::ToWaitList:
        counterAddWaiter(impl, t->waitCounter, t->previousFiber, t->waitValue);
        break;
    }

    t->prevAction = PrevAction::None;
    t->previousFiber = kInvalidFiber;
}

void workerThreadMain(
    JobSystem::Impl* impl, 
    int coreIndex, 
    unsigned threadIndex
) {
    if (coreIndex >= 0) {
        detail::pinCurrentThreadToCore(impl->cores[static_cast<std::size_t>(coreIndex)]);
    }

    WorkerTls tls;
    tls.threadIndex = threadIndex;
    tls.rngState = (0x9e3779b9u + threadIndex * 2654435761u) | 1u;
    tTls = &tls;
    tls.threadFiber = detail::convertThreadToFiber();

    std::uint16_t start = acquireFreeFiber(impl);
    if (start != kInvalidFiber) {
        tls.prevAction = PrevAction::None;
        tls.currentFiber = start;
        detail::switchToFiber(impl->fibers[start]);
    }

    detail::convertFiberToThread();
    tTls = nullptr;
}

void schedulerLoop(JobSystem::Impl* impl) {
    cleanupPreviousFiber(impl);

    for (;;) {
        const bool isMain = tTls->isMainThread;

        if (impl->quit.load(std::memory_order_acquire)) {
            detail::switchToFiber(tTls->threadFiber);
            return;
        }

        std::uint16_t rf = popReadyFiber(impl, isMain);
        if (rf != kInvalidFiber) {
            WorkerTls* t = tTls;
            t->previousFiber = t->currentFiber;
            t->prevAction = PrevAction::ToPool;
            t->currentFiber = rf;
            detail::switchToFiber(impl->fibers[rf]);
            cleanupPreviousFiber(impl);
            continue;
        }

        JobSystem::Impl::QueuedJob job;
        if (popJob(impl, job, tTls)) {
            const bool pinned = job.affinity == ThreadAffinity::Main;
            const std::uint16_t self = tTls->currentFiber;
            if (pinned) {
                impl->fiberPinnedToMain[self] = 1;
            }
            job.entry(job.arg);
            if (pinned) {
                impl->fiberPinnedToMain[self] = 0;
            }
            if (job.counter != nullptr) {
                counterDecrement(impl, job.counter);
            }
            continue;
        }

        detail::cpuPause();
        std::this_thread::yield();
    }
}

void schedulerFiberEntry(void* arg) {
    schedulerLoop(static_cast<JobSystem::Impl*>(arg));
}

Counter* allocCounter(
    JobSystem::Impl* impl,
    unsigned initial
) {
    std::uint16_t idx;
    {
        SpinLockGuard g(impl->counterLock);
        assert(!impl->freeCounters.empty() && "Counter pool exhausted");
        idx = impl->freeCounters.back();
        impl->freeCounters.pop_back();
    }
    Counter* c = &impl->counters[idx];
    c->value.store(initial, std::memory_order_release);
    for (auto& w : c->waiters) {
        w.used = false;
    }
    return c;
}

} // namespace

JobSystem::~JobSystem() {
    if (m_impl != nullptr) {
        shutdown();
    }
}

void JobSystem::init(const JobSystemDesc& desc) {
    assert(m_impl == nullptr && "JobSystem::init called twice");

    m_impl = new Impl();
    Impl* impl = m_impl;
    impl->desc = desc;

    if (desc.pinThreadsToCores) {
        impl->cores = detail::enumeratePhysicalCores();
        impl->pinning = !impl->cores.empty();
    }

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        hw = 1;
    }

    unsigned workers;
    if (desc.numWorkerThreads != 0) {
        workers = desc.numWorkerThreads;
    }
    else if (impl->pinning) {
        impl->schedulerCoreCount = static_cast<unsigned>(impl->cores.size());

        workers = impl->schedulerCoreCount > 1 
            ? impl->schedulerCoreCount - 1 
            : 0;
    }
    else {
        workers = hw > 1 
            ? hw - 1 
            : 0;
    }
    m_threadCount = workers + 1;

    impl->threadCount = m_threadCount;
    impl->threadDeques = std::make_unique<Impl::ThreadDeques[]>(m_threadCount);

    impl->fiberCount = desc.numFibers;
    impl->fibers = new FiberHandle[impl->fiberCount];
    impl->freeFibers.reserve(impl->fiberCount);
    impl->fiberPinnedToMain.assign(impl->fiberCount, 0);
    for (unsigned i = 0; i < impl->fiberCount; ++i) {
        impl->fibers[i] = detail::createFiber(
            desc.fiberStackSize,
            &schedulerFiberEntry,
            impl
        );
        impl->freeFibers.push_back(static_cast<std::uint16_t>(i));
    }

    impl->counters = new Counter[Impl::kMaxCounters];
    impl->freeCounters.reserve(Impl::kMaxCounters);
    for (unsigned i = 0; i < Impl::kMaxCounters; ++i) {
        impl->counters[i].poolIndex = static_cast<std::uint16_t>(i);
        impl->freeCounters.push_back(static_cast<std::uint16_t>(i));
    }

    impl->quit.store(false, std::memory_order_release);
    impl->workers.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        int coreIndex = -1;
        if (impl->pinning) {
            coreIndex = static_cast<int>((i + 1) % impl->schedulerCoreCount);
        }
        impl->workers.emplace_back(&workerThreadMain, impl, coreIndex, i + 1);
    }
}

void JobSystem::shutdown() {
    Impl* impl = m_impl;
    if (impl == nullptr) {
        return;
    }

    impl->quit.store(true, std::memory_order_release);
    for (auto& t : impl->workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    delete[] impl->fibers;
    delete[] impl->counters;
    delete impl;
    m_impl = nullptr;
    m_threadCount = 0;
}

void JobSystem::run(JobDecl mainJob) {
    assert(m_impl != nullptr && "JobSystem::run called before init");
    Impl* impl = m_impl;

    if (impl->pinning && !impl->cores.empty()) {
        detail::pinCurrentThreadToCore(impl->cores[0]);
    }

    WorkerTls tls;
    tls.isMainThread = true;
    tls.threadIndex = 0;
    tls.rngState = 0x9e3779b9u | 1u;
    tTls = &tls;
    tls.threadFiber = detail::convertThreadToFiber();

    kickJob(mainJob);

    std::uint16_t start = acquireFreeFiber(impl);
    if (start != kInvalidFiber) {
        tls.prevAction = PrevAction::None;
        tls.currentFiber = start;
        detail::switchToFiber(impl->fibers[start]);
    }

    detail::convertFiberToThread();
    tTls = nullptr;
}

void JobSystem::quit() noexcept {
    if (m_impl != nullptr) {
        m_impl->quit.store(true, std::memory_order_release);
    }
}

void JobSystem::kickJobs(
    const JobDecl* jobs,
    unsigned count,
    Counter** outCounter,
    JobPriority priority,
    ThreadAffinity affinity
) {
    Impl* impl = m_impl;
    Counter* c = nullptr;
    if (outCounter != nullptr) {
        c = allocCounter(impl, count);
        *outCounter = c;
    }

    if (count == 0) {
        return;
    }

    if (affinity == ThreadAffinity::Main) {
        SpinLockGuard g(impl->mainJobLock);
        auto& q = impl->mainQueues[static_cast<int>(priority)];
        for (unsigned i = 0; i < count; ++i) {
            q.push_back(Impl::QueuedJob{ jobs[i].entry, jobs[i].arg, c, affinity });
        }
        return;
    }

    WorkerTls* tls = tTls;
    assert(
        tls != nullptr
        && "kickJobs must be called from within a job (on a scheduler thread)"
    );

    auto& deque = impl->threadDeques[tls->threadIndex].pri[static_cast<int>(priority)];
    for (unsigned i = 0; i < count; ++i) {
        deque.push(Impl::QueuedJob{ jobs[i].entry, jobs[i].arg, c, affinity });
    }
}

void JobSystem::waitForCounter(
    Counter* counter,
    unsigned value
) {
    Impl* impl = m_impl;
    if (counter->value.load(std::memory_order_acquire) <= value) {
        return;
    }

    std::uint16_t freeFiber = acquireFreeFiber(impl);
    if (freeFiber == kInvalidFiber) {
        return;
    }

    WorkerTls* t = tTls;
    t->previousFiber = t->currentFiber;
    t->prevAction = PrevAction::ToWaitList;
    t->waitCounter = counter;
    t->waitValue = value;
    t->currentFiber = freeFiber;

    detail::switchToFiber(impl->fibers[freeFiber]);

    cleanupPreviousFiber(impl);
}

void JobSystem::freeCounter(Counter* counter) {
    Impl* impl = m_impl;
    SpinLockGuard g(impl->counterLock);
    impl->freeCounters.push_back(counter->poolIndex);
}

} // namespace loom