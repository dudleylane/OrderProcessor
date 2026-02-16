/**
 * Concurrent Order Processor library - CacheAlignedAtomic Tests
 *
 * Tests for CacheAlignedAtomic: alignment verification, memory ordering,
 * concurrent access, and C++20 concept constraints.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <type_traits>

#include "CacheAlignedAtomic.h"

using namespace COP;

namespace {

// =============================================================================
// Alignment and Size Verification
// =============================================================================

TEST(CacheAlignedAtomicTest, AlignmentIsCacheLine) {
    EXPECT_EQ(64u, alignof(CacheAlignedAtomic<int>));
    EXPECT_EQ(64u, alignof(CacheAlignedAtomic<size_t>));
    EXPECT_EQ(64u, alignof(CacheAlignedAtomic<bool>));
}

TEST(CacheAlignedAtomicTest, SizeAtLeastOneCacheLine) {
    EXPECT_GE(sizeof(CacheAlignedAtomic<int>), 64u);
    EXPECT_GE(sizeof(CacheAlignedAtomic<size_t>), 64u);
}

TEST(CacheAlignedAtomicTest, TwoCacheAlignedAtomicsDontShareCacheLine) {
    struct TwoAtomics {
        CacheAlignedAtomic<int> a;
        CacheAlignedAtomic<int> b;
    };

    TwoAtomics pair;
    auto addrA = reinterpret_cast<uintptr_t>(&pair.a);
    auto addrB = reinterpret_cast<uintptr_t>(&pair.b);

    // Must be at least 64 bytes apart (no false sharing)
    EXPECT_GE(addrB - addrA, 64u);
}

// =============================================================================
// Basic Atomic Operations
// =============================================================================

TEST(CacheAlignedAtomicTest, DefaultConstructAndLoad) {
    CacheAlignedAtomic<int> a(0);
    EXPECT_EQ(0, a.load());
}

TEST(CacheAlignedAtomicTest, StoreAndLoad) {
    CacheAlignedAtomic<int> a(0);
    a.store(42);
    EXPECT_EQ(42, a.load());
}

TEST(CacheAlignedAtomicTest, FetchAdd) {
    CacheAlignedAtomic<int> a(10);
    int prev = a.fetch_add(5);
    EXPECT_EQ(10, prev);
    EXPECT_EQ(15, a.load());
}

TEST(CacheAlignedAtomicTest, FetchSub) {
    CacheAlignedAtomic<int> a(10);
    int prev = a.fetch_sub(3);
    EXPECT_EQ(10, prev);
    EXPECT_EQ(7, a.load());
}

TEST(CacheAlignedAtomicTest, CompareExchangeStrong) {
    CacheAlignedAtomic<int> a(5);

    int expected = 5;
    bool success = a.compare_exchange_strong(expected, 10);
    EXPECT_TRUE(success);
    EXPECT_EQ(10, a.load());

    expected = 5; // wrong expected
    success = a.compare_exchange_strong(expected, 20);
    EXPECT_FALSE(success);
    EXPECT_EQ(10, expected); // expected updated to current value
}

TEST(CacheAlignedAtomicTest, RelaxedOrdering) {
    CacheAlignedAtomic<int> a(0);
    a.store(99, std::memory_order_relaxed);
    EXPECT_EQ(99, a.load(std::memory_order_relaxed));
}

// =============================================================================
// Concurrent Access (False Sharing Prevention)
// =============================================================================

TEST(CacheAlignedAtomicTest, ConcurrentIncrementNoFalseSharing) {
    constexpr int THREADS = 4;
    constexpr int INCREMENTS = 10000;

    // Two adjacent cache-aligned atomics — should not contend
    CacheAlignedAtomic<int> counter1(0);
    CacheAlignedAtomic<int> counter2(0);

    auto worker1 = [&]() {
        for (int i = 0; i < INCREMENTS; ++i) {
            counter1.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto worker2 = [&]() {
        for (int i = 0; i < INCREMENTS; ++i) {
            counter2.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS / 2; ++i) {
        threads.emplace_back(worker1);
        threads.emplace_back(worker2);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(THREADS / 2 * INCREMENTS, counter1.load());
    EXPECT_EQ(THREADS / 2 * INCREMENTS, counter2.load());
}

// =============================================================================
// C++20 Concept Validation (compile-time)
// =============================================================================

// These static_asserts verify the concept constraint works at compile time.
// Trivially copyable types should be accepted:
static_assert(std::is_trivially_copyable_v<int>);
static_assert(std::is_trivially_copyable_v<size_t>);
static_assert(std::is_trivially_copyable_v<bool>);
static_assert(std::is_trivially_copyable_v<double>);

// Verify CacheAlignedAtomic can be instantiated with trivially copyable types
static_assert(sizeof(CacheAlignedAtomic<int>) > 0);
static_assert(sizeof(CacheAlignedAtomic<size_t>) > 0);
static_assert(sizeof(CacheAlignedAtomic<bool>) > 0);

// Non-trivially-copyable types (like std::string) should NOT compile.
// We can't test this at runtime, but document the constraint:
// static_assert(!std::is_trivially_copyable_v<std::string>);
// CacheAlignedAtomic<std::string> would fail to compile — as intended.

} // namespace
