/**
 * Concurrent Order Processor library - FIX Gateway Benchmarks
 *
 * Measures throughput of FIX protocol components:
 *   - Enum conversion (FIX↔COP)
 *   - FIX message translation (NewOrderSingle → OrderEntry)
 *   - ExecutionReport construction
 *   - MultiOutQueues fan-out overhead
 *   - FX Swap matching vs single-leg matching
 *   - End-to-end: FIX NOS → IncomingQueues → Processor → match → fill → OutQueues
 */

#ifdef BUILD_FIX

// QuickFIX headers MUST come before any <flat_map> include (see FixGateway.h)
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include "FixGateway.h"
#include "MultiOutQueues.h"

#include <benchmark/benchmark.h>
#include <oneapi/tbb/info.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "DataModelDef.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "TransactionMgr.h"
#include "Processor.h"
#include "TaskManager.h"
#include "OrderMatcher.h"
#include "DeferedEvents.h"
#include "Logger.h"
#include "SubscrManager.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::Proc;
using namespace COP::Tasks;
using namespace COP::App;
using test::DummyOrderSaver;

namespace
{

const FIX::SessionID BENCH_SID("FIX.4.4", "BENCH_CLIENT", "ORDER_PROCESSOR", "");

// =============================================================================
// Null OutQueues — discards events (minimal overhead for isolation)
// =============================================================================

class NullOutQueues : public Queues::OutQueues
{
public:
    void push(const ExecReportEvent &, const std::string &) override
    {
        count_++;
    }
    void push(const CancelRejectEvent &, const std::string &) override {}
    void push(const BusinessRejectEvent &, const std::string &) override {}
    int count_ = 0;
};

// =============================================================================
// Counting OutQueues — counts the reports the pool's workers push
// =============================================================================

/// Atomic, since the benchmark's thread reads the counts while the workers write them. NullOutQueues keeps its plain
/// count, whose increment the fan-out benchmarks time (#309).
class CountingOutQueues final : public Queues::OutQueues
{
public:
    void push(const ExecReportEvent &evnt, const std::string &) override
    {
        reports_.fetch_add(1, std::memory_order_relaxed);
        if (TRADE_EXECTYPE == evnt.exec_->type_)
        {
            trades_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void push(const CancelRejectEvent &, const std::string &) override {}
    void push(const BusinessRejectEvent &, const std::string &) override {}

    u64 reports() const
    {
        return reports_.load(std::memory_order_relaxed);
    }
    /// Two a trade: one to each side
    u64 trades() const
    {
        return trades_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<u64> reports_{ 0 };
    std::atomic<u64> trades_{ 0 };
};

// =============================================================================
// Setup for benchmarks needing singletons
// =============================================================================

class FixBenchSetup
{
public:
    FixBenchSetup()
    {
        aux::ExchLogger::create();
        aux::ExchLogger::instance()->setNoteOn(false);  // measure the gateway, not spdlog (#121)
        aux::ExchLogger::instance()->setDebugOn(false); // measure the gateway, not debug logging (#76)
        WideDataStorage::create();
        SubscrMgr::SubscriptionMgr::create();
        IdTGenerator::create();
        OrderStorage::create();

        auto *instr = new InstrumentEntry();
        instr->symbol_ = "BENCHFX";
        instr->securityId_ = "SEC1";
        instr->securityIdSource_ = "SRC1";
        instr->instrumentType_ = EQUITY_INSTRUMENTTYPE;
        instrId_ = WideDataStorage::instance()->add(instr);

        auto *acct = new AccountEntry();
        acct->account_ = "BENCHACCT";
        acct->firm_ = "BENCHFIRM";
        acct->type_ = PRINCIPAL_ACCOUNTTYPE;
        WideDataStorage::instance()->add(acct);

        auto *clr = new ClearingEntry();
        clr->firm_ = "BENCHCLR";
        clearingId_ = WideDataStorage::instance()->add(clr);
    }

    ~FixBenchSetup()
    {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        SubscrMgr::SubscriptionMgr::destroy();
        WideDataStorage::destroy();
        aux::ExchLogger::destroy();
    }
    FixBenchSetup(const FixBenchSetup &) = delete;
    FixBenchSetup &operator=(const FixBenchSetup &) = delete;

    SourceIdT instrId_;
    SourceIdT clearingId_;
};

FIX44::NewOrderSingle makeBenchNOS(const std::string &clOrdId, char side, char ordType, double price, double qty)
{
    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX44::NewOrderSingle msg;
    msg.set(FIX::ClOrdID(clOrdId));
    msg.set(FIX::Side(side));
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(ordType));
    msg.set(FIX::Symbol("BENCHFX"));
    msg.set(FIX::OrderQty(qty));
    msg.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
    msg.set(FIX::Account("BENCHACCT"));
    msg.set(FIX::Currency("USD"));
    if (ordType != FIX::OrdType_MARKET)
    {
        msg.set(FIX::Price(price));
    }
    return msg;
}

/// Polls until the queue has counted `due` reports and the pool has finished, for a minute at most; whether it got
/// there. The blocking form of waitUntilTransactionsFinished() can't time a cycle: it sleeps a second between its two
/// checks (#309). The count is read first, since asking the pool takes the locks its workers need.
bool waitForPool(const TaskManager &taskMgr, const CountingOutQueues &outQueues, u64 due)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while ((outQueues.reports() < due) || !taskMgr.waitUntilTransactionsFinished(0))
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // anonymous namespace

// =============================================================================
// 1. Enum Conversion Benchmarks
// =============================================================================

static void BM_FixEnumToSide(benchmark::State &state)
{
    const char sides[] = { FIX::Side_BUY, FIX::Side_SELL, FIX::Side_SELL_SHORT };
    int i = 0;
    for (auto _ : state)
    {
        Side s = FixGateway::toSide(sides[i % 3]);
        benchmark::DoNotOptimize(s);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixEnumToSide);

static void BM_FixEnumFromOrdStatus(benchmark::State &state)
{
    const OrderStatus statuses[] = { NEW_ORDSTATUS, PARTFILL_ORDSTATUS, FILLED_ORDSTATUS, CANCELED_ORDSTATUS,
                                     REJECTED_ORDSTATUS };
    int i = 0;
    for (auto _ : state)
    {
        char c = FixGateway::fromOrdStatus(statuses[i % 5]);
        benchmark::DoNotOptimize(c);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixEnumFromOrdStatus);

static void BM_FixEnumToCurrency(benchmark::State &state)
{
    const std::string ccys[] = { "USD", "EUR", "GBP", "JPY", "CHF", "AUD", "CAD", "NZD" };
    int i = 0;
    for (auto _ : state)
    {
        Currency c = FixGateway::toCurrency(ccys[i % 8]);
        benchmark::DoNotOptimize(c);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixEnumToCurrency);

// =============================================================================
// 2. FIX Message Translation Benchmark
// =============================================================================

static void BM_FixNewOrderSingleTranslation(benchmark::State &state)
{
    FixBenchSetup setup;
    IncomingQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);

    int ordNum = 0;
    for (auto _ : state)
    {
        std::string clOrdId = "BENCH-" + std::to_string(++ordNum);
        auto msg = makeBenchNOS(clOrdId, FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);
        gateway.onMessage(msg, BENCH_SID);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixNewOrderSingleTranslation);

// =============================================================================
// 3. ExecutionReport Construction Benchmark
// =============================================================================

static void BM_FixExecReportConstruction(benchmark::State &state)
{
    int i = 0;
    for (auto _ : state)
    {
        FIX44::ExecutionReport report;
        report.set(FIX::OrderID(std::to_string(++i)));
        report.set(FIX::ExecID(std::to_string(i + 100000)));
        report.set(FIX::ExecType(FIX::ExecType_TRADE));
        report.set(FIX::OrdStatus(FIX::OrdStatus_FILLED));
        report.set(FIX::Side(FIX::Side_BUY));
        report.set(FIX::LeavesQty(0));
        report.set(FIX::CumQty(100));
        report.set(FIX::AvgPx(1.2650));
        report.set(FIX::Symbol("BENCHFX"));
        report.set(FIX::ClOrdID("CLORD-001"));
        report.set(FIX::LastQty(100));
        report.set(FIX::LastPx(1.2650));
        benchmark::DoNotOptimize(report);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixExecReportConstruction);

// =============================================================================
// 4. MultiOutQueues Fan-Out Overhead
// =============================================================================

// The engine pushes through an OutQueues pointer, a virtual call, whatever queue is behind it, so both push that way,
// through a pointer the compiler can't see through, to a target made once. A push into a local NullOutQueues was
// inlined into nothing, and timed 0 ns (#310).
static void BM_MultiOutQueuesFanOut(benchmark::State &state)
{
    NullOutQueues out1, out2;
    MultiOutQueues multi;
    multi.addDelegate(&out1);
    multi.addDelegate(&out2);
    Queues::OutQueues *out = &multi;
    benchmark::DoNotOptimize(out);

    ExecutionEntry exec;
    ExecReportEvent evt(&exec);
    const std::string target = "target";

    for (auto _ : state)
    {
        out->push(evt, target);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MultiOutQueuesFanOut);

static void BM_DirectOutQueuesPush(benchmark::State &state)
{
    NullOutQueues queue;
    Queues::OutQueues *out = &queue;
    benchmark::DoNotOptimize(out);

    ExecutionEntry exec;
    ExecReportEvent evt(&exec);
    const std::string target = "target";

    for (auto _ : state)
    {
        out->push(evt, target);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DirectOutQueuesPush);

// =============================================================================
// 5. FX Swap Matching vs Single-Leg Matching
// =============================================================================
//
// Each makes its resting sell and its aggressor buy once, and times one match() an iteration (#306). match() finds the
// first sell that trades with the buy and defers the trade, which the benchmark's container deletes, so neither the
// book nor the two orders change: every iteration matches the same pair. A deeper book wouldn't change that, since the
// search ends at the first sell, which trades.

static void BM_SingleLegMatch(benchmark::State &state)
{
    FixBenchSetup setup;

    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(setup.instrId_);
    OrderBookImpl orderBook;
    orderBook.init(instruments);

    IncomingQueues inQueues;
    NullOutQueues outQueues;

    class BenchDeferedContainer : public DeferedEventContainer
    {
    public:
        void addDeferedEvent(DeferedEventBase *evnt) override
        {
            delete evnt;
            count_++;
        }
        size_t deferedEventCount() const override
        {
            return count_;
        }
        void removeDeferedEventsFrom(size_t) override {}
        size_t count_ = 0;
    };
    BenchDeferedContainer defered;

    OrderMatcher matcher;
    matcher.init(&defered);

    Context ctxt(OrderStorage::instance(), &orderBook, &inQueues, &outQueues, &matcher, IdTGenerator::instance());

    // The resting sell, in the book
    SourceIdT srcId, destId, clOrdId, origClOrdId, accountId, clearingId, execList;
    const std::string clOrdStr = "SELL-1";
    auto *raw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size()));
    clOrdId = WideDataStorage::instance()->add(raw);

    auto sell = std::make_unique<OrderEntry>(srcId, destId, clOrdId, origClOrdId, setup.instrId_, accountId, clearingId,
                                             execList);
    sell->side_ = SELL_SIDE;
    sell->ordType_ = LIMIT_ORDERTYPE;
    sell->price_ = 1.2650;
    sell->orderQty_ = 100;
    sell->leavesQty_ = 100;
    sell->status_ = NEW_ORDSTATUS;
    OrderEntry *savedSell = OrderStorage::instance()->save(*sell, IdTGenerator::instance());
    sell.reset(); // save() stored a clone
    orderBook.add(*savedSell);

    // The aggressor buy
    const std::string buyClOrdStr = "BUY-1";
    auto *buyRaw = new RawDataEntry(STRING_RAWDATATYPE, buyClOrdStr.c_str(), static_cast<u32>(buyClOrdStr.size()));
    SourceIdT buyClOrdId = WideDataStorage::instance()->add(buyRaw);

    auto buy = std::make_unique<OrderEntry>(srcId, destId, buyClOrdId, origClOrdId, setup.instrId_, accountId,
                                            clearingId, execList);
    buy->side_ = BUY_SIDE;
    buy->ordType_ = LIMIT_ORDERTYPE;
    buy->price_ = 1.2650;
    buy->orderQty_ = 100;
    buy->leavesQty_ = 100;
    buy->status_ = NEW_ORDSTATUS;
    OrderEntry *savedBuy = OrderStorage::instance()->save(*buy, IdTGenerator::instance());
    buy.reset(); // save() stored a clone

    for (auto _ : state)
    {
        matcher.match(savedBuy, ctxt);
        benchmark::DoNotOptimize(defered.count_);
    }
    // Each match defers its one trade: a run that timed anything else fails
    if (defered.count_ != static_cast<size_t>(state.iterations()))
    {
        state.SkipWithError("a match didn't defer exactly one trade");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleLegMatch);

static void BM_FxSwapMatch(benchmark::State &state)
{
    FixBenchSetup setup;

    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(setup.instrId_);
    OrderBookImpl orderBook;
    orderBook.init(instruments);

    IncomingQueues inQueues;
    NullOutQueues outQueues;

    class BenchDeferedContainer : public DeferedEventContainer
    {
    public:
        void addDeferedEvent(DeferedEventBase *evnt) override
        {
            delete evnt;
            count_++;
        }
        size_t deferedEventCount() const override
        {
            return count_;
        }
        void removeDeferedEventsFrom(size_t) override {}
        size_t count_ = 0;
    };
    BenchDeferedContainer defered;

    OrderMatcher matcher;
    matcher.init(&defered);

    Context ctxt(OrderStorage::instance(), &orderBook, &inQueues, &outQueues, &matcher, IdTGenerator::instance());

    SourceIdT srcId, destId, origClOrdId, accountId, clearingId, execList;

    // The resting sell swap, in the book
    const std::string sellStr = "SWSELL-1";
    auto *sellRaw = new RawDataEntry(STRING_RAWDATATYPE, sellStr.c_str(), static_cast<u32>(sellStr.size()));
    SourceIdT sellClOrdId = WideDataStorage::instance()->add(sellRaw);

    auto sell = std::make_unique<OrderEntry>(srcId, destId, sellClOrdId, origClOrdId, setup.instrId_, accountId,
                                             clearingId, execList);
    sell->side_ = SELL_SIDE;
    sell->ordType_ = FXSWAP_ORDERTYPE;
    sell->price_ = 1.2650;
    sell->farPrice_ = 1.2680;
    sell->settlDate_ = 1000;
    sell->farSettlDate_ = 2000;
    sell->orderQty_ = 100;
    sell->leavesQty_ = 100;
    sell->status_ = NEW_ORDSTATUS;
    OrderEntry *savedSell = OrderStorage::instance()->save(*sell, IdTGenerator::instance());
    sell.reset(); // save() stored a clone
    orderBook.add(*savedSell);

    // The aggressor buy swap
    const std::string buyStr = "SWBUY-1";
    auto *buyRaw = new RawDataEntry(STRING_RAWDATATYPE, buyStr.c_str(), static_cast<u32>(buyStr.size()));
    SourceIdT buyClOrdId = WideDataStorage::instance()->add(buyRaw);

    auto buy = std::make_unique<OrderEntry>(srcId, destId, buyClOrdId, origClOrdId, setup.instrId_, accountId,
                                            clearingId, execList);
    buy->side_ = BUY_SIDE;
    buy->ordType_ = FXSWAP_ORDERTYPE;
    buy->price_ = 1.2650;
    buy->farPrice_ = 1.2680;
    buy->settlDate_ = 1000;
    buy->farSettlDate_ = 2000;
    buy->orderQty_ = 100;
    buy->leavesQty_ = 100;
    buy->status_ = NEW_ORDSTATUS;
    OrderEntry *savedBuy = OrderStorage::instance()->save(*buy, IdTGenerator::instance());
    buy.reset(); // save() stored a clone

    for (auto _ : state)
    {
        matcher.match(savedBuy, ctxt);
        benchmark::DoNotOptimize(defered.count_);
    }
    // Each match defers its one swap execution: a run that timed anything else fails
    if (defered.count_ != static_cast<size_t>(state.iterations()))
    {
        state.SkipWithError("a match didn't defer exactly one swap execution");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FxSwapMatch);

// =============================================================================
// 6. End-to-End: FIX NOS → Processor → Match → Fill → OutQueues
// =============================================================================

/// A sell through the gateway, booked, then a buy that trades with it, each waited for until the pool has finished. The
/// pool's workers do the work, so a cycle is timed in real time: this thread's CPU time is the two translations and the
/// polls (#309).
static void BM_FixEndToEndFillCycle(benchmark::State &state)
{
    // As in BM_TaskManagerThroughput (#120): this thread only polls, so on one CPU the pool has no worker and nothing
    // drains. Say so now, not after a minute's deadline.
    if (oneapi::tbb::info::default_concurrency() < 2)
    {
        state.SkipWithError("needs at least 2 CPUs, so TaskManager has a worker; pin to a range such as 2-5");
        return;
    }

    FixBenchSetup setup;

    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(setup.instrId_);
    auto orderBook = std::make_unique<OrderBookImpl>();
    orderBook->init(instruments);

    auto inQueues = std::make_unique<IncomingQueues>();
    auto outQueues = std::make_unique<CountingOutQueues>();

    TransactionMgrParams transParams(IdTGenerator::instance());
    auto transMgr = std::make_unique<TransactionMgr>();
    transMgr->init(transParams);

    ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook.get(), inQueues.get(),
                           outQueues.get(), inQueues.get(), transMgr.get());

    TaskManagerParams tmparams;
    auto *evtProc = new Processor();
    evtProc->init(params);
    tmparams.evntProcessors_.push_back(evtProc);

    auto *trProc = new Processor();
    trProc->init(params);
    tmparams.transactProcessors_.push_back(trProc);

    tmparams.transactMgr_ = transMgr.get();
    tmparams.inQueues_ = inQueues.get();

    TaskManager::init(0);
    auto taskMgr = std::make_unique<TaskManager>(tmparams);

    auto gateway = std::make_unique<FixGateway>(inQueues.get(), WideDataStorage::instance(), OrderStorage::instance(),
                                                setup.clearingId_);

    u64 reportsDue = 0;
    int ordNum = 0;
    for (auto _ : state)
    {
        // Submit sell: its report says it rests
        std::string sellId = "E2E-S-" + std::to_string(++ordNum);
        auto sellMsg = makeBenchNOS(sellId, FIX::Side_SELL, FIX::OrdType_LIMIT, 1.2650, 100);
        gateway->onMessage(sellMsg, BENCH_SID);
        reportsDue += 1;
        if (!waitForPool(*taskMgr, *outQueues, reportsDue))
        {
            state.SkipWithError("the pool did not book the sell within 60s");
            break;
        }

        // Submit buy → triggers match + fill: its own report, and the trade's, one to each side
        std::string buyId = "E2E-B-" + std::to_string(ordNum);
        auto buyMsg = makeBenchNOS(buyId, FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);
        gateway->onMessage(buyMsg, BENCH_SID);
        reportsDue += 3;
        if (!waitForPool(*taskMgr, *outQueues, reportsDue))
        {
            state.SkipWithError("the pool did not fill the buy within 60s");
            break;
        }
    }
    // Every cycle is a trade: a run that timed anything else fails
    if (!state.skipped() && (outQueues->trades() != 2 * static_cast<u64>(state.iterations())))
    {
        state.SkipWithError(("the cycles made " + std::to_string(outQueues->trades()) + " trade reports, not " +
                             std::to_string(2 * state.iterations()))
                                .c_str());
    }
    state.SetItemsProcessed(state.iterations() * 2); // 2 orders per cycle

    // Nothing to wait for: each cycle ended with the pool finished, and a run stopped at a deadline has waited a minute
    transMgr->stop();
    inQueues->detach();
    transMgr->detach();
    taskMgr.reset();
    transMgr.reset();
    outQueues.reset();
    inQueues.reset();
    orderBook.reset();
    gateway.reset();
    TaskManager::destroy();
}
BENCHMARK(BM_FixEndToEndFillCycle)->UseRealTime()->Unit(benchmark::kMicrosecond);

#endif // BUILD_FIX
