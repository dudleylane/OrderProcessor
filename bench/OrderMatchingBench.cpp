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
#include <random>

#include "OrderMatcher.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "DataModelDef.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::Proc;
using test::DummyOrderSaver;

namespace {

// =============================================================================
// Benchmark Setup
// =============================================================================

class OrderBookBenchmarkSetup {
public:
    OrderBookBenchmarkSetup() {
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();

        // Add test instrument
        auto instr = new InstrumentEntry();
        instr->symbol_ = "BENCH";
        instr->securityId_ = "BENCHSEC";
        instr->securityIdSource_ = "ISIN";
        instrId_ = WideDataStorage::instance()->add(instr);

        // Initialize order book
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(instrId_);
        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments, &orderSaver_);
    }

    ~OrderBookBenchmarkSetup() {
        orderBook_.reset();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    OrderEntry* createOrder(Side side, PriceT price, QuantityT qty) {
        SourceIdT srcId, destId, clOrdId, origClOrdId, accountId, clearingId, execList;
        auto order = new OrderEntry(srcId, destId, clOrdId, origClOrdId, instrId_, accountId, clearingId, execList);
        order->side_ = side;
        order->price_ = price;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->status_ = NEW_ORDSTATUS;
        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

    SourceIdT instrId_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    DummyOrderSaver orderSaver_;
};

class OrderFunctorImpl : public OrderFunctor {
public:
    OrderFunctorImpl(const SourceIdT& instr, Side side, bool result)
        : instr_(instr), result_(result) {
        side_ = side;
    }

    SourceIdT instrument() const override { return instr_; }
    bool match(const IdT&, bool* stop) const override {
        if (stop) *stop = false;
        return result_;
    }

    SourceIdT instr_;
    bool result_;
};

} // namespace

// =============================================================================
// Order Book Add Benchmarks
// =============================================================================

static void BM_OrderBookAddBuy(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;
    double price = 100.0;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder(BUY_SIDE, price, 100);
        setup.orderBook_->add(*order);
        price += 0.01;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookAddBuy)->Range(8, 8 << 10);

static void BM_OrderBookAddSell(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;
    double price = 100.0;

    for (auto _ : state) {
        OrderEntry* order = setup.createOrder(SELL_SIDE, price, 100);
        setup.orderBook_->add(*order);
        price += 0.01;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookAddSell)->Range(8, 8 << 10);

static void BM_OrderBookAddMixed(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;
    double price = 100.0;
    int iteration = 0;

    for (auto _ : state) {
        Side side = (iteration % 2 == 0) ? BUY_SIDE : SELL_SIDE;
        OrderEntry* order = setup.createOrder(side, price, 100);
        setup.orderBook_->add(*order);
        price += 0.01;
        ++iteration;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookAddMixed)->Range(8, 8 << 10);

// =============================================================================
// Order Book Remove Benchmarks
// =============================================================================

static void BM_OrderBookRemove(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;
    std::vector<OrderEntry*> orders;

    // Pre-populate order book
    for (int64_t i = 0; i < state.range(0); ++i) {
        OrderEntry* order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
        orders.push_back(order);
    }

    size_t index = 0;
    for (auto _ : state) {
        if (index < orders.size()) {
            setup.orderBook_->remove(*orders[index]);
            ++index;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookRemove)->Range(8, 8 << 10);

// =============================================================================
// Order Book GetTop Benchmarks
// =============================================================================

static void BM_OrderBookGetTopBuy(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i) {
        OrderEntry* order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    for (auto _ : state) {
        IdT topId = setup.orderBook_->getTop(setup.instrId_, BUY_SIDE);
        benchmark::DoNotOptimize(topId);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookGetTopBuy)->Range(8, 8 << 10);

static void BM_OrderBookGetTopSell(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i) {
        OrderEntry* order = setup.createOrder(SELL_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    for (auto _ : state) {
        IdT topId = setup.orderBook_->getTop(setup.instrId_, SELL_SIDE);
        benchmark::DoNotOptimize(topId);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookGetTopSell)->Range(8, 8 << 10);

// =============================================================================
// Order Book Find Benchmarks
// =============================================================================

static void BM_OrderBookFind(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i) {
        OrderEntry* order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    OrderFunctorImpl functor(setup.instrId_, BUY_SIDE, true);

    for (auto _ : state) {
        IdT result = setup.orderBook_->find(functor);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookFind)->Range(8, 8 << 10);

static void BM_OrderBookFindAll(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i) {
        OrderEntry* order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    OrderFunctorImpl functor(setup.instrId_, BUY_SIDE, true);
    OrderBook::OrdersT results;

    for (auto _ : state) {
        results.clear();
        setup.orderBook_->findAll(functor, &results);
        benchmark::DoNotOptimize(results.size());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookFindAll)->Range(8, 8 << 10);

// =============================================================================
// Order Book Depth Scaling Benchmarks
// =============================================================================

static void BM_OrderBookDepthAdd(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;
    const int depth = state.range(0);

    // Pre-populate with depth levels
    for (int i = 0; i < depth; ++i) {
        OrderEntry* buyOrder = setup.createOrder(BUY_SIDE, 100.0 - i * 0.01, 100);
        setup.orderBook_->add(*buyOrder);

        OrderEntry* sellOrder = setup.createOrder(SELL_SIDE, 101.0 + i * 0.01, 100);
        setup.orderBook_->add(*sellOrder);
    }

    double price = 105.0;
    for (auto _ : state) {
        OrderEntry* order = setup.createOrder(BUY_SIDE, price, 100);
        setup.orderBook_->add(*order);
        price += 0.01;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookDepthAdd)->RangeMultiplier(4)->Range(16, 4096);

// =============================================================================
// Order Book Worst Case Benchmarks
// =============================================================================

static void BM_OrderBookWorstCaseAdd(benchmark::State& state) {
    OrderBookBenchmarkSetup setup;
    std::mt19937 gen(42);
    std::uniform_real_distribution<> priceDist(90.0, 110.0);

    for (auto _ : state) {
        double price = priceDist(gen);
        Side side = (gen() % 2 == 0) ? BUY_SIDE : SELL_SIDE;
        OrderEntry* order = setup.createOrder(side, price, 100);
        setup.orderBook_->add(*order);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookWorstCaseAdd)->Range(8, 8 << 10);
