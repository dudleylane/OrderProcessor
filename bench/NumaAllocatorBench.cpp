/**
 * Concurrent Order Processor library - Google Benchmark
 *
 * NumaAllocator and HugePages benchmarks: allocation throughput,
 * NUMA-local vs cross-node access, and comparison with standard mmap.
 */

#include <benchmark/benchmark.h>
#include <cstring>

#include "NumaAllocator.h"
#include "HugePages.h"

using namespace COP;

// =============================================================================
// NumaAllocator::allocateLocal Throughput
// =============================================================================

static void BM_NumaAllocateLocal(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        void* ptr = NumaAllocator::allocateLocal(size);
        benchmark::DoNotOptimize(ptr);
        if (ptr) {
            NumaAllocator::deallocate(ptr, size);
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}
BENCHMARK(BM_NumaAllocateLocal)->Arg(4096)->Arg(65536)->Arg(1 << 20);

// =============================================================================
// NumaAllocator::allocateOnNode Throughput
// =============================================================================

static void BM_NumaAllocateOnNode(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        void* ptr = NumaAllocator::allocateOnNode(size, 0);
        benchmark::DoNotOptimize(ptr);
        if (ptr) {
            NumaAllocator::deallocate(ptr, size);
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}
BENCHMARK(BM_NumaAllocateOnNode)->Arg(4096)->Arg(65536)->Arg(1 << 20);

// =============================================================================
// HugePages::allocate Throughput (with fallback)
// =============================================================================

static void BM_HugePagesAllocate(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        void* ptr = HugePages::allocate(size, true);
        benchmark::DoNotOptimize(ptr);
        if (ptr) {
            HugePages::deallocate(ptr, size);
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}
BENCHMARK(BM_HugePagesAllocate)->Arg(4096)->Arg(1 << 20)->Arg(4 << 20);

// =============================================================================
// Memory Touch Latency: NUMA-local allocation
// =============================================================================

static void BM_NumaLocalMemoryTouch(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    void* ptr = NumaAllocator::allocateLocal(size);
    if (!ptr) {
        state.SkipWithMessage("allocateLocal failed");
        return;
    }

    for (auto _ : state) {
        std::memset(ptr, 0, size);
        benchmark::ClobberMemory();
    }

    NumaAllocator::deallocate(ptr, size);
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}
BENCHMARK(BM_NumaLocalMemoryTouch)->Arg(4096)->Arg(65536)->Arg(1 << 20);

// =============================================================================
// Memory Touch Latency: Standard mmap (baseline)
// =============================================================================

static void BM_StandardMmapMemoryTouch(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));

    // Standard allocation for comparison
    auto* ptr = new char[size];
    if (!ptr) {
        state.SkipWithMessage("allocation failed");
        return;
    }

    for (auto _ : state) {
        std::memset(ptr, 0, size);
        benchmark::ClobberMemory();
    }

    delete[] ptr;
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}
BENCHMARK(BM_StandardMmapMemoryTouch)->Arg(4096)->Arg(65536)->Arg(1 << 20);

// =============================================================================
// NUMA Topology Detection
// =============================================================================

static void BM_NumaNodeCount(benchmark::State& state) {
    for (auto _ : state) {
        int count = NumaAllocator::nodeCount();
        benchmark::DoNotOptimize(count);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NumaNodeCount);

static void BM_HugePagesRoundUp(benchmark::State& state) {
    for (auto _ : state) {
        size_t result = HugePages::roundUpToHugePage(state.range(0));
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HugePagesRoundUp)->Arg(1)->Arg(4096)->Arg(1 << 20)->Arg(3 << 20);
