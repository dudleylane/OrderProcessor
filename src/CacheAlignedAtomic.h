/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <cstddef>

namespace COP {

/**
 * Cache line size constant for x86-64 processors.
 * Used to prevent false sharing between frequently accessed atomics.
 */
constexpr std::size_t CACHE_LINE_SIZE = 64;

/**
 * Wrapper template that ensures atomic variables are aligned to cache line boundaries.
 * Prevents false sharing when multiple atomics are accessed concurrently by different threads.
 *
 * Usage:
 *   CacheAlignedAtomic<int> counter;
 *   counter.value.fetch_add(1, std::memory_order_relaxed);
 */
template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheAlignedAtomic {
    std::atomic<T> value;

    CacheAlignedAtomic() : value{} {}
    explicit CacheAlignedAtomic(T initial) : value{initial} {}

    // Convenience operators
    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return value.load(order);
    }

    void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
        value.store(desired, order);
    }

    T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.fetch_add(arg, order);
    }

    T fetch_sub(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.fetch_sub(arg, order);
    }

    T exchange(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.exchange(desired, order);
    }

    bool compare_exchange_weak(T& expected, T desired,
                               std::memory_order success = std::memory_order_seq_cst,
                               std::memory_order failure = std::memory_order_seq_cst) noexcept {
        return value.compare_exchange_weak(expected, desired, success, failure);
    }

    bool compare_exchange_strong(T& expected, T desired,
                                 std::memory_order success = std::memory_order_seq_cst,
                                 std::memory_order failure = std::memory_order_seq_cst) noexcept {
        return value.compare_exchange_strong(expected, desired, success, failure);
    }

    // Implicit conversion for compatibility
    operator T() const noexcept { return value.load(); }
};

} // namespace COP
