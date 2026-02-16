/**
 * Concurrent Order Processor library - TransactionScopePool Tests
 *
 * Tests for the lock-free TransactionScope object pool: acquire/release
 * cycle, pool exhaustion with heap fallback, detach-based release,
 * RAII semantics, and alternative allocation modes.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <set>

#include "TransactionScopePool.h"

using namespace COP::ACID;

namespace {

// =============================================================================
// Basic Pool Tests
// =============================================================================

class TransactionScopePoolTest : public ::testing::Test {
protected:
    static constexpr size_t SMALL_POOL = 8;
};

TEST_F(TransactionScopePoolTest, AcquireReturnsValidScope) {
    TransactionScopePool pool(SMALL_POOL);
    size_t idx = TransactionScopePool::INVALID_INDEX;

    TransactionScope* scope = pool.acquire(idx);

    ASSERT_NE(nullptr, scope);
    EXPECT_NE(TransactionScopePool::INVALID_INDEX, idx);

    pool.releaseByIndex(idx);
}

TEST_F(TransactionScopePoolTest, AcquireReleaseCycle) {
    TransactionScopePool pool(SMALL_POOL);

    for (int i = 0; i < 100; ++i) {
        size_t idx;
        TransactionScope* scope = pool.acquire(idx);
        ASSERT_NE(nullptr, scope);

        if (idx != TransactionScopePool::INVALID_INDEX) {
            pool.releaseByIndex(idx);
        } else {
            delete scope;
        }
    }

    EXPECT_EQ(0u, pool.cacheMisses());
}

TEST_F(TransactionScopePoolTest, PoolSizeReported) {
    TransactionScopePool pool(SMALL_POOL);
    EXPECT_EQ(SMALL_POOL, pool.poolSize());
}

// =============================================================================
// Exhaustion and Heap Fallback
// =============================================================================

TEST_F(TransactionScopePoolTest, ExhaustionFallsBackToHeap) {
    TransactionScopePool pool(4);

    std::vector<std::pair<TransactionScope*, size_t>> acquired;

    // Acquire all 4 pool slots
    for (int i = 0; i < 4; ++i) {
        size_t idx;
        TransactionScope* scope = pool.acquire(idx);
        ASSERT_NE(nullptr, scope);
        acquired.push_back({scope, idx});
    }

    // Next acquire should fall back to heap
    size_t idx;
    TransactionScope* heapScope = pool.acquire(idx);
    ASSERT_NE(nullptr, heapScope);
    EXPECT_EQ(TransactionScopePool::INVALID_INDEX, idx);
    EXPECT_GE(pool.cacheMisses(), 1u);

    delete heapScope;

    // Release pool slots
    for (auto& [scope, i] : acquired) {
        if (i != TransactionScopePool::INVALID_INDEX)
            pool.releaseByIndex(i);
    }
}

TEST_F(TransactionScopePoolTest, CacheMissCounterIncrementsOnExhaustion) {
    TransactionScopePool pool(2);

    size_t idx1, idx2, idx3;
    auto* s1 = pool.acquire(idx1);
    auto* s2 = pool.acquire(idx2);

    EXPECT_EQ(0u, pool.cacheMisses());

    auto* s3 = pool.acquire(idx3);
    EXPECT_GE(pool.cacheMisses(), 1u);

    delete s3; // heap allocated
    pool.releaseByIndex(idx2);
    pool.releaseByIndex(idx1);
}

// =============================================================================
// Detach Mechanism
// =============================================================================

TEST_F(TransactionScopePoolTest, DetachReturnsScope) {
    TransactionScopePool pool(SMALL_POOL);
    size_t idx;

    TransactionScope* scope = pool.acquire(idx);
    ASSERT_NE(TransactionScopePool::INVALID_INDEX, idx);

    TransactionScope* detached = pool.detach(idx);
    EXPECT_EQ(scope, detached);

    // Pool slot is now empty; next acquire should still work
    size_t idx2;
    TransactionScope* scope2 = pool.acquire(idx2);
    ASSERT_NE(nullptr, scope2);

    if (idx2 != TransactionScopePool::INVALID_INDEX)
        pool.releaseByIndex(idx2);

    delete detached;
}

// =============================================================================
// PooledTransactionScope RAII
// =============================================================================

TEST_F(TransactionScopePoolTest, PooledScopeAutoReleasesOnDestruction) {
    TransactionScopePool pool(2);

    // Acquire both slots via RAII scopes, then let them destruct
    {
        PooledTransactionScope ps1(&pool);
        PooledTransactionScope ps2(&pool);
        ASSERT_NE(nullptr, ps1.get());
        ASSERT_NE(nullptr, ps2.get());
    }

    // Both slots should be returned — acquire should not miss
    size_t idx;
    auto* scope = pool.acquire(idx);
    ASSERT_NE(nullptr, scope);
    EXPECT_NE(TransactionScopePool::INVALID_INDEX, idx);
    pool.releaseByIndex(idx);

    EXPECT_EQ(0u, pool.cacheMisses());
}

TEST_F(TransactionScopePoolTest, PooledScopeReleaseTransfersOwnership) {
    TransactionScopePool pool(SMALL_POOL);

    TransactionScope* released;
    {
        PooledTransactionScope ps(&pool);
        released = ps.release();
        ASSERT_NE(nullptr, released);
        // ps destructor should NOT return scope to pool (already released)
    }

    delete released;
}

TEST_F(TransactionScopePoolTest, PooledScopeWithNullPool) {
    PooledTransactionScope ps(nullptr);
    ASSERT_NE(nullptr, ps.get());

    TransactionScope* released = ps.release();
    ASSERT_NE(nullptr, released);
    delete released;
}

TEST_F(TransactionScopePoolTest, PooledScopeOperatorArrow) {
    TransactionScopePool pool(SMALL_POOL);
    PooledTransactionScope ps(&pool);

    // Should be able to use operator-> to access TransactionScope methods
    TransactionId id(42, 1);
    ps->setTransactionId(id);
    EXPECT_EQ(id, ps->transactionId());
}

// =============================================================================
// Allocation Modes
// =============================================================================

TEST_F(TransactionScopePoolTest, DefaultAllocMode) {
    TransactionScopePool pool(SMALL_POOL, TransactionScopePool::ALLOC_DEFAULT);

    size_t idx;
    auto* scope = pool.acquire(idx);
    ASSERT_NE(nullptr, scope);

    if (idx != TransactionScopePool::INVALID_INDEX)
        pool.releaseByIndex(idx);
    else
        delete scope;
}

TEST_F(TransactionScopePoolTest, HugePagesAllocMode) {
    // HugePages may not be available — pool should fall back gracefully
    TransactionScopePool pool(SMALL_POOL, TransactionScopePool::ALLOC_HUGE_PAGES);

    size_t idx;
    auto* scope = pool.acquire(idx);
    ASSERT_NE(nullptr, scope);

    if (idx != TransactionScopePool::INVALID_INDEX)
        pool.releaseByIndex(idx);
    else
        delete scope;
}

TEST_F(TransactionScopePoolTest, NumaLocalAllocMode) {
    // NUMA may not be available — pool should fall back gracefully
    TransactionScopePool pool(SMALL_POOL, TransactionScopePool::ALLOC_NUMA_LOCAL);

    size_t idx;
    auto* scope = pool.acquire(idx);
    ASSERT_NE(nullptr, scope);

    if (idx != TransactionScopePool::INVALID_INDEX)
        pool.releaseByIndex(idx);
    else
        delete scope;
}

// =============================================================================
// Concurrent Acquire/Release
// =============================================================================

TEST_F(TransactionScopePoolTest, ConcurrentAcquireRelease) {
    constexpr size_t POOL_SIZE = 64;
    constexpr int THREADS = 4;
    constexpr int OPS_PER_THREAD = 200;

    TransactionScopePool pool(POOL_SIZE);
    std::atomic<int> successCount{0};

    auto worker = [&]() {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            size_t idx;
            TransactionScope* scope = pool.acquire(idx);
            ASSERT_NE(nullptr, scope);
            successCount.fetch_add(1, std::memory_order_relaxed);

            if (idx != TransactionScopePool::INVALID_INDEX) {
                pool.releaseByIndex(idx);
            } else {
                delete scope;
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

    EXPECT_EQ(THREADS * OPS_PER_THREAD, successCount.load());
}

} // namespace
