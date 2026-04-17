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
#include <memory>

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

namespace {

const FIX::SessionID BENCH_SID("FIX.4.4", "BENCH_CLIENT", "ORDER_PROCESSOR", "");

// =============================================================================
// Null OutQueues — discards events (minimal overhead for isolation)
// =============================================================================

class NullOutQueues : public Queues::OutQueues {
public:
    void push(const ExecReportEvent&, const std::string&) override { count_++; }
    void push(const CancelRejectEvent&, const std::string&) override {}
    void push(const BusinessRejectEvent&, const std::string&) override {}
    int count_ = 0;
};

// =============================================================================
// Setup for benchmarks needing singletons
// =============================================================================

class FixBenchSetup {
public:
    FixBenchSetup() {
        if (!aux::ExchLogger::instance()) aux::ExchLogger::create();
        WideDataStorage::create();
        SubscrMgr::SubscriptionMgr::create();
        IdTGenerator::create();
        OrderStorage::create();

        auto* instr = new InstrumentEntry();
        instr->symbol_ = "BENCHFX";
        instr->securityId_ = "SEC1";
        instr->securityIdSource_ = "SRC1";
        instr->instrumentType_ = EQUITY_INSTRUMENTTYPE;
        instrId_ = WideDataStorage::instance()->add(instr);

        auto* acct = new AccountEntry();
        acct->account_ = "BENCHACCT";
        acct->firm_ = "BENCHFIRM";
        acct->type_ = PRINCIPAL_ACCOUNTTYPE;
        WideDataStorage::instance()->add(acct);

        auto* clr = new ClearingEntry();
        clr->firm_ = "BENCHCLR";
        clearingId_ = WideDataStorage::instance()->add(clr);
    }

    ~FixBenchSetup() {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        SubscrMgr::SubscriptionMgr::destroy();
        WideDataStorage::destroy();
    }
    FixBenchSetup(const FixBenchSetup&) = delete;
    FixBenchSetup& operator=(const FixBenchSetup&) = delete;

    SourceIdT instrId_;
    SourceIdT clearingId_;
};

FIX44::NewOrderSingle makeBenchNOS(const std::string& clOrdId, char side,
                                    char ordType, double price, double qty) {
    FIX::UtcTimeStamp now;
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
        msg.set(FIX::Price(price));
    return msg;
}

} // anonymous namespace

// =============================================================================
// 1. Enum Conversion Benchmarks
// =============================================================================

static void BM_FixEnumToSide(benchmark::State& state) {
    const char sides[] = {FIX::Side_BUY, FIX::Side_SELL, FIX::Side_SELL_SHORT};
    int i = 0;
    for (auto _ : state) {
        Side s = FixGateway::toSide(sides[i % 3]);
        benchmark::DoNotOptimize(s);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixEnumToSide);

static void BM_FixEnumFromOrdStatus(benchmark::State& state) {
    const OrderStatus statuses[] = {NEW_ORDSTATUS, PARTFILL_ORDSTATUS, FILLED_ORDSTATUS,
                                     CANCELED_ORDSTATUS, REJECTED_ORDSTATUS};
    int i = 0;
    for (auto _ : state) {
        char c = FixGateway::fromOrdStatus(statuses[i % 5]);
        benchmark::DoNotOptimize(c);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixEnumFromOrdStatus);

static void BM_FixEnumToCurrency(benchmark::State& state) {
    const std::string ccys[] = {"USD", "EUR", "GBP", "JPY", "CHF", "AUD", "CAD", "NZD"};
    int i = 0;
    for (auto _ : state) {
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

static void BM_FixNewOrderSingleTranslation(benchmark::State& state) {
    FixBenchSetup setup;
    IncomingQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(),
                       OrderStorage::instance(), setup.clearingId_);

    int ordNum = 0;
    for (auto _ : state) {
        std::string clOrdId = "BENCH-" + std::to_string(++ordNum);
        auto msg = makeBenchNOS(clOrdId, FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);
        gateway.onMessage(msg, BENCH_SID);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixNewOrderSingleTranslation)->Range(8, 1024);

// =============================================================================
// 3. ExecutionReport Construction Benchmark
// =============================================================================

static void BM_FixExecReportConstruction(benchmark::State& state) {
    int i = 0;
    for (auto _ : state) {
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
BENCHMARK(BM_FixExecReportConstruction)->Range(8, 1024);

// =============================================================================
// 4. MultiOutQueues Fan-Out Overhead
// =============================================================================

static void BM_MultiOutQueuesFanOut(benchmark::State& state) {
    NullOutQueues out1, out2;
    MultiOutQueues multi;
    multi.addDelegate(&out1);
    multi.addDelegate(&out2);

    ExecutionEntry exec;
    ExecReportEvent evt(&exec);

    for (auto _ : state) {
        multi.push(evt, "target");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MultiOutQueuesFanOut);

static void BM_DirectOutQueuesPush(benchmark::State& state) {
    NullOutQueues out;
    ExecutionEntry exec;
    ExecReportEvent evt(&exec);

    for (auto _ : state) {
        out.push(evt, "target");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DirectOutQueuesPush);

// =============================================================================
// 5. FX Swap Matching vs Single-Leg Matching
// =============================================================================

static void BM_SingleLegMatch(benchmark::State& state) {
    FixBenchSetup setup;

    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(setup.instrId_);
    OrderBookImpl orderBook;
    orderBook.init(instruments, &saver);

    IncomingQueues inQueues;
    NullOutQueues outQueues;

    class BenchDeferedContainer : public DeferedEventContainer {
    public:
        void addDeferedEvent(DeferedEventBase* evnt) override { delete evnt; count_++; }
        size_t deferedEventCount() const override { return count_; }
        void removeDeferedEventsFrom(size_t) override {}
        size_t count_ = 0;
    };
    BenchDeferedContainer defered;

    OrderMatcher matcher;
    matcher.init(&defered);

    Context ctxt(OrderStorage::instance(), &orderBook, &inQueues, &outQueues,
                 &matcher, IdTGenerator::instance());

    int ordNum = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Create and add a resting sell order
        SourceIdT srcId, destId, clOrdId, origClOrdId, accountId, clearingId, execList;
        std::string clOrdStr = "SELL-" + std::to_string(++ordNum);
        auto* raw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size()));
        clOrdId = WideDataStorage::instance()->add(raw);

        auto* sell = new OrderEntry(srcId, destId, clOrdId, origClOrdId, setup.instrId_, accountId, clearingId, execList);
        sell->side_ = SELL_SIDE;
        sell->ordType_ = LIMIT_ORDERTYPE;
        sell->price_ = 1.2650;
        sell->orderQty_ = 100;
        sell->leavesQty_ = 100;
        sell->status_ = NEW_ORDSTATUS;
        OrderEntry* savedSell = OrderStorage::instance()->save(*sell, IdTGenerator::instance());
        orderBook.add(*savedSell);

        // Create aggressor buy
        std::string buyClOrdStr = "BUY-" + std::to_string(ordNum);
        auto* buyRaw = new RawDataEntry(STRING_RAWDATATYPE, buyClOrdStr.c_str(), static_cast<u32>(buyClOrdStr.size()));
        SourceIdT buyClOrdId = WideDataStorage::instance()->add(buyRaw);

        auto* buy = new OrderEntry(srcId, destId, buyClOrdId, origClOrdId, setup.instrId_, accountId, clearingId, execList);
        buy->side_ = BUY_SIDE;
        buy->ordType_ = LIMIT_ORDERTYPE;
        buy->price_ = 1.2650;
        buy->orderQty_ = 100;
        buy->leavesQty_ = 100;
        buy->status_ = NEW_ORDSTATUS;
        OrderEntry* savedBuy = OrderStorage::instance()->save(*buy, IdTGenerator::instance());

        defered.count_ = 0;
        state.ResumeTiming();

        matcher.match(savedBuy, ctxt);
        benchmark::DoNotOptimize(defered.count_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleLegMatch)->Range(8, 512);

static void BM_FxSwapMatch(benchmark::State& state) {
    FixBenchSetup setup;

    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(setup.instrId_);
    OrderBookImpl orderBook;
    orderBook.init(instruments, &saver);

    IncomingQueues inQueues;
    NullOutQueues outQueues;

    class BenchDeferedContainer : public DeferedEventContainer {
    public:
        void addDeferedEvent(DeferedEventBase* evnt) override { delete evnt; count_++; }
        size_t deferedEventCount() const override { return count_; }
        void removeDeferedEventsFrom(size_t) override {}
        size_t count_ = 0;
    };
    BenchDeferedContainer defered;

    OrderMatcher matcher;
    matcher.init(&defered);

    Context ctxt(OrderStorage::instance(), &orderBook, &inQueues, &outQueues,
                 &matcher, IdTGenerator::instance());

    int ordNum = 0;
    for (auto _ : state) {
        state.PauseTiming();
        SourceIdT srcId, destId, origClOrdId, accountId, clearingId, execList;

        // Resting sell swap
        std::string sellStr = "SWSELL-" + std::to_string(++ordNum);
        auto* sellRaw = new RawDataEntry(STRING_RAWDATATYPE, sellStr.c_str(), static_cast<u32>(sellStr.size()));
        SourceIdT sellClOrdId = WideDataStorage::instance()->add(sellRaw);

        auto* sell = new OrderEntry(srcId, destId, sellClOrdId, origClOrdId, setup.instrId_, accountId, clearingId, execList);
        sell->side_ = SELL_SIDE;
        sell->ordType_ = FXSWAP_ORDERTYPE;
        sell->price_ = 1.2650;
        sell->farPrice_ = 1.2680;
        sell->settlDate_ = 1000;
        sell->farSettlDate_ = 2000;
        sell->orderQty_ = 100;
        sell->leavesQty_ = 100;
        sell->status_ = NEW_ORDSTATUS;
        OrderEntry* savedSell = OrderStorage::instance()->save(*sell, IdTGenerator::instance());
        orderBook.add(*savedSell);

        // Aggressor buy swap
        std::string buyStr = "SWBUY-" + std::to_string(ordNum);
        auto* buyRaw = new RawDataEntry(STRING_RAWDATATYPE, buyStr.c_str(), static_cast<u32>(buyStr.size()));
        SourceIdT buyClOrdId = WideDataStorage::instance()->add(buyRaw);

        auto* buy = new OrderEntry(srcId, destId, buyClOrdId, origClOrdId, setup.instrId_, accountId, clearingId, execList);
        buy->side_ = BUY_SIDE;
        buy->ordType_ = FXSWAP_ORDERTYPE;
        buy->price_ = 1.2650;
        buy->farPrice_ = 1.2680;
        buy->settlDate_ = 1000;
        buy->farSettlDate_ = 2000;
        buy->orderQty_ = 100;
        buy->leavesQty_ = 100;
        buy->status_ = NEW_ORDSTATUS;
        OrderEntry* savedBuy = OrderStorage::instance()->save(*buy, IdTGenerator::instance());

        defered.count_ = 0;
        state.ResumeTiming();

        matcher.match(savedBuy, ctxt);
        benchmark::DoNotOptimize(defered.count_);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FxSwapMatch)->Range(8, 512);

// =============================================================================
// 6. End-to-End: FIX NOS → Processor → Match → Fill → OutQueues
// =============================================================================

static void BM_FixEndToEndFillCycle(benchmark::State& state) {
    FixBenchSetup setup;

    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(setup.instrId_);
    auto orderBook = std::make_unique<OrderBookImpl>();
    orderBook->init(instruments, &saver);

    auto inQueues = std::make_unique<IncomingQueues>();
    auto outQueues = std::make_unique<NullOutQueues>();

    TransactionMgrParams transParams(IdTGenerator::instance());
    auto transMgr = std::make_unique<TransactionMgr>();
    transMgr->init(transParams);

    ProcessorParams params(
        IdTGenerator::instance(), OrderStorage::instance(),
        orderBook.get(), inQueues.get(), outQueues.get(),
        inQueues.get(), transMgr.get());

    TaskManagerParams tmparams;
    auto* evtProc = new Processor();
    evtProc->init(params);
    tmparams.evntProcessors_.push_back(evtProc);

    auto* trProc = new Processor();
    trProc->init(params);
    tmparams.transactProcessors_.push_back(trProc);

    tmparams.transactMgr_ = transMgr.get();
    tmparams.inQueues_ = inQueues.get();

    TaskManager::init(0);
    auto taskMgr = std::make_unique<TaskManager>(tmparams);

    auto gateway = std::make_unique<FixGateway>(
        inQueues.get(), WideDataStorage::instance(),
        OrderStorage::instance(), setup.clearingId_);

    int ordNum = 0;
    for (auto _ : state) {
        // Submit sell
        std::string sellId = "E2E-S-" + std::to_string(++ordNum);
        auto sellMsg = makeBenchNOS(sellId, FIX::Side_SELL, FIX::OrdType_LIMIT, 1.2650, 100);
        gateway->onMessage(sellMsg, BENCH_SID);
        taskMgr->waitUntilTransactionsFinished(10);

        // Submit buy → triggers match + fill
        std::string buyId = "E2E-B-" + std::to_string(ordNum);
        auto buyMsg = makeBenchNOS(buyId, FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);
        gateway->onMessage(buyMsg, BENCH_SID);
        taskMgr->waitUntilTransactionsFinished(10);
    }
    state.SetItemsProcessed(state.iterations() * 2); // 2 orders per cycle

    transMgr->stop();
    taskMgr->waitUntilTransactionsFinished(5);
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
BENCHMARK(BM_FixEndToEndFillCycle)->Range(1, 64)->Unit(benchmark::kMillisecond);

#endif // BUILD_FIX
