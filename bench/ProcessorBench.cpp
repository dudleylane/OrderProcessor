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
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DataModelDef.h"
#include "ExchUtils.h"
#include "FileStorageDef.h"
#include "IdTGenerator.h"
#include "IncomingQueues.h"
#include "Logger.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "Processor.h"
#include "QueuesDef.h"
#include "StorageRecordDispatcher.h"
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

/// Keeps nothing it is given: what a record costs before LMDB writes it, without the write
class DiscardingFileSaver final : public FileSaver
{
public:
    void load(const std::string &, FileStorageObserver *) override {}
    IdT save(const char *, size_t) override
    {
        return IdT();
    }
    void save(const IdT &, const char *, size_t) override {}
    u32 update(const IdT &, const char *, size_t size) override
    {
        bytes_ += size;
        return ++version_;
    }
    u32 replace(const IdT &, u32, const char *, size_t) override
    {
        return 0;
    }
    void erase(const IdT &, u32) override {}
    void erase(const IdT &) override {}

    /// How many records it was given, and their bytes
    u32 records() const
    {
        return version_;
    }
    u64 bytes() const
    {
        return bytes_;
    }

private:
    u32 version_ = 0;
    u64 bytes_ = 0;
};

/// Keeps no report it is given, as production's queues keep none: WsOutQueues serializes each one as it is pushed. The
/// benchmarks that time the engine report into it, so that none charges the engine for storing its reports (#447).
/// OutgoingQueues, which they reported into until then, keeps each one until it is popped, and its cost moved in steps
/// with a report's size: oneTBB's queue puts two items on a page up to 128 bytes, and one above, a line #411's figures
/// took a report past. It is timed on its own (BM_OutQueuePush*), and a report's consumers on theirs
/// (BM_WsOutQueuesPushTradeReport, and the FIX gateway's BM_FixSendExecutionReport).
class DiscardingOutQueues final : public OutQueues
{
public:
    void push(const ExecReportEvent &, const std::string &) override {}
    void push(const CancelRejectEvent &, const std::string &) override {}
    void push(const BusinessRejectEvent &, const std::string &) override {}
    /// Counts the orders refused unstored for a ClOrdID in use, which a warm-up checks (#432)
    void push(const OrderRejectEvent &evnt, const std::string &) override
    {
        duplicates_ += evnt.duplicateClOrderId_ ? 1 : 0;
    }

    u64 duplicateRejects() const
    {
        return duplicates_;
    }

private:
    u64 duplicates_ = 0;
};

/// A whole engine minus the worker pool: one Processor whose transactions execute inline, reporting into a queue that
/// keeps nothing (#447). No OrderSaver is attached unless recordOrders() asks for one, so these are in-memory costs;
/// persistence adds an LMDB write on top.
class EngineFixture
{
public:
    /// The engine's clock is the system's unless a benchmark gives one: a restatement reads it for the business day
    /// it begins (#244)
    explicit EngineFixture(std::function<DateTimeT()> clock = {}) : counter_(0)
    {
        data_ = createEngineState();

        orderBook_ = std::make_unique<OrderBookImpl>();
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(data_.instrument_);
        orderBook_->init(instruments);

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<DiscardingOutQueues>();
        transactMgr_ = std::make_unique<InlineTransactionManager>();

        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                               outQueues_.get(), inQueues_.get(), transactMgr_.get());
        params.clock_ = std::move(clock);
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

    /// A replacement of a stored order, made as the server's session makes one (#74): the order's clone, with a fresh
    /// id, a ClOrdID of its own, the original's as its OrigClOrdID, and a new price. Null for an order not stored.
    OrderEntry *replacementOf(const IdT &orderId, PriceT price)
    {
        const OrderEntry *original = OrderStorage::instance()->locateByOrderId(orderId);
        if (nullptr == original)
        {
            return nullptr;
        }
        const std::string clOrdId = "BENCH-" + std::to_string(++counter_);
        OrderEntry *replacement = original->clone();
        replacement->orderId_ = IdT();
        replacement->clOrderId_ = WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size())));
        replacement->origClOrderId_ = original->clOrderId_;
        replacement->price_ = price;
        replacement->status_ = RECEIVEDNEW_ORDSTATUS;
        return replacement;
    }

    /// A new order that reuses a stored order's ClOrdID, made as the server's session makes a new order: a fresh one,
    /// as newOrder() makes it, with the stored order's ClOrdID in a reference of its own. Null for an order not stored.
    OrderEntry *duplicateOf(const IdT &orderId)
    {
        const OrderEntry *stored = OrderStorage::instance()->locateByOrderId(orderId);
        if (nullptr == stored)
        {
            return nullptr;
        }
        const RawDataEntry &clOrdId = stored->clOrderId_.get();
        OrderEntry *order = newOrder(stored->side_, stored->price_, stored->orderQty_);
        order->clOrderId_ =
            WideDataStorage::instance()->add(new RawDataEntry(STRING_RAWDATATYPE, clOrdId.data_, clOrdId.length_));
        return order;
    }

    /// Has each order the engine persists encoded as the server has it, by the dispatcher it attaches, with the
    /// record's strings found in reference data, into a file storage that keeps nothing: the record without its write
    void recordOrders()
    {
        dispatcher_.init(WideDataStorage::instance(), orderBook_.get(), &discarded_, OrderStorage::instance());
        OrderStorage::instance()->attach(&dispatcher_);
    }

    const DiscardingFileSaver &discarded() const
    {
        return discarded_;
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

    /// Fails the benchmark unless the order was restated as the timed path restates its orders: a day later than
    /// restedIn, the one it rested in, begun, and a RESTATED report sent. A restatement of an order no longer working
    /// decides nothing, and one that finds its day begun only reports it.
    bool restated(benchmark::State &state, const IdT &orderId, DateTimeT restedIn)
    {
        const OrderEntry *order = OrderStorage::instance()->locateByOrderId(orderId);
        if (nullptr == order)
        {
            state.SkipWithError("engine did not store the warm-up order");
            return false;
        }
        const ExecutionsT *executions = order->executions_.get();
        const ExecutionEntry *last =
            executions->empty() ? nullptr : OrderStorage::instance()->locateByExecId(executions->back().eventId_);
        if ((nullptr == last) || (RESTATED_EXECTYPE != last->type_))
        {
            state.SkipWithError("the warm-up order's last report is not a RESTATED one");
            return false;
        }
        if (order->dayStart_ <= restedIn)
        {
            state.SkipWithError("the warm-up order's restatement did not begin its next day");
            return false;
        }
        return true;
    }

    /// Fails the benchmark unless the order was replaced as the timed path replaces its orders: the original REPLACED,
    /// and its replacement, the last order made, stored in its place at the new price, NEW with nothing filled. A
    /// refused replace leaves the original working and stores nothing, and a replacement that trades isn't NEW.
    bool replaced(benchmark::State &state, const IdT &originalId, PriceT price)
    {
        const OrderEntry *original = OrderStorage::instance()->locateByOrderId(originalId);
        if ((nullptr == original) || (REPLACED_ORDSTATUS != original->status_))
        {
            state.SkipWithError("the warm-up order was not replaced");
            return false;
        }
        const OrderEntry *replacement = lastAccepted();
        if ((nullptr == replacement) || (originalId != replacement->origOrderId_) || (price != replacement->price_))
        {
            state.SkipWithError("engine did not store the warm-up order's replacement at its new price");
            return false;
        }
        return lastEndedAs(state, NEW_ORDSTATUS, 0);
    }

    /// Fails the benchmark unless the engine refused as the timed path refuses: each order given it so far reported to
    /// its sender as refused unstored for a ClOrdID in use, and the stored order whose ClOrdID they reused untouched,
    /// NEW with all it had left. An order the engine took instead would be stored, NEW, with no such report.
    bool refusedAsDuplicates(benchmark::State &state, const IdT &storedId, u64 refusals)
    {
        if (refusals != outQueues_->duplicateRejects())
        {
            state.SkipWithError(("the engine refused " + std::to_string(outQueues_->duplicateRejects()) +
                                 " orders for a ClOrdID in use, not " + std::to_string(refusals))
                                    .c_str());
            return false;
        }
        const OrderEntry *stored = OrderStorage::instance()->locateByOrderId(storedId);
        if ((nullptr == stored) || (NEW_ORDSTATUS != stored->status_) || (stored->orderQty_ != stored->leavesQty_))
        {
            state.SkipWithError(
                "the order whose ClOrdID the warm-up order reused is no longer NEW with all it had left");
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

    DiscardingFileSaver discarded_;
    StorageRecordDispatcher dispatcher_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<DiscardingOutQueues> outQueues_;
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

// BM_ProcessNewOrderWithMatch with each persist's record encoded, as the dispatcher the server attaches encodes it for
// LMDB, which keeps nothing here: the booking's record and both of the trade's
static void BM_ProcessNewOrderWithMatchRecorded(benchmark::State &state)
{
    EngineFixture engine;
    engine.recordOrders();
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    if (0 == engine.discarded().records())
    {
        state.SkipWithError("the warm-up order's record wasn't encoded");
        return;
    }

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
BENCHMARK(BM_ProcessNewOrderWithMatchRecorded);

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

static void BM_ProcessReplaceOrder(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: the warm-up order, a buy at 1.0, replaced by a buy at 2.0, which crosses nothing
    const IdT warmUp = engine.lastAccepted()->orderId_;
    engine.inQueues().push("bench", OrderReplaceEvent(warmUp, engine.replacementOf(warmUp, 2.0)));
    engine.processor().process();
    if (!engine.replaced(state, warmUp, 2.0))
    {
        return;
    }

    // One resting buy per iteration, each replaced by a buy at a new price, which crosses nothing: the book holds no sell
    // (#457). The event enqueues a transaction, which runs inline: it decides the replace under the original's lock,
    // stores the replacement, takes the original off the book and books the replacement, persists both and reports
    // both. The replacements are made first, as a session makes them on the IO thread.
    std::vector<IdT> resting;
    resting.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        resting.push_back(engine.submit(engine.newOrder(BUY_SIDE, 100.0, 100)));
    }
    std::vector<OrderEntry *> replacements;
    replacements.reserve(state.max_iterations);
    for (const IdT &original : resting)
    {
        replacements.push_back(engine.replacementOf(original, 101.0));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderReplaceEvent(resting[next], replacements[next]));
        engine.processor().process();
        ++next;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessReplaceOrder);

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

static void BM_ProcessFokFill(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: a FOK the book fills at once
    engine.submit(engine.newOrder(SELL_SIDE, 50.0, 100));
    OrderEntry *warmUp = engine.newOrder(BUY_SIDE, 50.0, 100);
    warmUp->tif_ = FOK_TIF;
    engine.submit(warmUp);
    if (!engine.lastEndedAs(state, FILLED_ORDSTATUS, 100))
    {
        return;
    }

    // One resting sell per iteration, and a FOK buy for all of it at its price, so every timed order fills at once: the
    // check that the book can fill it (OrderMatcher::canFillAtOnce(), #240), which walks the sells until what they have
    // left is enough, then the trade, its deferred execution events, and the book removal that follows. The only
    // benchmark that reaches the check: nothing else sends a FOK order.
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        engine.submit(engine.newOrder(SELL_SIDE, 100.0, 100));
    }

    std::vector<OrderEntry *> aggressors;
    aggressors.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        OrderEntry *fok = engine.newOrder(BUY_SIDE, 100.0, 100);
        fok->tif_ = FOK_TIF;
        aggressors.push_back(fok);
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(aggressors[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessFokFill);

static void BM_ProcessSweep(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: a buy for 100 that takes four sells of 25, so only a sweep of all four fills it
    for (int i = 0; i < 4; ++i)
    {
        engine.submit(engine.newOrder(SELL_SIDE, 50.0, 25));
    }
    engine.submit(engine.newOrder(BUY_SIDE, 50.0, 100));
    if (!engine.lastEndedAs(state, FILLED_ORDSTATUS, 100))
    {
        return;
    }

    // Four resting sells of 25 per iteration, all at one price, and a buy for 100 at it, so each timed buy sweeps the
    // four oldest: four trades, each with its deferred execution events and its sell's book removal, and between them
    // the three matches that continue the sweep (#477). The only benchmark in which a sweep's next match finds another
    // order to trade with: every other timed order trades once, and an IOC's next match finds nothing.
    for (u64 i = 0; i < 4 * static_cast<u64>(state.max_iterations); ++i)
    {
        engine.submit(engine.newOrder(SELL_SIDE, 100.0, 25));
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
BENCHMARK(BM_ProcessSweep);

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

static void BM_ProcessRestatement(benchmark::State &state)
{
    // The engine's clock, moved on a day once the orders rest
    DateTimeT now = aux::currentDateTime();
    EngineFixture engine(
        [&now]
        {
            return now;
        });
    if (!engine.acceptsOrders(state))
    {
        return;
    }

    // One resting GTC order per iteration, and the warm-up's, each restated by the RESTATEMENT timer ExpiryScheduler
    // sets an order that rests past its business day (#244, #451). The event enqueues a transaction, which runs
    // inline: it begins the order's next day, persists the order and reports it RESTATED.
    std::vector<IdT> resting;
    resting.reserve(state.max_iterations + 1);
    for (u64 i = 0; i <= static_cast<u64>(state.max_iterations); ++i)
    {
        OrderEntry *order = engine.newOrder(BUY_SIDE, 100.0, 100);
        order->tif_ = GTC_TIF;
        resting.push_back(engine.submit(order));
    }
    // A day on, as at 17:00 New York: each restatement begins its order's next day. In the day the orders were
    // accepted in, a restatement would find the day begun and only report it.
    now += 24 * 60 * 60 * 1000;

    // Where the timed path ends: the warm-up order restated
    const OrderEntry *warmUp = OrderStorage::instance()->locateByOrderId(resting[0]);
    const DateTimeT restedIn = (nullptr != warmUp) ? warmUp->dayStart_ : 0;
    engine.inQueues().push("bench", TimerEvent(resting[0], TimerEvent::RESTATEMENT));
    engine.processor().process();
    if (!engine.restated(state, resting[0], restedIn))
    {
        return;
    }

    size_t next = 1;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", TimerEvent(resting[next++], TimerEvent::RESTATEMENT));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessRestatement);

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
    // limit order needs, so that every other check runs first: refused() refuses it before it is stored, and the
    // reject's row stores it (processReject(onOrderReceived)) and reports it, with no exception raised (#435, #436)
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
    // Where the timed path ends: a market buy with nothing to buy, which the engine rejects, storing it
    OrderEntry *warmUp = engine.newOrder(BUY_SIDE, 0.0, 100);
    warmUp->ordType_ = MARKET_ORDERTYPE;
    engine.submit(warmUp);
    if (!engine.lastEndedAs(state, REJECTED_ORDSTATUS, 0))
    {
        return;
    }

    // One market buy per iteration, with no sell in the book (the fixture's first order is a buy): refused() finds no
    // market before it is stored, and the reject's row stores it (processReject(onOrderReceived)) and reports it, with
    // no exception raised (#435, #436)
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

static void BM_ProcessDuplicateClOrdId(benchmark::State &state)
{
    EngineFixture engine;
    if (!engine.acceptsOrders(state))
    {
        return;
    }
    // Where the timed path ends: a new order with the ClOrdID of the fixture's first order, which the store refuses and
    // the engine rejects unstored, telling its sender directly (#67)
    const IdT stored = engine.lastAccepted()->orderId_;
    engine.inQueues().push("bench", OrderEvent(engine.duplicateOf(stored)));
    engine.processor().process();
    if (!engine.refusedAsDuplicates(state, stored, 1))
    {
        return;
    }

    // One such order per iteration, made as the fixture's first order was made, so that refused()'s checks pass it:
    // only the store refuses it. receive() queues the reject and throws to stop the transition to New, the reject's own
    // store refuses it again, and Processor reports it to its sender, with the transaction's scope unused: three
    // exceptions raised, where since #436 every other refusal of a new order raises none (#432)
    std::vector<OrderEntry *> orders;
    orders.reserve(state.max_iterations);
    for (u64 i = 0; i < static_cast<u64>(state.max_iterations); ++i)
    {
        orders.push_back(engine.duplicateOf(stored));
    }

    size_t next = 0;
    for (auto _ : state)
    {
        engine.inQueues().push("bench", OrderEvent(orders[next++]));
        engine.processor().process();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessDuplicateClOrdId);

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
        auto outQueues = std::make_unique<DiscardingOutQueues>(); // as the per-order benchmarks' (#447)

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
