/**
 Concurrent Order Processor library - Google Test Migration

 Original Author: Sergey Mikhailik
 Test Migration: 2026

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 Tests for InterLockCache: basic pop/push, cache exhaustion,
 CAS contention under multi-threaded access, and CacheAutoPtr RAII.
*/

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <set>

#include "InterLockCache.h"

using namespace aux;

namespace {

class TestType {
public:
    TestType() : value_(0) {}
    int value_;
};

class InterlockCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        InterLockCache<TestType>::create("test_cache", 10);
    }

    void TearDown() override {
        InterLockCache<TestType>::destroy();
    }
};

// =============================================================================
// Basic Pop/Push
// =============================================================================

TEST_F(InterlockCacheTest, PopAndPushBasic) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    // Pop an item from the cache
    TestType* item = cache->popFront();
    ASSERT_NE(nullptr, item);

    item->value_ = 42;

    // Push it back
    cache->pushBack(item);
}

TEST_F(InterlockCacheTest, MultiplePopPush) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    std::vector<TestType*> items;

    // Pop multiple items
    for (int i = 0; i < 5; ++i) {
        TestType* item = cache->popFront();
        ASSERT_NE(nullptr, item);
        item->value_ = i;
        items.push_back(item);
    }

    // Push them all back
    for (auto* item : items) {
        cache->pushBack(item);
    }
}

TEST_F(InterlockCacheTest, ExceedCacheSize) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    std::vector<TestType*> items;

    // Pop more items than cache size (should allocate new ones)
    for (int i = 0; i < 15; ++i) {
        TestType* item = cache->popFront();
        ASSERT_NE(nullptr, item);
        items.push_back(item);
    }

    // Push them all back
    for (auto* item : items) {
        cache->pushBack(item);
    }
}

// =============================================================================
// CAS Contention Tests
// =============================================================================

TEST_F(InterlockCacheTest, ConcurrentPopPush) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    constexpr int THREADS = 4;
    constexpr int OPS_PER_THREAD = 50;

    std::atomic<int> successCount{0};

    auto worker = [&]() {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            TestType* item = cache->popFront();
            ASSERT_NE(nullptr, item);
            item->value_ = i;
            successCount.fetch_add(1, std::memory_order_relaxed);
            cache->pushBack(item);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(THREADS * OPS_PER_THREAD, successCount.load());
}

TEST_F(InterlockCacheTest, ConcurrentPopOnly) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    constexpr int THREADS = 4;
    constexpr int POPS_PER_THREAD = 10;

    std::vector<std::thread> threads;
    std::vector<std::vector<TestType*>> perThreadItems(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < POPS_PER_THREAD; ++i) {
                TestType* item = cache->popFront();
                ASSERT_NE(nullptr, item);
                perThreadItems[t].push_back(item);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Verify no two threads got the same pointer (no double-pop)
    std::set<TestType*> allPtrs;
    for (auto& vec : perThreadItems) {
        for (auto* ptr : vec) {
            auto [_, inserted] = allPtrs.insert(ptr);
            EXPECT_TRUE(inserted) << "Duplicate pointer detected — CAS pop race condition";
        }
    }

    // Push them all back
    for (auto& vec : perThreadItems) {
        for (auto* ptr : vec) {
            cache->pushBack(ptr);
        }
    }
}

TEST_F(InterlockCacheTest, ConcurrentPushOnly) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    // Pre-pop items for concurrent pushing
    constexpr int TOTAL_ITEMS = 8;
    std::vector<TestType*> items;
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        TestType* item = cache->popFront();
        ASSERT_NE(nullptr, item);
        item->value_ = i;
        items.push_back(item);
    }

    // Push back concurrently from multiple threads
    constexpr int THREADS = 4;
    int itemsPerThread = TOTAL_ITEMS / THREADS;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < itemsPerThread; ++i) {
                cache->pushBack(items[t * itemsPerThread + i]);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    SUCCEED();
}

TEST_F(InterlockCacheTest, RapidCycleConcurrent) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    constexpr int THREADS = 4;
    constexpr int CYCLES = 100;

    std::atomic<int> totalCycles{0};

    auto worker = [&]() {
        for (int i = 0; i < CYCLES; ++i) {
            TestType* item = cache->popFront();
            if (item) {
                // Simulate brief use
                item->value_ = i;
                cache->pushBack(item);
                totalCycles.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(THREADS * CYCLES, totalCycles.load());
}

// =============================================================================
// CacheAutoPtr RAII
// =============================================================================

TEST_F(InterlockCacheTest, CacheAutoPtrAcquiresAndReturns) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    {
        CacheAutoPtr<TestType> ptr(cache);
        ASSERT_NE(nullptr, ptr.get());
        ptr->value_ = 99;
        EXPECT_EQ(99, ptr->value_);
    }
    // Destructor should have pushed item back to cache
    SUCCEED();
}

TEST_F(InterlockCacheTest, CacheAutoPtrRelease) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    TestType* raw;
    {
        CacheAutoPtr<TestType> ptr(cache);
        raw = ptr.release();
        ASSERT_NE(nullptr, raw);
        // ptr destructor should NOT push back since ownership was released
    }
    // Clean up manually
    cache->pushBack(raw);
}

TEST_F(InterlockCacheTest, CacheAutoPtrReset) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    CacheAutoPtr<TestType> ptr(cache);
    ASSERT_NE(nullptr, ptr.get());
    ptr.reset();
    EXPECT_EQ(nullptr, ptr.get());
}

// =============================================================================
// Alignment Verification
// =============================================================================

TEST_F(InterlockCacheTest, AtomicMembersAreCacheLineAligned) {
    // Verify compile-time assertion matches runtime
    EXPECT_GE(alignof(InterLockCache<TestType>), 64u);
}

} // namespace
