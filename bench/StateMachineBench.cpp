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
#include <vector>

#include "StateMachine.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "DataModelDef.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::OrdState;

namespace {

// =============================================================================
// Benchmark Setup
// =============================================================================

class StateMachineBenchmarkSetup {
public:
    StateMachineBenchmarkSetup() {
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();

        // Add test instrument
        auto instr = new InstrumentEntry();
        instr->symbol_ = "BENCH";
        instr->securityId_ = "BENCHSEC";
        instr->securityIdSource_ = "ISIN";
        instrId_ = WideDataStorage::instance()->add(instr);
    }

    ~StateMachineBenchmarkSetup() {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    OrderEntry* createOrder() {
        SourceIdT srcId, destId, clOrdId, origClOrdId, accountId, clearingId, execList;
        auto order = new OrderEntry(srcId, destId, clOrdId, origClOrdId, instrId_, accountId, clearingId, execList);
        order->side_ = BUY_SIDE;
        order->price_ = 100.0;
        order->orderQty_ = 100;
        order->leavesQty_ = 100;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

    SourceIdT instrId_;
};

} // namespace

// =============================================================================
// State Machine Creation Benchmarks
// =============================================================================

static void BM_StateMachineCreate(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        benchmark::DoNotOptimize(stateMachine.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateMachineCreate)->Range(8, 8 << 10);

// =============================================================================
// State Transition Benchmarks - New Order Flow
// =============================================================================

static void BM_StateTransitionReceivedToNew(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        order->status_ = RECEIVEDNEW_ORDSTATUS;

        auto stateMachine = std::make_unique<OrderState>(order);

        // Transition to NEW
        order->status_ = NEW_ORDSTATUS;
        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionReceivedToNew)->Range(8, 8 << 10);

static void BM_StateTransitionNewToPartialFill(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        order->status_ = NEW_ORDSTATUS;

        auto stateMachine = std::make_unique<OrderState>(order);

        // Transition to PARTIAL FILL
        order->status_ = PARTFILL_ORDSTATUS;
        order->cumQty_ = 50;
        order->leavesQty_ = 50;
        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionNewToPartialFill)->Range(8, 8 << 10);

static void BM_StateTransitionNewToFilled(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        order->status_ = NEW_ORDSTATUS;

        auto stateMachine = std::make_unique<OrderState>(order);

        // Transition to FILLED
        order->status_ = FILLED_ORDSTATUS;
        order->cumQty_ = 100;
        order->leavesQty_ = 0;
        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionNewToFilled)->Range(8, 8 << 10);

static void BM_StateTransitionNewToCanceled(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        order->status_ = NEW_ORDSTATUS;

        auto stateMachine = std::make_unique<OrderState>(order);

        // Transition to CANCELED
        order->status_ = CANCELED_ORDSTATUS;
        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionNewToCanceled)->Range(8, 8 << 10);

// =============================================================================
// Full Order Lifecycle Benchmarks
// =============================================================================

static void BM_OrderLifecycleNewToFilled(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();

        // RECEIVED_NEW
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        auto stateMachine = std::make_unique<OrderState>(order);

        // NEW
        order->status_ = NEW_ORDSTATUS;

        // PARTIAL FILL
        order->status_ = PARTFILL_ORDSTATUS;
        order->cumQty_ = 50;
        order->leavesQty_ = 50;

        // FILLED
        order->status_ = FILLED_ORDSTATUS;
        order->cumQty_ = 100;
        order->leavesQty_ = 0;

        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderLifecycleNewToFilled)->Range(8, 8 << 10);

static void BM_OrderLifecycleNewToCanceled(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();

        // RECEIVED_NEW
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        auto stateMachine = std::make_unique<OrderState>(order);

        // NEW
        order->status_ = NEW_ORDSTATUS;

        // CANCELED
        order->status_ = CANCELED_ORDSTATUS;

        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderLifecycleNewToCanceled)->Range(8, 8 << 10);

static void BM_OrderLifecycleWithReplace(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();

        // RECEIVED_NEW
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        auto stateMachine = std::make_unique<OrderState>(order);

        // NEW
        order->status_ = NEW_ORDSTATUS;

        // PENDING REPLACE
        order->status_ = PENDINGREPLACE_ORDSTATUS;

        // REPLACED (back to NEW with new values)
        order->status_ = NEW_ORDSTATUS;
        order->price_ = 105.0;
        order->orderQty_ = 150;

        // FILLED
        order->status_ = FILLED_ORDSTATUS;
        order->cumQty_ = 150;
        order->leavesQty_ = 0;

        benchmark::DoNotOptimize(order->status_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderLifecycleWithReplace)->Range(8, 8 << 10);

// =============================================================================
// Batch State Transition Benchmarks
// =============================================================================

static void BM_BatchStateTransitions(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;
    const int batchSize = state.range(0);
    std::vector<OrderEntry*> orders;

    // Pre-create orders
    for (int i = 0; i < batchSize; ++i) {
        orders.push_back(setup.createOrder());
    }

    for (auto _ : state) {
        for (auto* order : orders) {
            order->status_ = NEW_ORDSTATUS;
        }
        for (auto* order : orders) {
            order->status_ = PARTFILL_ORDSTATUS;
        }
        for (auto* order : orders) {
            order->status_ = FILLED_ORDSTATUS;
        }
    }
    state.SetItemsProcessed(state.iterations() * batchSize * 3);
}
BENCHMARK(BM_BatchStateTransitions)->RangeMultiplier(4)->Range(16, 1024);

// =============================================================================
// State Machine Memory Benchmark
// =============================================================================

static void BM_StateMachineMemoryUsage(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;
    std::vector<std::unique_ptr<OrderState>> machines;
    machines.reserve(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        machines.clear();
        state.ResumeTiming();

        for (int64_t i = 0; i < state.range(0); ++i) {
            OrderEntry* order = setup.createOrder();
            auto machine = std::make_unique<OrderState>(order);
            machines.push_back(std::move(machine));
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StateMachineMemoryUsage)->RangeMultiplier(4)->Range(64, 4096);
