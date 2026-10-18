/**
 * Concurrent Order Processor library - Google Benchmark
 *
 * Authors: dudleylane, Claude
 * Benchmark Implementation: 2026
 *
 * Copyright (C) 2026 dudleylane
 *
 * Distributed under the GNU Affero General Public License (AGPL).
 */

#include <algorithm>
#include <benchmark/benchmark.h>
#include <chrono>
#include <memory>
#include <vector>
#include <random>

#include "OrderMatcher.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "DataModelDef.h"
#include "Logger.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::Proc;
using test::DummyOrderSaver;

namespace
{

// =============================================================================
// Benchmark Setup
// =============================================================================

class OrderBookBenchmarkSetup
{
public:
    OrderBookBenchmarkSetup()
    {
        aux::ExchLogger::create();
        aux::ExchLogger::instance()->setNoteOn(false);  // measure the engine, not spdlog (#121)
        aux::ExchLogger::instance()->setDebugOn(false); // measure the engine, not debug logging (#76)
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
        orderBook_->init(instruments);
    }

    ~OrderBookBenchmarkSetup()
    {
        orderBook_.reset();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
        aux::ExchLogger::destroy();
    }

    OrderEntry *createOrder(Side side, PriceT price, QuantityT qty)
    {
        SourceIdT srcId, destId, origClOrdId, accountId, clearingId, execList;

        // Create a clOrderId
        static int clOrdIdCounter = 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "CLORD%d", ++clOrdIdCounter);
        auto *rawClOrdId = new RawDataEntry(STRING_RAWDATATYPE, buf, static_cast<u32>(strlen(buf)));
        SourceIdT clOrdId = WideDataStorage::instance()->add(rawClOrdId);

        // save() stores a clone; this order is only its template, freed on return.
        auto order = std::make_unique<OrderEntry>(srcId, destId, clOrdId, origClOrdId, instrId_, accountId, clearingId,
                                                  execList);
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

class OrderFunctorImpl : public OrderFunctor
{
public:
    OrderFunctorImpl(const SourceIdT &instr, Side side, bool result) : instr_(instr), result_(result)
    {
        side_ = side;
    }

    SourceIdT instrument() const override
    {
        return instr_;
    }
    bool match(const IdT &, bool *stop) const override
    {
        if (stop)
        {
            *stop = false;
        }
        return result_;
    }

    SourceIdT instr_;
    bool result_;
};

/// The time since start, for SetIterationTime() in a benchmark registered with UseManualTime(): a batch timed by hand,
/// with nothing else in it (#305). Pausing the timer instead reads the thread's CPU clock, a system call, and each pause
/// and resume counted about 1.5 us with the batch, more than a batch of 8 takes.
double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Times adding orders into a book that holds the orders it was given throughout, as BM_OrderBookRemove times removing
// them (#181): each iteration adds the orders made beforehand for it, timed by hand (#305), then takes them out again
// untimed. So add() alone is timed, and items processed counts adds. The book holds between its own depth and that plus
// the batch, which is at most 64 and never more than the depth. Until #293 each iteration made and stored its order
// inside the timed loop and left it in the book, so the book grew by an order an iteration, whatever the argument
// said, and every size timed the same work.
void timeAdds(benchmark::State &state, OrderBookBenchmarkSetup &setup, const std::vector<OrderEntry *> &adds)
{
    int64_t added = 0;
    for (auto _ : state)
    {
        const auto start = std::chrono::steady_clock::now();
        for (OrderEntry *order : adds)
        {
            setup.orderBook_->add(*order);
        }
        state.SetIterationTime(secondsSince(start));
        added += static_cast<int64_t>(adds.size());

        for (OrderEntry *order : adds)
        {
            setup.orderBook_->remove(*order);
        }
    }
    state.SetItemsProcessed(added);
}

/// The orders a batch adds into a book of depth orders
size_t addBatch(int64_t depth)
{
    return std::min<size_t>(static_cast<size_t>(depth), 64);
}

} // namespace

// =============================================================================
// Order Book Add Benchmarks
// =============================================================================

// Adds into a book of range(0) orders (#293). Each order in the book and in the batch is priced a tick above the one
// before it, so every buy added becomes the top of its side and every sell goes to the back of its side, as when the
// benchmarks added one order an iteration. The mixed benchmark alternates the sides.
static void addInSequence(benchmark::State &state, Side first, Side second)
{
    OrderBookBenchmarkSetup setup;
    const int64_t depth = state.range(0);
    const size_t batch = addBatch(depth);
    for (int64_t i = 0; i < depth; ++i)
    {
        setup.orderBook_->add(*setup.createOrder((0 == i % 2) ? first : second, 100.0 + i * 0.01, 100));
    }
    std::vector<OrderEntry *> adds;
    for (int64_t i = depth; i < depth + static_cast<int64_t>(batch); ++i)
    {
        adds.push_back(setup.createOrder((0 == i % 2) ? first : second, 100.0 + i * 0.01, 100));
    }
    timeAdds(state, setup, adds);
}

static void BM_OrderBookAddBuy(benchmark::State &state)
{
    addInSequence(state, BUY_SIDE, BUY_SIDE);
}
BENCHMARK(BM_OrderBookAddBuy)->Range(8, 8 << 10)->UseManualTime();

static void BM_OrderBookAddSell(benchmark::State &state)
{
    addInSequence(state, SELL_SIDE, SELL_SIDE);
}
BENCHMARK(BM_OrderBookAddSell)->Range(8, 8 << 10)->UseManualTime();

static void BM_OrderBookAddMixed(benchmark::State &state)
{
    addInSequence(state, BUY_SIDE, SELL_SIDE);
}
BENCHMARK(BM_OrderBookAddMixed)->Range(8, 8 << 10)->UseManualTime();

// =============================================================================
// Order Book Remove Benchmarks
// =============================================================================

// Removal from a book that holds range(0) orders throughout (#181). Each iteration removes a batch of them, timed by
// hand (#305), and puts them back untimed; items processed counts removals. Before #181 an iteration removed one order
// until the book was empty, so all but range(0) of millions timed nothing, and the benchmark reported 0 ns. Until #305
// the batch went back with the timer paused, and each pause and resume counted about 1.5 us, most of /8's time.
static void BM_OrderBookRemove(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;
    std::vector<OrderEntry *> orders;

    // Pre-populate order book
    for (int64_t i = 0; i < state.range(0); ++i)
    {
        OrderEntry *order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
        orders.push_back(order);
    }

    const size_t batch = std::min<size_t>(orders.size(), 64);
    size_t next = 0;
    int64_t removed = 0;
    for (auto _ : state)
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < batch; ++i)
        {
            setup.orderBook_->remove(*orders[(next + i) % orders.size()]);
        }
        state.SetIterationTime(secondsSince(start));
        removed += static_cast<int64_t>(batch);

        for (size_t i = 0; i < batch; ++i)
        {
            setup.orderBook_->add(*orders[(next + i) % orders.size()]);
        }
        next = (next + batch) % orders.size();
    }
    state.SetItemsProcessed(removed);
}
BENCHMARK(BM_OrderBookRemove)->Range(8, 8 << 10)->UseManualTime();

// =============================================================================
// Order Book GetTop Benchmarks
// =============================================================================

static void BM_OrderBookGetTopBuy(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i)
    {
        OrderEntry *order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    for (auto _ : state)
    {
        IdT topId = setup.orderBook_->getTop(setup.instrId_, BUY_SIDE);
        benchmark::DoNotOptimize(topId);
    }
    state.SetItemsProcessed(state.iterations());
}
// No argument: getTop() reads the first entry of its side, so the book's depth doesn't change it (#293)
BENCHMARK(BM_OrderBookGetTopBuy);

static void BM_OrderBookGetTopSell(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i)
    {
        OrderEntry *order = setup.createOrder(SELL_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    for (auto _ : state)
    {
        IdT topId = setup.orderBook_->getTop(setup.instrId_, SELL_SIDE);
        benchmark::DoNotOptimize(topId);
    }
    state.SetItemsProcessed(state.iterations());
}
// No argument, as BM_OrderBookGetTopBuy
BENCHMARK(BM_OrderBookGetTopSell);

// =============================================================================
// Order Book Find Benchmarks
// =============================================================================

static void BM_OrderBookFind(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int i = 0; i < 1000; ++i)
    {
        OrderEntry *order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    OrderFunctorImpl functor(setup.instrId_, BUY_SIDE, true);

    for (auto _ : state)
    {
        IdT result = setup.orderBook_->find(functor);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
// No argument: the functor matches the first order it is offered, so find() reads one entry whatever the book's depth
// (#293). BM_OrderBookFindAll walks them all.
BENCHMARK(BM_OrderBookFind);

// Walks a book of range(0) orders, each a match (#293; until then 1000 orders, whatever the argument)
static void BM_OrderBookFindAll(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;

    // Pre-populate order book
    for (int64_t i = 0; i < state.range(0); ++i)
    {
        OrderEntry *order = setup.createOrder(BUY_SIDE, 100.0 + i * 0.01, 100);
        setup.orderBook_->add(*order);
    }

    OrderFunctorImpl functor(setup.instrId_, BUY_SIDE, true);
    OrderBook::OrdersT results;

    for (auto _ : state)
    {
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

// Adds buys above a book with range(0) levels a side, which it now holds (#293): it grew by an order an iteration too
static void BM_OrderBookDepthAdd(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;
    const int64_t depth = state.range(0);

    // Pre-populate with depth levels
    for (int64_t i = 0; i < depth; ++i)
    {
        OrderEntry *buyOrder = setup.createOrder(BUY_SIDE, 100.0 - i * 0.01, 100);
        setup.orderBook_->add(*buyOrder);

        OrderEntry *sellOrder = setup.createOrder(SELL_SIDE, 101.0 + i * 0.01, 100);
        setup.orderBook_->add(*sellOrder);
    }

    std::vector<OrderEntry *> adds;
    for (size_t i = 0; i < addBatch(depth); ++i)
    {
        adds.push_back(setup.createOrder(BUY_SIDE, 105.0 + i * 0.01, 100));
    }
    timeAdds(state, setup, adds);
}
BENCHMARK(BM_OrderBookDepthAdd)->RangeMultiplier(4)->Range(16, 4096)->UseManualTime();

// =============================================================================
// Order Book Worst Case Benchmarks
// =============================================================================

// Adds at random prices and sides into a book of range(0) orders drawn the same way (#293)
static void BM_OrderBookWorstCaseAdd(benchmark::State &state)
{
    OrderBookBenchmarkSetup setup;
    std::mt19937 gen(42);
    std::uniform_real_distribution<> priceDist(90.0, 110.0);
    auto randomOrder = [&]()
    {
        double price = priceDist(gen);
        Side side = (gen() % 2 == 0) ? BUY_SIDE : SELL_SIDE;
        return setup.createOrder(side, price, 100);
    };

    const int64_t depth = state.range(0);
    for (int64_t i = 0; i < depth; ++i)
    {
        setup.orderBook_->add(*randomOrder());
    }
    std::vector<OrderEntry *> adds;
    for (size_t i = 0; i < addBatch(depth); ++i)
    {
        adds.push_back(randomOrder());
    }
    timeAdds(state, setup, adds);
}
BENCHMARK(BM_OrderBookWorstCaseAdd)->Range(8, 8 << 10)->UseManualTime();
