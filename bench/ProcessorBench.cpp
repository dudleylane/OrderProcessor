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

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DataModelDef.h"
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
    order->creationTime_ = 100;
    order->lastUpdateTime_ = 115;
    order->expireTime_ = 175;
    order->settlDate_ = 225;
    return order;
}

/// Runs each transaction on the thread that built it, as soon as it is enqueued, so the per-order
/// benchmarks measure the processing path itself - state machine, operations, book, storage -
/// rather than TaskManager's scheduling, which BM_TaskManagerThroughput covers.
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
    for (u64 i = 0; i < state.max_iterations; ++i)
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
    for (u64 i = 0; i < state.max_iterations; ++i)
    {
        engine.submit(engine.newOrder(SELL_SIDE, 100.0, 100));
    }

    std::vector<OrderEntry *> aggressors;
    aggressors.reserve(state.max_iterations);
    for (u64 i = 0; i < state.max_iterations; ++i)
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
    for (u64 i = 0; i < state.max_iterations; ++i)
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
            }
            state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
        }
        inQueues->detach();
        transactMgr->detach();
        TaskManager::destroy();
    }
    destroyEngineState();
}
BENCHMARK(BM_TaskManagerThroughput)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Iterations(20);
