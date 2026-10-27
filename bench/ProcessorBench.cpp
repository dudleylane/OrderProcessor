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
#include <benchmark/benchmark.h>
#include <oneapi/tbb/info.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DataModelDef.h"
#include "ExchUtils.h"
#include "IdTGenerator.h"
#include "IncomingQueues.h"
#include "Logger.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "OutgoingQueues.h"
#include "Processor.h"
#include "QueuesDef.h"
#include "TaskManager.h"
#include "TransactionDef.h"
#include "TransactionMgr.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::ACID;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::Store;
using namespace COP::Tasks;

namespace
{
/// The shared entries every order refers to. Held by id, the way the engine stores them.
struct WideData
{
    SourceIdT instrument_;
    SourceIdT source_;
    SourceIdT destination_;
    SourceIdT account_;
    SourceIdT clearing_;
};

/// Creates the singletons and the reference data one instrument's orders need. Notes are turned
/// off: they are on by default and app/main.cpp keeps them on, but every order writes several lines
/// to exchange.log, and with them on these benchmarks measure spdlog's file I/O rather than the
/// engine (about 24 us per order on the dev box, against 3 us of engine work). Logging cost is real
/// in production; it is simply not what these numbers are for.
WideData createEngineState()
{
    aux::ExchLogger::create();
    aux::ExchLogger::instance()->setNoteOn(false);
    aux::ExchLogger::instance()->setWarnOn(false);
    aux::ExchLogger::instance()->setDebugOn(false); // as the server does by default (#76)
    WideDataStorage::create();
    IdTGenerator::create();
    OrderStorage::create();

    WideData data;
    auto *instrument = new InstrumentEntry();
    instrument->symbol_ = "BENCH";
    instrument->securityId_ = "BENCHSEC";
    instrument->securityIdSource_ = "ISIN";
    data.instrument_ = WideDataStorage::instance()->add(instrument);
    data.source_ = WideDataStorage::instance()->add(new StringT("BENCHCLIENT"));
    data.destination_ = WideDataStorage::instance()->add(new StringT("BENCHVENUE"));

    auto *account = new AccountEntry();
    account->type_ = PRINCIPAL_ACCOUNTTYPE;
    account->firm_ = "BENCHFIRM";
    account->account_ = "BENCHACCT";
    data.account_ = WideDataStorage::instance()->add(account);

    auto *clearing = new ClearingEntry();
    clearing->firm_ = "BENCHCLEARER";
    data.clearing_ = WideDataStorage::instance()->add(clearing);
    return data;
}

void destroyEngineState()
{
    OrderStorage::destroy();
    IdTGenerator::destroy();
    WideDataStorage::destroy();
    aux::ExchLogger::destroy();
}

/// One order, with a unique ClOrdID (storage rejects a duplicate), its own executions list, and
/// every field OrderEntry::isValid() checks - an order failing that is rejected, not processed.
OrderEntry *makeOrder(const WideData &data, u64 sequence, const Side &side, PriceT price, QuantityT qty)
{
    const std::string clOrdId = "BENCH-" + std::to_string(sequence);
    SourceIdT clOrdIdRef = WideDataStorage::instance()->add(
        new RawDataEntry(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size())));
    SourceIdT executions = WideDataStorage::instance()->add(new ExecutionsT());
    SourceIdT none;

    auto *order = new OrderEntry(data.source_, data.destination_, clOrdIdRef, none, data.instrument_, data.account_,
                                 data.clearing_, executions);
    order->status_ = RECEIVEDNEW_ORDSTATUS;
    order->side_ = side;
    order->ordType_ = LIMIT_ORDERTYPE;
    order->tif_ = DAY_TIF;
    order->settlType_ = _3_SETTLTYPE;
    order->capacity_ = PRINCIPAL_CAPACITY;
    order->currency_ = USD_CURRENCY;
    order->price_ = price;
    order->orderQty_ = qty;
    order->leavesQty_ = qty;
    // Created in the business day the engine's clock is in, as a gateway creates an order (#244): acceptance asks for
    // the day of an order's creation and a match for its clock's, and each thread keeps the last day it answered.
    // Orders from 1970 made every timed match look both days up in the zone database, which production never does.
    static const DateTimeT created = aux::currentDateTime();
    order->creationTime_ = created;
    order->lastUpdateTime_ = created + 15;
    order->expireTime_ = 175;
    order->settlDate_ = 225;
    return order;
}

/// Runs each transaction on the thread that built it, as soon as it is enqueued, so the per-order
/// benchmarks measure the processing path itself - state machine, operations, book, storage -
/// rather than TaskManager's scheduling, which BM_TaskManagerThroughput and BM_TaskManagerTrades cover.
class InlineTransactionManager final : public TransactionManager
{
public:
    InlineTransactionManager() : processor_(nullptr), nextId_(1) {}

    void attach(TransactionObserver *) override {}
    TransactionObserver *detach() override
    {
        return nullptr;
    }

    void addTransaction(std::unique_ptr<Transaction> &tr) override
    {
        tr->setTransactionId(TransactionId(nextId_++, 1));
        if (nullptr != processor_)
        {
            processor_->process(tr->transactionId(), tr.get());
        }
    }

    bool removeTransaction(const TransactionId &, Transaction *) override
    {
        return false;
    }
    bool getParentTransactions(const TransactionId &, TransactionIdsT *) const override
    {
        return false;
    }
    bool getRelatedTransactions(const TransactionId &, TransactionIdsT *) const override
    {
        return false;
    }
    TransactionIterator *iterator() override
    {
        return nullptr;
    }
    /// Each transaction runs as it is added, so none is ever left
    size_t pendingTransactions() const override
    {
        return 0;
    }

    Processor *processor_;

private:
    u64 nextId_;
};

/// A whole engine minus the worker pool: one Processor whose transactions execute inline. No
/// OrderSaver is attached, so these are in-memory costs; persistence adds an LMDB write on top.
class EngineFixture
{
public:
    EngineFixture() : counter_(0)
    {
        data_ = createEngineState();

        orderBook_ = std::make_unique<OrderBookImpl>();
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(data_.instrument_);
        orderBook_->init(instruments);

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<OutgoingQueues>();
        transactMgr_ = std::make_unique<InlineTransactionManager>();

        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                               outQueues_.get(), inQueues_.get(), transactMgr_.get());
        processor_ = std::make_unique<Processor>();
        processor_->init(params);
        transactMgr_->processor_ = processor_.get();
    }

    ~EngineFixture()
    {
        processor_.reset();
        transactMgr_.reset();
        outQueues_.reset();
        inQueues_.reset();
        orderBook_.reset();
        destroyEngineState();
    }

    OrderEntry *newOrder(const Side &side, PriceT price, QuantityT qty)
    {
        return makeOrder(data_, ++counter_, side, price, qty);
    }

    IdT submit(OrderEntry *order)
    {
        inQueues_->push("bench", OrderEvent(order));
        processor_->process();
        OrderEntry *accepted = lastAccepted();
        return (nullptr != accepted) ? accepted->orderId_ : IdT();
    }

    OrderEntry *lastAccepted() const
    {
        const std::string clOrdId = "BENCH-" + std::to_string(counter_);
        RawDataEntry key(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size()));
        return OrderStorage::instance()->locateByClOrderId(key);
    }

    /// Fails the benchmark rather than silently timing a reject: an order that never reaches
    /// NEW_ORDSTATUS would make this path look far cheaper than it is.
    bool acceptsOrders(benchmark::State &state)
    {
        submit(newOrder(BUY_SIDE, 1.0, 1));
        OrderEntry *order = lastAccepted();
        if (nullptr == order)
        {
            state.SkipWithError("engine did not accept the warm-up order");
            return false;
        }
        if (NEW_ORDSTATUS != order->status_)
        {
            state.SkipWithError(
                ("warm-up order status is " + std::to_string(static_cast<int>(order->status_))).c_str());
            return false;
        }
        return true;
    }

    /// Fails the benchmark unless the last order made ended as the timed path leaves its orders: one rejected, or left
    /// working, would mean the benchmark timed some other path
    bool lastEndedAs(benchmark::State &state, OrderStatus status, QuantityT cumQty)
    {
        OrderEntry *order = lastAccepted();
        if (nullptr == order)
        {
            state.SkipWithError("engine did not store the warm-up order");
            return false;
        }
        if ((status != order->status_) || (cumQty != order->cumQty_))
        {
            state.SkipWithError(("warm-up order status is " + std::to_string(static_cast<int>(order->status_)) +
                                 " with " + std::to_string(order->cumQty_) + " filled")
                                    .c_str());
            return false;
        }
        return true;
    }

    Processor &processor()
    {
        return *processor_;
    }
    IncomingQueues &inQueues()
    {
        return *inQueues_;
    }

private:
    WideData data_;
    u64 counter_;

    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<OutgoingQueues> outQueues_;
    std::unique_ptr<InlineTransactionManager> transactMgr_;
    std::unique_ptr<Processor> processor_;
};

/// Counts the trade reports the engine sends and keeps nothing, as production's queues keep nothing, so that a pool
/// benchmark can tell when its batch has traded. Transaction workers push at once.
class TradeCountingOutQueues final : public OutQueues
{
public:
    void push(const ExecReportEvent &evnt, const std::string &) override
    {
        if (TRADE_EXECTYPE == evnt.exec_->type_)
        {
            tradeReports_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void push(const CancelRejectEvent &, const std::string &) override {}
    void push(const BusinessRejectEvent &, const std::string &) override {}

    /// Two a trade: one to each side
    u64 tradeReports() const
    {
        return tradeReports_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<u64> tradeReports_{ 0 };
};

} // namespace

// =============================================================================
// Per-order cost through the real Processor
// =============================================================================

static void BM_ProcessNewOrder(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }

    // Built outside the timed region: the measurement is the processing, not order construction.
    std::vector<OrderEntry *> orders;
    orders.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        orders.push_back(engine.newOrder(BUY_SIDE, 100.0, 100));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(orders[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessNewOrder);

static void BM_ProcessNewOrderWithMatch(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }

    // One resting sell per iteration, so every timed order crosses: a trade, the deferred execution
    // events it raises, and the book removal that follows.
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        engine.submit(engine.newOrder(SELL_SIDE, 100.0, 100));
    }

    std::vector<OrderEntry *> aggressors;
    aggressors.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        aggressors.push_back(engine.newOrder(BUY_SIDE, 100.0, 100));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(aggressors[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessNewOrderWithMatch);

static void BM_ProcessCancelOrder(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }

    std::vector<IdT> resting;
    resting.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        resting.push_back(engine.submit(engine.newOrder(BUY_SIDE, 100.0, 100)));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderCancelEvent(resting[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessCancelOrder);

static void BM_ProcessIocRemainder(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: an IOC that trades part of itself, and the engine cancels the rest
    engine.submit(engine.newOrder(SELL_SIDE, 50.0, 40));
    OrderEntry *warmUp = engine.newOrder(BUY_SIDE, 50.0, 100);
    warmUp->tif_ = IOC_TIF;
    engine.submit(warmUp);
    if (!engine.lastEndedAs(state, CANCELED_ORDSTATUS, 40))
    {
        return;
    }

    // One resting sell for 40 per iteration, each a point above the last. Each IOC buys 100 at its own sell's price, so
    // it trades with that sell alone, the cheapest left (the fixture's first order is a buy), and the engine cancels
    // the 60 it couldn't trade at once (#426): a trade, its deferred execution events, and the cancel's.
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        engine.submit(engine.newOrder(SELL_SIDE, 100.0 + static_cast<PriceT>(i), 40));
    }

    std::vector<OrderEntry *> aggressors;
    aggressors.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        OrderEntry *ioc = engine.newOrder(BUY_SIDE, 100.0 + static_cast<PriceT>(i), 100);
        ioc->tif_ = IOC_TIF;
        aggressors.push_back(ioc);
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(aggressors[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessIocRemainder);

static void BM_ProcessExpiry(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: the warm-up order, a resting DAY order, expired at its day's end
    engine.inQueues().push("bench", TimerEvent(engine.lastAccepted()->orderId_, TimerEvent::DAY_END));
    engine.processor().process();
    if (!engine.lastEndedAs(state, EXPIRED_ORDSTATUS, 0))
    {
        return;
    }

    // One resting DAY order per iteration, each expired by the DAY_END timer ExpiryScheduler sets a DAY order (#426).
    // The event enqueues a transaction, which runs inline: it decides the expiry, takes the order off the book and
    // reports it.
    std::vector<IdT> resting;
    resting.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        resting.push_back(engine.submit(engine.newOrder(BUY_SIDE, 100.0, 100)));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", TimerEvent(resting[next++], TimerEvent::DAY_END));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessExpiry);

static void BM_ProcessInvalidOrder(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: an order OrderEntry::isValid() refuses, which the engine stores as it rejects it
    engine.submit(engine.newOrder(BUY_SIDE, 0.0, 100));
    if (!engine.lastEndedAs(state, REJECTED_ORDSTATUS, 0))
    {
        return;
    }

    // One limit buy at 0 per iteration, refused for its price (#188), which isValid() checks after everything else a
    // limit order needs, so that every other check runs first: processReceive() throws before it stores the order, and
    // the reject stores it (processReject(onRecvOrderRejected)) and reports it (#435)
    std::vector<OrderEntry *> orders;
    orders.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        orders.push_back(engine.newOrder(BUY_SIDE, 0.0, 100));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(orders[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessInvalidOrder);

static void BM_ProcessMarketOrderWithNoMarket(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: a market buy with nothing to buy, which the engine stores, then rejects
    OrderEntry *warmUp = engine.newOrder(BUY_SIDE, 0.0, 100);
    warmUp->ordType_ = MARKET_ORDERTYPE;
    engine.submit(warmUp);
    if (!engine.lastEndedAs(state, REJECTED_ORDSTATUS, 0))
    {
        return;
    }

    // One market buy per iteration, with no sell in the book (the fixture's first order is a buy): processReceive()
    // stores it and finds no market, and the reject keeps the stored order (processReject(onRecvOrderRejected) returns
    // at alreadyStored()) and reports it (#435)
    std::vector<OrderEntry *> orders;
    orders.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        OrderEntry *order = engine.newOrder(BUY_SIDE, 0.0, 100);
        order->ordType_ = MARKET_ORDERTYPE;
        orders.push_back(order);
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(orders[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessMarketOrderWithNoMarket);

// =============================================================================
// FX swaps (this branch only, #162)
// =============================================================================

namespace
{
/// A swap order: the order makeOrder makes, with a far leg. Two swaps match only when their near and far dates agree, so
/// every swap here has the same ones.
OrderEntry *newSwap(EngineFixture &engine, const Side &side, PriceT price, QuantityT qty)
{
    OrderEntry *order = engine.newOrder(side, price, qty);
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->farPrice_ = 100.5;
    order->farSettlDate_ = 325; // after makeOrder's settlDate_
    return order;
}
} // namespace

// BM_ProcessNewOrderWithMatch for swaps: OrderMatcher's swap matching, and the swap's deferred events, which report and
// book a near leg and a far leg for each side
static void BM_ProcessSwapWithMatch(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    engine.submit(newSwap(engine, BUY_SIDE, 1.0, 1)); // priced away from the book, as acceptsOrders' order is
    const OrderEntry *warmUp = engine.lastAccepted();
    if ((nullptr == warmUp) || (NEW_ORDSTATUS != warmUp->status_))
    {
        state.SkipWithError("engine did not accept the warm-up swap");
        return;
    }

    // One resting sell swap per iteration, so every timed swap crosses
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        engine.submit(newSwap(engine, SELL_SIDE, 100.0, 100));
    }

    std::vector<OrderEntry *> aggressors;
    aggressors.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        aggressors.push_back(newSwap(engine, BUY_SIDE, 100.0, 100));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(aggressors[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());

    // The last swap timed crossed a resting one, so it filled; otherwise this timed something cheaper than a match
    const OrderEntry *last = engine.lastAccepted();
    if ((nullptr == last) || (FILLED_ORDSTATUS != last->status_))
    {
        state.SkipWithError("the timed swaps didn't fill");
    }
}
BENCHMARK(BM_ProcessSwapWithMatch);

// =============================================================================
// End-to-end throughput through TaskManager's worker pool
// =============================================================================

/// Pushes a batch of new orders into the queues and waits for the pool to finish them. The argument
/// is the number of event and transaction processors, so the numbers show how throughput scales.
///
/// Completion is polled with waitUntilTransactionsFinished(0), which returns the condition without
/// blocking. The blocking form cannot be used for timing: it insists on seeing "finished" twice with
/// a one-second sleep in between, so it has a two-second floor whatever the work costs.
///
/// The argument starts at 2 because TaskManager::init(n) caps TBB parallelism at n, and this thread
/// is polling rather than running tasks, so a single-worker configuration never drains.
static void BM_TaskManagerThroughput(benchmark::State &state)
{
    // oneTBB sizes its pool from the CPUs the process may use. On one CPU it has no worker, and this
    // thread only polls, so no batch would ever drain (#120). Say so now, not after every deadline.
    if (oneapi::tbb::info::default_concurrency() < 2)
    {
        state.SkipWithError("needs at least 2 CPUs, so TaskManager has a worker; pin to a range such as 2-5");
        return;
    }

    const int processors = static_cast<int>(state.range(0));
    const u64 batch = 500;

    WideData data = createEngineState();
    {
        auto orderBook = std::make_unique<OrderBookImpl>();
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(data.instrument_);
        orderBook->init(instruments);

        auto inQueues = std::make_unique<IncomingQueues>();
        auto outQueues = std::make_unique<OutgoingQueues>();

        TransactionMgrParams transParams(IdTGenerator::instance());
        auto transactMgr = std::make_unique<TransactionMgr>();
        transactMgr->init(transParams);

        ProcessorParams procParams(IdTGenerator::instance(), OrderStorage::instance(), orderBook.get(), inQueues.get(),
                                   outQueues.get(), inQueues.get(), transactMgr.get());

        TaskManagerParams params;
        params.transactMgr_ = transactMgr.get();
        params.inQueues_ = inQueues.get();
        // TaskManager owns and deletes these, and event and transaction processors must be distinct.
        for (int i = 0; i < processors; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(procParams);
            params.evntProcessors_.push_back(proc.release());
        }
        for (int i = 0; i < processors; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(procParams);
            params.transactProcessors_.push_back(proc.release());
        }

        TaskManager::init(processors);
        {
            auto taskMgr = std::make_unique<TaskManager>(params);

            u64 sequence = 0;
            for (auto _ : state)
            {
                state.PauseTiming();
                std::vector<OrderEntry *> orders;
                orders.reserve(batch);
                for (u64 i = 0; i < batch; ++i)
                {
                    orders.push_back(makeOrder(data, ++sequence, BUY_SIDE, 100.0 + static_cast<PriceT>(i % 50), 100));
                }
                state.ResumeTiming();

                for (OrderEntry *order : orders)
                {
                    inQueues->push("bench", OrderEvent(order));
                }

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                while (!taskMgr->waitUntilTransactionsFinished(0))
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        state.SkipWithError("worker pool did not finish the batch within 60s");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                // SkipWithError doesn't end the loop: without this, each remaining iteration waits out
                // its own deadline, 20 minutes per case.
                if (state.skipped())
                {
                    break;
                }
            }
            state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
        }
        inQueues->detach();
        transactMgr->detach();
        // As app/main.cpp does once the TaskManager is gone (#37); ~TransactionMgr asserts it.
        transactMgr->stop();
        TaskManager::destroy();
    }
    destroyEngineState();
}
BENCHMARK(BM_TaskManagerThroughput)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Iterations(20);

/// The worker pool while orders trade (#290). BM_TaskManagerThroughput's orders all buy, so it times no match: no
/// deferred match or execution event, no fill, and no transaction that waits for another's. Here each batch alternates
/// sells and buys at one price and quantity, so every order trades in full, with whichever order of the other side
/// reached the book first, and the batch makes one trade a pair and leaves the book empty.
///
/// The first argument is the number of instruments the pairs are dealt over, in turn. In one, every booking, match
/// and fill relates to the same instrument; in several, the pool can run each instrument's transactions beside the
/// others'. The second is the number of event and transaction processors, as BM_TaskManagerThroughput's argument is,
/// and the pool is wired and polled as there. A batch is done when its trades have been reported and the pool has
/// finished. Reports orders per second, and trades per second as "trades".
static void BM_TaskManagerTrades(benchmark::State &state)
{
    if (oneapi::tbb::info::default_concurrency() < 2)
    {
        state.SkipWithError("needs at least 2 CPUs, so TaskManager has a worker; pin to a range such as 2-5");
        return;
    }

    const size_t instrumentCount = static_cast<size_t>(state.range(0));
    const int processors = static_cast<int>(state.range(1));
    const u64 batch = 500; // 250 pairs, a trade each

    WideData data = createEngineState();
    {
        // An instrument's orders differ from the first instrument's in their instrument alone
        std::vector<WideData> books(instrumentCount, data);
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(data.instrument_);
        for (size_t k = 1; k < instrumentCount; ++k)
        {
            auto *instrument = new InstrumentEntry();
            instrument->symbol_ = "BENCH" + std::to_string(k);
            instrument->securityId_ = "BENCHSEC" + std::to_string(k);
            instrument->securityIdSource_ = "ISIN";
            books[k].instrument_ = WideDataStorage::instance()->add(instrument);
            instruments.insert(books[k].instrument_);
        }
        auto orderBook = std::make_unique<OrderBookImpl>();
        orderBook->init(instruments);

        auto inQueues = std::make_unique<IncomingQueues>();
        auto outQueues = std::make_unique<TradeCountingOutQueues>();

        TransactionMgrParams transParams(IdTGenerator::instance());
        auto transactMgr = std::make_unique<TransactionMgr>();
        transactMgr->init(transParams);

        ProcessorParams procParams(IdTGenerator::instance(), OrderStorage::instance(), orderBook.get(), inQueues.get(),
                                   outQueues.get(), inQueues.get(), transactMgr.get());

        TaskManagerParams params;
        params.transactMgr_ = transactMgr.get();
        params.inQueues_ = inQueues.get();
        // TaskManager owns and deletes these, and event and transaction processors must be distinct.
        for (int i = 0; i < processors; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(procParams);
            params.evntProcessors_.push_back(proc.release());
        }
        for (int i = 0; i < processors; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(procParams);
            params.transactProcessors_.push_back(proc.release());
        }

        TaskManager::init(processors);
        {
            auto taskMgr = std::make_unique<TaskManager>(params);

            u64 sequence = 0;
            u64 reportsDue = 0; // the trade reports the batches so far make
            for (auto _ : state)
            {
                state.PauseTiming();
                std::vector<OrderEntry *> orders;
                orders.reserve(batch);
                for (u64 i = 0; i < batch; ++i)
                {
                    const WideData &book = books[(i / 2) % instrumentCount];
                    orders.push_back(makeOrder(book, ++sequence, (0 == i % 2) ? SELL_SIDE : BUY_SIDE, 100.0, 100));
                }
                reportsDue += batch; // a trade a pair, reported to both its sides
                state.ResumeTiming();

                for (OrderEntry *order : orders)
                {
                    inQueues->push("bench", OrderEvent(order));
                }

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                while ((outQueues->tradeReports() < reportsDue) || !taskMgr->waitUntilTransactionsFinished(0))
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        state.SkipWithError("worker pool did not finish the batch's trades within 60s");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                // As in BM_TaskManagerThroughput: otherwise each remaining iteration waits out its own deadline
                if (state.skipped())
                {
                    break;
                }
            }
            if (!state.skipped() && (outQueues->tradeReports() != reportsDue))
            {
                state.SkipWithError(("the batches made " + std::to_string(outQueues->tradeReports()) +
                                     " trade reports, not " + std::to_string(reportsDue))
                                        .c_str());
            }
            state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
            state.counters["trades"] =
                benchmark::Counter(static_cast<double>(state.iterations() * batch / 2), benchmark::Counter::kIsRate);
        }
        inQueues->detach();
        transactMgr->detach();
        // As app/main.cpp does once the TaskManager is gone (#37); ~TransactionMgr asserts it.
        transactMgr->stop();
        TaskManager::destroy();
    }
    destroyEngineState();
}
BENCHMARK(BM_TaskManagerTrades)
    ->ArgNames({ "instruments", "processors" })
    ->ArgsProduct({ { 1, 8 }, { 2, 4, 8 } })
    ->UseRealTime()
    ->Iterations(20);
