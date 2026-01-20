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
#include <memory>
#include <string>

#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "DataModelDef.h"
#include "QueuesDef.h"

using namespace COP;
using namespace COP::Queues;
using namespace COP::Store;

namespace {

// =============================================================================
// Setup Helpers
// =============================================================================

class BenchmarkSetup {
public:
    BenchmarkSetup() {
        WideDataStorage::create();
        IdTGenerator::create();
    }

    ~BenchmarkSetup() {
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }
};

OrderEvent createOrderEvent() {
    SourceIdT srcId, destId, clOrdId, origClOrdId, instrId, accountId, clearingId, execList;
    auto order = new OrderEntry(srcId, destId, clOrdId, origClOrdId, instrId, accountId, clearingId, execList);
    order->side_ = BUY_SIDE;
    order->ordType_ = LIMIT_ORDERTYPE;
    order->price_ = 100.0;
    order->orderQty_ = 100;
    return OrderEvent(order);
}

ExecutionEntry* createExecution() {
    auto exec = new ExecutionEntry();
    exec->execId_ = IdTGenerator::instance()->getId();
    exec->type_ = NEW_EXECTYPE;
    return exec;
}

} // namespace

// =============================================================================
// Queue Push Benchmarks
// =============================================================================

static void BM_QueuePushOrderEvent(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;

    for (auto _ : state) {
        OrderEvent event = createOrderEvent();
        queues.push("source", event);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePushOrderEvent)->Range(8, 8 << 10);

static void BM_QueuePushCancelEvent(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;

    for (auto _ : state) {
        OrderCancelEvent event;
        queues.push("source", event);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePushCancelEvent)->Range(8, 8 << 10);

static void BM_QueuePushReplaceEvent(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;

    for (auto _ : state) {
        OrderReplaceEvent event;
        queues.push("source", event);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePushReplaceEvent)->Range(8, 8 << 10);

static void BM_QueuePushTimerEvent(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;

    for (auto _ : state) {
        TimerEvent event;
        queues.push("source", event);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePushTimerEvent)->Range(8, 8 << 10);

// =============================================================================
// Queue Pop Benchmarks
// =============================================================================

static void BM_QueuePopEvents(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;

    // Pre-populate queue
    for (int64_t i = 0; i < state.range(0); ++i) {
        TimerEvent event;
        queues.push("source", event);
    }

    // Benchmark pop
    class TestProcessor : public InQueueProcessor {
    public:
        bool process() override { return false; }
        void onEvent(const std::string&, const OrderEvent&) override { ++count_; }
        void onEvent(const std::string&, const OrderCancelEvent&) override { ++count_; }
        void onEvent(const std::string&, const OrderReplaceEvent&) override { ++count_; }
        void onEvent(const std::string&, const OrderChangeStateEvent&) override { ++count_; }
        void onEvent(const std::string&, const ProcessEvent&) override { ++count_; }
        void onEvent(const std::string&, const TimerEvent&) override { ++count_; }
        int count_ = 0;
    };

    TestProcessor processor;

    for (auto _ : state) {
        queues.pop(&processor);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePopEvents)->Range(8, 8 << 10);

// =============================================================================
// Outgoing Queue Benchmarks
// =============================================================================

static void BM_OutQueuePushExecReport(benchmark::State& state) {
    BenchmarkSetup setup;
    OutgoingQueues queues;

    for (auto _ : state) {
        auto exec = createExecution();
        ExecReportEvent event(exec);
        queues.push(event, "target");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OutQueuePushExecReport)->Range(8, 8 << 10);

static void BM_OutQueuePushCancelReject(benchmark::State& state) {
    BenchmarkSetup setup;
    OutgoingQueues queues;

    for (auto _ : state) {
        CancelRejectEvent event;
        queues.push(event, "target");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OutQueuePushCancelReject)->Range(8, 8 << 10);

static void BM_OutQueuePushBusinessReject(benchmark::State& state) {
    BenchmarkSetup setup;
    OutgoingQueues queues;

    for (auto _ : state) {
        BusinessRejectEvent event;
        queues.push(event, "target");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OutQueuePushBusinessReject)->Range(8, 8 << 10);

// =============================================================================
// High Volume Throughput Benchmark
// =============================================================================

static void BM_QueueThroughput(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;
    const int batchSize = state.range(0);

    for (auto _ : state) {
        // Push batch
        for (int i = 0; i < batchSize; ++i) {
            TimerEvent event;
            queues.push("source", event);
        }

        // Pop batch
        class NullProcessor : public InQueueProcessor {
        public:
            bool process() override { return false; }
            void onEvent(const std::string&, const OrderEvent&) override {}
            void onEvent(const std::string&, const OrderCancelEvent&) override {}
            void onEvent(const std::string&, const OrderReplaceEvent&) override {}
            void onEvent(const std::string&, const OrderChangeStateEvent&) override {}
            void onEvent(const std::string&, const ProcessEvent&) override {}
            void onEvent(const std::string&, const TimerEvent&) override {}
        };
        NullProcessor processor;

        for (int i = 0; i < batchSize; ++i) {
            queues.pop(&processor);
        }
    }
    state.SetItemsProcessed(state.iterations() * batchSize * 2);
}
BENCHMARK(BM_QueueThroughput)->Range(64, 4096);

// =============================================================================
// Mixed Event Types Benchmark
// =============================================================================

static void BM_QueueMixedEvents(benchmark::State& state) {
    BenchmarkSetup setup;
    IncomingQueues queues;
    int iteration = 0;

    for (auto _ : state) {
        switch (iteration % 6) {
            case 0: {
                OrderEvent event = createOrderEvent();
                queues.push("source", event);
                break;
            }
            case 1: {
                OrderCancelEvent event;
                queues.push("source", event);
                break;
            }
            case 2: {
                OrderReplaceEvent event;
                queues.push("source", event);
                break;
            }
            case 3: {
                OrderChangeStateEvent event;
                queues.push("source", event);
                break;
            }
            case 4: {
                ProcessEvent event;
                queues.push("source", event);
                break;
            }
            case 5: {
                TimerEvent event;
                queues.push("source", event);
                break;
            }
        }
        ++iteration;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueueMixedEvents)->Range(8, 8 << 10);
