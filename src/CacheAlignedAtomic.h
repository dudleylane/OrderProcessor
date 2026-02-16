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
#include <concepts>

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
/// Requires T to be trivially copyable — a prerequisite for std::atomic<T>.
template<typename T>
    requires std::is_trivially_copyable_v<T>
struct alignas(CACHE_LINE_SIZE) CacheAlignedAtomic {
    std::atomic<T> value;

    CacheAlignedAtomic() : value{} {}
    explicit CacheAlignedAtomic(T initial) : value{initial} {}

    // Convenience operators — default to relaxed ordering.
    // Callers requiring stronger ordering must pass it explicitly.
    T load(std::memory_order order = std::memory_order_relaxed) const noexcept {
        return value.load(order);
    }

    void store(T desired, std::memory_order order = std::memory_order_relaxed) noexcept {
        value.store(desired, order);
    }

    T fetch_add(T arg, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value.fetch_add(arg, order);
    }

    T fetch_sub(T arg, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value.fetch_sub(arg, order);
    }

    T exchange(T desired, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value.exchange(desired, order);
    }

    bool compare_exchange_weak(T& expected, T desired,
                               std::memory_order success = std::memory_order_relaxed,
                               std::memory_order failure = std::memory_order_relaxed) noexcept {
        return value.compare_exchange_weak(expected, desired, success, failure);
    }

    bool compare_exchange_strong(T& expected, T desired,
                                 std::memory_order success = std::memory_order_relaxed,
                                 std::memory_order failure = std::memory_order_relaxed) noexcept {
        return value.compare_exchange_strong(expected, desired, success, failure);
    }

    // Implicit conversion — uses relaxed ordering for consistency with other defaults
    operator T() const noexcept { return value.load(std::memory_order_relaxed); }
};

// Compile-time validation: ensure CacheAlignedAtomic actually has cache-line alignment
static_assert(alignof(CacheAlignedAtomic<int>) == CACHE_LINE_SIZE,
              "CacheAlignedAtomic must be aligned to cache line boundary");
static_assert(sizeof(CacheAlignedAtomic<int>) >= CACHE_LINE_SIZE,
              "CacheAlignedAtomic must occupy at least one cache line to prevent false sharing");

} // namespace COP
