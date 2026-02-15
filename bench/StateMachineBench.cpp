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
#include "OrderBookImpl.h"
#include "DataModelDef.h"
#include "Logger.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::OrdState;
using test::DummyOrderSaver;

namespace {

// =============================================================================
// Benchmark Setup - includes OrderBook for proper state machine operation
// =============================================================================

class StateMachineBenchmarkSetup {
public:
    StateMachineBenchmarkSetup() {
        aux::ExchLogger::create();
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();

        // Add test instrument
        auto instr = new InstrumentEntry();
        instr->symbol_ = "BENCH";
        instr->securityId_ = "BENCHSEC";
        instr->securityIdSource_ = "ISIN";
        instrId_ = WideDataStorage::instance()->add(instr);

        // Initialize order book (required for some state transitions)
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(instrId_);
        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments, &orderSaver_);
    }

    ~StateMachineBenchmarkSetup() {
        orderBook_.reset();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
        aux::ExchLogger::destroy();
    }

    OrderEntry* createOrder() {
        SourceIdT srcId, destId, origClOrdId, accountId, clearingId, execList;

        // Create a clOrderId
        static int clOrdIdCounter = 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "SMCLORD%d", ++clOrdIdCounter);
        auto* rawClOrdId = new RawDataEntry(STRING_RAWDATATYPE, buf, static_cast<u32>(strlen(buf)));
        SourceIdT clOrdId = WideDataStorage::instance()->add(rawClOrdId);

        auto order = new OrderEntry(srcId, destId, clOrdId, origClOrdId, instrId_, accountId, clearingId, execList);
        order->side_ = BUY_SIDE;
        order->price_ = 100.0;
        order->orderQty_ = 100;
        order->leavesQty_ = 100;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

    // Create an event with required dependencies
    template<typename EventT>
    EventT createEvent() {
        EventT evnt;
        evnt.generator_ = IdTGenerator::instance();
        evnt.orderStorage_ = OrderStorage::instance();
        evnt.orderBook_ = orderBook_.get();
        evnt.testStateMachine_ = true;
        evnt.testStateMachineCheckResult_ = true;
        return evnt;
    }

    SourceIdT instrId_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    DummyOrderSaver orderSaver_;
};

} // namespace

// =============================================================================
// State Machine Creation Benchmark
// =============================================================================

static void BM_StateMachineCreate(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();
        benchmark::DoNotOptimize(stateMachine.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateMachineCreate)->Range(8, 8 << 10);

// =============================================================================
// State Transition Benchmarks - Using actual process_event() calls
// =============================================================================

static void BM_StateTransitionOrderReceived(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();

        // Actually process the onOrderReceived event through state machine
        onOrderReceived evnt = setup.createEvent<onOrderReceived>();
        stateMachine->process_event(evnt);

        benchmark::DoNotOptimize(stateMachine->getPersistence());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionOrderReceived)->Range(8, 8 << 10);

static void BM_StateTransitionCancelReceived(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();

        // First accept the order
        onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
        stateMachine->process_event(acceptEvnt);

        // Then process cancel received
        onCancelReceived cancelEvnt = setup.createEvent<onCancelReceived>();
        stateMachine->process_event(cancelEvnt);

        benchmark::DoNotOptimize(stateMachine->getPersistence());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionCancelReceived)->Range(8, 8 << 10);

static void BM_StateTransitionSuspend(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();

        // First accept the order
        onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
        stateMachine->process_event(acceptEvnt);

        // Then suspend
        onSuspended suspendEvnt = setup.createEvent<onSuspended>();
        stateMachine->process_event(suspendEvnt);

        benchmark::DoNotOptimize(stateMachine->getPersistence());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionSuspend)->Range(8, 8 << 10);

static void BM_StateTransitionExpire(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();

        // First accept the order
        onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
        stateMachine->process_event(acceptEvnt);

        // Then expire
        onExpired expireEvnt = setup.createEvent<onExpired>();
        stateMachine->process_event(expireEvnt);

        benchmark::DoNotOptimize(stateMachine->getPersistence());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateTransitionExpire)->Range(8, 8 << 10);

// =============================================================================
// Full Order Lifecycle Benchmarks - Multiple transitions
// =============================================================================

static void BM_OrderLifecycleAcceptToSuspendToResume(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();

        // Accept
        onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
        stateMachine->process_event(acceptEvnt);

        // Suspend
        onSuspended suspendEvnt = setup.createEvent<onSuspended>();
        stateMachine->process_event(suspendEvnt);

        // Resume (Continue)
        onContinue continueEvnt = setup.createEvent<onContinue>();
        stateMachine->process_event(continueEvnt);

        benchmark::DoNotOptimize(stateMachine->getPersistence());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderLifecycleAcceptToSuspendToResume)->Range(8, 8 << 10);

static void BM_OrderLifecycleAcceptToCancelToRejected(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder();
        auto stateMachine = std::make_unique<OrderState>(order);
        stateMachine->start();

        // Accept order
        onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
        stateMachine->process_event(acceptEvnt);

        // Cancel received
        onCancelReceived cancelEvnt = setup.createEvent<onCancelReceived>();
        stateMachine->process_event(cancelEvnt);

        // Cancel rejected
        onCancelRejected rejectEvnt = setup.createEvent<onCancelRejected>();
        stateMachine->process_event(rejectEvnt);

        benchmark::DoNotOptimize(stateMachine->getPersistence());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderLifecycleAcceptToCancelToRejected)->Range(8, 8 << 10);

// =============================================================================
// State Persistence Benchmarks
// =============================================================================

static void BM_StateMachineGetPersistence(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;
    OrderEntry* order = setup.createOrder();
    auto stateMachine = std::make_unique<OrderState>(order);
    stateMachine->start();

    // Pre-transition to a meaningful state
    onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
    stateMachine->process_event(acceptEvnt);

    for (auto _ : state) {
        auto persistence = stateMachine->getPersistence();
        benchmark::DoNotOptimize(persistence);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateMachineGetPersistence)->Range(8, 8 << 10);

static void BM_StateMachineSetPersistence(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;
    OrderEntry* order = setup.createOrder();
    auto stateMachine = std::make_unique<OrderState>(order);
    stateMachine->start();

    // Get a persistence to restore
    onOrderReceived acceptEvnt = setup.createEvent<onOrderReceived>();
    stateMachine->process_event(acceptEvnt);
    auto persistence = stateMachine->getPersistence();

    for (auto _ : state) {
        stateMachine->setPersistance(persistence);
        benchmark::DoNotOptimize(stateMachine.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StateMachineSetPersistence)->Range(8, 8 << 10);

// =============================================================================
// Batch State Transition Benchmarks
// =============================================================================

static void BM_BatchStateTransitions(benchmark::State& state) {
    StateMachineBenchmarkSetup setup;
    const int batchSize = state.range(0);

    // Pre-create orders and state machines
    std::vector<OrderEntry*> orders;
    std::vector<std::unique_ptr<OrderState>> machines;
    for (int i = 0; i < batchSize; ++i) {
        orders.push_back(setup.createOrder());
        auto sm = std::make_unique<OrderState>(orders.back());
        sm->start();
        machines.push_back(std::move(sm));
    }

    for (auto _ : state) {
        // Process accept event for all
        for (auto& sm : machines) {
            onOrderReceived evnt = setup.createEvent<onOrderReceived>();
            sm->process_event(evnt);
        }
        // Process suspend for all
        for (auto& sm : machines) {
            onSuspended evnt = setup.createEvent<onSuspended>();
            sm->process_event(evnt);
        }
        // Process continue for all
        for (auto& sm : machines) {
            onContinue evnt = setup.createEvent<onContinue>();
            sm->process_event(evnt);
        }
    }
    state.SetItemsProcessed(state.iterations() * batchSize * 3);
}
BENCHMARK(BM_BatchStateTransitions)->RangeMultiplier(4)->Range(16, 256);

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
            machine->start();
            machines.push_back(std::move(machine));
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StateMachineMemoryUsage)->RangeMultiplier(4)->Range(64, 1024);
