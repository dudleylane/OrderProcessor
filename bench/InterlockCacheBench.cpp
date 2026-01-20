/**
 * Concurrent Order Processor library - Google Benchmark
 *
 * Author: Sergey Mikhailik
 * Benchmark Implementation: 2026
 *
 * Copyright (C) 2009-2026 Sergey Mikhailik
 *
 * Distributed under the GNU General Public License (GPL).
 */

#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>

#include "InterLockCache.h"
#include "Logger.h"

using namespace aux;

namespace {

// =============================================================================
// Test Object for Cache
// =============================================================================

struct TestObject {
    int value;
    char padding[60];  // Make it 64 bytes for cache line alignment

    TestObject() : value(0) {}
    explicit TestObject(int v) : value(v) {}
};

// Define the static instance pointer
template<>
InterLockCache<TestObject>* InterLockCache<TestObject>::instance_ = nullptr;

// =============================================================================
// Benchmark Setup - manages singleton lifecycle
// =============================================================================

class CacheBenchmarkSetup {
public:
    CacheBenchmarkSetup(size_t cacheSize = 1024) {
        ExchLogger::create();
        InterLockCache<TestObject>::create("bench_cache", cacheSize);
    }

    ~CacheBenchmarkSetup() {
        InterLockCache<TestObject>::destroy();
        ExchLogger::destroy();
    }

    InterLockCache<TestObject>* cache() {
        return InterLockCache<TestObject>::instance();
    }
};

} // namespace

// =============================================================================
// Single-Threaded Push Benchmarks
// =============================================================================

static void BM_CachePushBack(benchmark::State& state) {
    CacheBenchmarkSetup setup(1024);
    auto* cache = setup.cache();

    // First pop items so we have something to push back
    std::vector<TestObject*> items;
    for (int i = 0; i < 512; ++i) {
        TestObject* item = cache->popFront();
        if (item) items.push_back(item);
    }

    size_t idx = 0;
    for (auto _ : state) {
        if (idx < items.size()) {
            cache->pushBack(items[idx]);
            // Pop it again for next iteration
            TestObject* item = cache->popFront();
            if (item) items[idx] = item;
            ++idx;
            if (idx >= items.size()) idx = 0;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CachePushBack)->Range(8, 8 << 10);

// =============================================================================
// Single-Threaded Pop Benchmarks
// =============================================================================

static void BM_CachePopFront(benchmark::State& state) {
    CacheBenchmarkSetup setup(2048);
    auto* cache = setup.cache();

    std::vector<TestObject*> popped;
    popped.reserve(1024);

    for (auto _ : state) {
        TestObject* obj = cache->popFront();
        if (obj) {
            benchmark::DoNotOptimize(obj);
            popped.push_back(obj);

            // Push back periodically to avoid exhausting cache
            if (popped.size() >= 512) {
                for (auto* item : popped) {
                    cache->pushBack(item);
                }
                popped.clear();
            }
        }
    }

    // Push back remaining items
    for (auto* item : popped) {
        cache->pushBack(item);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CachePopFront)->Range(8, 8 << 10);

// =============================================================================
// Push/Pop Cycle Benchmarks
// =============================================================================

static void BM_CacheCycle(benchmark::State& state) {
    CacheBenchmarkSetup setup(1024);
    auto* cache = setup.cache();

    for (auto _ : state) {
        // Pop
        TestObject* obj = cache->popFront();
        benchmark::DoNotOptimize(obj);

        // Push back
        if (obj) {
            cache->pushBack(obj);
        }
    }
    state.SetItemsProcessed(state.iterations() * 2);  // 2 operations per iteration
}
BENCHMARK(BM_CacheCycle)->Range(8, 8 << 10);

// =============================================================================
// Batch Operations Benchmarks
// =============================================================================

static void BM_CacheBatchPush(benchmark::State& state) {
    const int batchSize = state.range(0);
    CacheBenchmarkSetup setup(4096);
    auto* cache = setup.cache();

    // Pre-pop items for pushing
    std::vector<TestObject*> items;
    for (int i = 0; i < batchSize; ++i) {
        TestObject* item = cache->popFront();
        if (item) items.push_back(item);
    }

    for (auto _ : state) {
        // Push all items
        for (auto* item : items) {
            cache->pushBack(item);
        }

        state.PauseTiming();
        // Pop them back for next iteration
        items.clear();
        for (int i = 0; i < batchSize; ++i) {
            TestObject* item = cache->popFront();
            if (item) items.push_back(item);
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * batchSize);
}
BENCHMARK(BM_CacheBatchPush)->Range(64, 2048);

static void BM_CacheBatchPop(benchmark::State& state) {
    const int batchSize = state.range(0);
    CacheBenchmarkSetup setup(4096);
    auto* cache = setup.cache();

    std::vector<TestObject*> items;
    items.reserve(batchSize);

    for (auto _ : state) {
        // Pop batch
        items.clear();
        for (int i = 0; i < batchSize; ++i) {
            TestObject* obj = cache->popFront();
            if (obj) {
                items.push_back(obj);
            }
        }
        benchmark::DoNotOptimize(items.size());

        state.PauseTiming();
        // Push them back for next iteration
        for (auto* item : items) {
            cache->pushBack(item);
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * batchSize);
}
BENCHMARK(BM_CacheBatchPop)->Range(64, 2048);

// =============================================================================
// Empty Cache Behavior Benchmarks
// =============================================================================

static void BM_CacheEmptyPop(benchmark::State& state) {
    CacheBenchmarkSetup setup(10);  // Small cache
    auto* cache = setup.cache();

    // Exhaust the cache
    std::vector<TestObject*> exhausted;
    for (int i = 0; i < 100; ++i) {
        TestObject* item = cache->popFront();
        if (item) exhausted.push_back(item);
        else break;
    }

    for (auto _ : state) {
        // Pop from empty/near-empty cache (will allocate new)
        TestObject* obj = cache->popFront();
        benchmark::DoNotOptimize(obj);
        if (obj) {
            cache->pushBack(obj);
        }
    }

    // Restore items
    for (auto* item : exhausted) {
        cache->pushBack(item);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CacheEmptyPop)->Range(8, 8 << 10);

// =============================================================================
// Latency Measurement Benchmarks
// =============================================================================

static void BM_CachePushLatency(benchmark::State& state) {
    CacheBenchmarkSetup setup(1024);
    auto* cache = setup.cache();

    // Pop an item to push
    TestObject* item = cache->popFront();

    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();
        cache->pushBack(item);
        auto end = std::chrono::high_resolution_clock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        state.SetIterationTime(elapsed.count() / 1e9);

        // Pop for next iteration
        item = cache->popFront();
    }

    if (item) cache->pushBack(item);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CachePushLatency)->UseManualTime();

static void BM_CachePopLatency(benchmark::State& state) {
    CacheBenchmarkSetup setup(2048);
    auto* cache = setup.cache();

    std::vector<TestObject*> popped;
    popped.reserve(1024);

    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();
        TestObject* obj = cache->popFront();
        auto end = std::chrono::high_resolution_clock::now();

        benchmark::DoNotOptimize(obj);

        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        state.SetIterationTime(elapsed.count() / 1e9);

        if (obj) {
            popped.push_back(obj);
            // Push back periodically
            if (popped.size() >= 512) {
                for (auto* item : popped) {
                    cache->pushBack(item);
                }
                popped.clear();
            }
        }
    }

    // Push back remaining
    for (auto* item : popped) {
        cache->pushBack(item);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CachePopLatency)->UseManualTime();

// =============================================================================
// Cache Size Scaling Benchmarks
// =============================================================================

static void BM_CacheSizeScaling64(benchmark::State& state) {
    CacheBenchmarkSetup setup(64);
    auto* cache = setup.cache();

    for (auto _ : state) {
        TestObject* obj = cache->popFront();
        benchmark::DoNotOptimize(obj);
        if (obj) cache->pushBack(obj);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_CacheSizeScaling64);

static void BM_CacheSizeScaling256(benchmark::State& state) {
    CacheBenchmarkSetup setup(256);
    auto* cache = setup.cache();

    for (auto _ : state) {
        TestObject* obj = cache->popFront();
        benchmark::DoNotOptimize(obj);
        if (obj) cache->pushBack(obj);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_CacheSizeScaling256);

static void BM_CacheSizeScaling1024(benchmark::State& state) {
    CacheBenchmarkSetup setup(1024);
    auto* cache = setup.cache();

    for (auto _ : state) {
        TestObject* obj = cache->popFront();
        benchmark::DoNotOptimize(obj);
        if (obj) cache->pushBack(obj);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_CacheSizeScaling1024);

static void BM_CacheSizeScaling4096(benchmark::State& state) {
    CacheBenchmarkSetup setup(4096);
    auto* cache = setup.cache();

    for (auto _ : state) {
        TestObject* obj = cache->popFront();
        benchmark::DoNotOptimize(obj);
        if (obj) cache->pushBack(obj);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_CacheSizeScaling4096);
