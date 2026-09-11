#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>


namespace loom::detail {

template <class T>
class WorkStealingDeque {
    static_assert(
        std::is_trivially_copyable_v<T>,
        "WorkStealingDeque element must be trivially copyable"
    );

    struct Ring {
        std::int64_t cap;
        std::int64_t mask;
        T*           buf;

        explicit Ring(std::int64_t c) 
            : cap(c)
            , mask(c - 1)
            , buf(new T[c]) {}

        ~Ring() {
            delete[] buf; 
        }

        void put(std::int64_t i, const T& v) { buf[i & mask] = v; }
        T get(std::int64_t i) const { return buf[i & mask]; }
    };

public:
    explicit WorkStealingDeque(std::int64_t initialCap = 256) {
        // Capacity must be a power of two for the index masking to work.
        std::int64_t cap = 1;
        while (cap < initialCap) {
            cap <<= 1;
        }
        m_array.store(new Ring(cap), std::memory_order_relaxed);
        m_top.store(0, std::memory_order_relaxed);
        m_bottom.store(0, std::memory_order_relaxed);
    }

    ~WorkStealingDeque() {
        delete m_array.load(std::memory_order_relaxed);
        for (Ring* r : m_retired) {
            delete r;
        }
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    void push(const T& v) {
        std::int64_t b = m_bottom.load(std::memory_order_relaxed);
        std::int64_t t = m_top.load(std::memory_order_acquire);
        Ring* a = m_array.load(std::memory_order_relaxed);
        if (b - t > a->cap - 1) {
            a = grow(a, b, t);
        }
        a->put(b, v);
        std::atomic_thread_fence(std::memory_order_release);
        m_bottom.store(b + 1, std::memory_order_relaxed);
    }

    bool pop(T& out) {
        std::int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
        Ring* a = m_array.load(std::memory_order_relaxed);
        m_bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t t = m_top.load(std::memory_order_relaxed);

        if (t > b) {
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }

        T v = a->get(b);
        if (t != b) {
            out = v;
            return true;
        }

        bool won = m_top.compare_exchange_strong(
            t,
            t + 1,
            std::memory_order_seq_cst,
            std::memory_order_relaxed
        );

        m_bottom.store(
            b + 1,
            std::memory_order_relaxed
        );

        if (won) {
            out = v;
            return true;
        }
        return false;
    }

    bool steal(T& out) {
        std::int64_t t = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t b = m_bottom.load(std::memory_order_acquire);
        if (t >= b) {
            return false;
        }

        Ring* a = m_array.load(std::memory_order_acquire);
        T v = a->get(t);
        if (!m_top.compare_exchange_strong(
            t,
            t + 1,
            std::memory_order_seq_cst,
            std::memory_order_relaxed
        )) {
            return false;
        }
        out = v;
        return true;
    }

private:
    Ring* grow(Ring* a, std::int64_t b, std::int64_t t) {
        Ring* na = new Ring(a->cap * 2);
        for (std::int64_t i = t; i < b; ++i) {
            na->put(i, a->get(i));
        }
        m_array.store(na, std::memory_order_release);
        m_retired.push_back(a);
        return na;
    }

    std::atomic<std::int64_t> m_top;
    std::atomic<std::int64_t> m_bottom;
    std::atomic<Ring*>        m_array;
    std::vector<Ring*>        m_retired;
};

} // namespace loom::detail
