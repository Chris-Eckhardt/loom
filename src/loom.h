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

    struct Impl;

private:
    Impl* m_impl = nullptr;
    unsigned m_threadCount = 0;
};

} // namespace loom