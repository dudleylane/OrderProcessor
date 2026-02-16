/**
 * Concurrent Order Processor library - Google Benchmark
 *
 * TransactionScopePool benchmarks: single-threaded acquire/release,
 * concurrent acquire/release, pool exhaustion with heap fallback,
 * and PooledTransactionScope RAII overhead.
 */

#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>

#include "TransactionScopePool.h"

using namespace COP::ACID;

// =============================================================================
// Single-Threaded Acquire/Release
// =============================================================================

static void BM_PoolAcquireRelease(benchmark::State& state) {
    TransactionScopePool pool(static_cast<size_t>(state.range(0)));

    for (auto _ : state) {
        size_t idx;
        TransactionScope* scope = pool.acquire(idx);
        benchmark::DoNotOptimize(scope);

        if (idx != TransactionScopePool::INVALID_INDEX) {
            pool.releaseByIndex(idx);
        } else {
            delete scope;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolAcquireRelease)->Arg(64)->Arg(256)->Arg(1024);

// =============================================================================
// PooledTransactionScope RAII Cycle
// =============================================================================

static void BM_PooledScopeRAII(benchmark::State& state) {
    TransactionScopePool pool(static_cast<size_t>(state.range(0)));

    for (auto _ : state) {
        PooledTransactionScope ps(&pool);
        benchmark::DoNotOptimize(ps.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PooledScopeRAII)->Arg(64)->Arg(256)->Arg(1024);

// =============================================================================
// Detach (Release for External Ownership)
// =============================================================================

static void BM_PoolDetach(benchmark::State& state) {
    TransactionScopePool pool(static_cast<size_t>(state.range(0)));

    for (auto _ : state) {
        PooledTransactionScope ps(&pool);
        TransactionScope* released = ps.release();
        benchmark::DoNotOptimize(released);
        delete released;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolDetach)->Arg(64)->Arg(256)->Arg(1024);

// =============================================================================
// Concurrent Acquire/Release (Multi-Threaded)
// =============================================================================

static void BM_PoolConcurrentAcquireRelease(benchmark::State& state) {
    const int numThreads = static_cast<int>(state.range(0));
    TransactionScopePool pool(1024);

    for (auto _ : state) {
        std::atomic<int> ops{0};
        const int opsPerThread = 1000;

        std::vector<std::thread> threads;
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < opsPerThread; ++i) {
                    size_t idx;
                    TransactionScope* scope = pool.acquire(idx);
                    if (idx != TransactionScopePool::INVALID_INDEX) {
                        pool.releaseByIndex(idx);
                    } else {
                        delete scope;
                    }
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }

        state.SetItemsProcessed(ops.load());
    }
}
BENCHMARK(BM_PoolConcurrentAcquireRelease)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

// =============================================================================
// Exhaustion Fallback Cost
// =============================================================================

static void BM_PoolExhaustion(benchmark::State& state) {
    // Tiny pool to force frequent heap fallback
    TransactionScopePool pool(4);

    // Hold all pool slots so every acquire falls back to heap
    std::vector<std::pair<TransactionScope*, size_t>> held;
    for (int i = 0; i < 4; ++i) {
        size_t idx;
        TransactionScope* s = pool.acquire(idx);
        held.push_back({s, idx});
    }

    for (auto _ : state) {
        size_t idx;
        TransactionScope* scope = pool.acquire(idx);
        benchmark::DoNotOptimize(scope);
        delete scope;  // heap-allocated
    }

    // Release held slots
    for (auto& [scope, idx] : held) {
        if (idx != TransactionScopePool::INVALID_INDEX) {
            pool.releaseByIndex(idx);
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolExhaustion);

// =============================================================================
// Arena Allocate vs Heap (within TransactionScope)
// =============================================================================

static void BM_ArenaAllocate(benchmark::State& state) {
    for (auto _ : state) {
        TransactionScope scope;
        void* ptr = scope.arenaAllocate(128, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArenaAllocate);

static void BM_HeapAllocate(benchmark::State& state) {
    for (auto _ : state) {
        auto* ptr = new char[128];
        benchmark::DoNotOptimize(ptr);
        delete[] ptr;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapAllocate);
