/**
 * Concurrent Order Processor library - FIX Gateway Benchmarks
 *
 * Measures throughput of FIX protocol components:
 *   - Enum conversion (FIX↔COP)
 *   - FIX message translation (NewOrderSingle → OrderEntry)
 *   - ExecutionReports: the gateway's builder, and a report's way to a logged-on session; a new order and its
 *     acknowledgement, without the engine between them (#395)
 *   - A session's cancels, replaces and status requests, and the refusals it is sent: the gateway's own and the
 *     engine's (#423)
 *   - MultiOutQueues fan-out overhead
 *   - FX Swap matching vs single-leg matching
 *   - End-to-end: FIX NOS → IncomingQueues → Processor → match → fill → OutQueues, and FixOutQueues to the gateway
 */

#ifdef BUILD_FIX

// QuickFIX headers MUST come before any <flat_map> include (see FixGateway.h)
#include <quickfix/fix50sp2/NewOrderSingle.h>
#include <quickfix/fix50sp2/ExecutionReport.h>
#include <quickfix/fix50sp2/OrderCancelReplaceRequest.h>
#include <quickfix/fix50sp2/OrderCancelRequest.h>
#include <quickfix/fix50sp2/OrderStatusRequest.h>
#include <quickfix/fixt11/Logon.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include <quickfix/NullStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionFactory.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/Utility.h>
#include "FixGateway.h"
#include "FixOutQueues.h"
#include "MultiOutQueues.h"

#include <benchmark/benchmark.h>
#include <oneapi/tbb/info.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <thread>
#include <utility>

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

const FIX::SessionID BENCH_SID("FIXT.1.1", "BENCH_CLIENT", "ORDER_PROCESSOR", "");

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

FIX50SP2::NewOrderSingle makeBenchNOS(const std::string &clOrdId, char side, char ordType, double price, double qty)
{
    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX50SP2::NewOrderSingle msg;
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

/// The time since start, for SetIterationTime() in a benchmark registered with UseManualTime(), as OrderMatchingBench
/// times by hand (#305): pausing the timer instead reads the thread's CPU clock, about 1.5 us a pause and resume
double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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

/// The gateway's handling of a new order, onMessage() alone, timed by hand. Each order is then answered, untimed, as
/// the engine answers every order: here by its refusal, since none is stored, which the gateway takes with no session to
/// send it to. An order never answered stays in the gateway's record of the orders it has sent (#380), which grew by a
/// key an iteration, where production's holds only the orders on their way (#400).
static void BM_FixNewOrderSingleTranslation(benchmark::State &state)
{
    FixBenchSetup setup;
    IncomingQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    const std::string source = FixGateway::makeSourceString(BENCH_SID);

    int ordNum = 0;
    for (auto _ : state)
    {
        std::string clOrdId = "BENCH-" + std::to_string(++ordNum);
        auto msg = makeBenchNOS(clOrdId, FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);
        const auto start = std::chrono::steady_clock::now();
        gateway.onMessage(msg, BENCH_SID);
        state.SetIterationTime(secondsSince(start));

        OrderRejectEvent answer;
        answer.clOrderId_ = FixGateway::sessionClOrdId(source, clOrdId); // as the engine names it, stored (#71)
        gateway.sendOrderReject(answer, source);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixNewOrderSingleTranslation)->UseManualTime();

// =============================================================================
// 3. ExecutionReports: the gateway's builder, and a report's way to its session (#395)
// =============================================================================
//
// BM_FixExecReportConstruction, which these replace, set 12 fields of a report by hand: it timed QuickFIX, not the
// gateway, so a change to FixGateway::buildExecutionReport couldn't be measured (#395).

namespace
{

/// An order the session sent, stored as the gateway stores one: its source names the session, and its ClOrdID is under
/// the session's key (#71). 40 of 100 are filled at 1.265, so its trade's report has every quantity one has.
OrderEntry *storeBenchOrder(const FixBenchSetup &setup, const std::string &source, OrderType ordType)
{
    WideParamsDataStorage *wide = WideDataStorage::instance();
    const std::string key = FixGateway::sessionClOrdId(source, "CLORD-001");
    OrderEntry order(wide->add(new StringT(source)), wide->add(new StringT("Internal")),
                     wide->add(new RawDataEntry(STRING_RAWDATATYPE, key.c_str(), static_cast<u32>(key.size()))),
                     SourceIdT(), setup.instrId_, SourceIdT(), setup.clearingId_, wide->add(new ExecutionsT()));
    order.side_ = BUY_SIDE;
    order.ordType_ = ordType;
    order.tif_ = DAY_TIF;
    order.price_ = 1.265;
    order.orderQty_ = 100;
    order.cumQty_ = 40;
    order.leavesQty_ = 60;
    order.avgPx_ = 1.265;
    if (FXSWAP_ORDERTYPE == ordType)
    {
        order.settlDate_ = 1793059200000;    // 2026-10-27, the near leg's
        order.farSettlDate_ = 1795737600000; // 2026-11-27
        order.farPrice_ = 1.266;
    }
    return OrderStorage::instance()->save(order, IdTGenerator::instance());
}

/// The order's fill of 40 at 1.265, on one of a swap's legs or on an outright order
TradeExecEntry benchTrade(const OrderEntry &order, ExecLegType leg)
{
    TradeExecEntry trade;
    trade.type_ = TRADE_EXECTYPE;
    trade.orderStatus_ = PARTFILL_ORDSTATUS;
    trade.orderId_ = order.orderId_;
    trade.execId_ = IdTGenerator::instance()->getId();
    trade.lastQty_ = 40;
    trade.lastPx_ = 1.265;
    trade.tradeDate_ = 20261025;
    trade.execLegType_ = leg;
    trade.tradeLink_ = 1;
    return trade;
}

/// The engine's queue, as far as the gateway sees it: it keeps the new order it is handed until take(), which the
/// benchmark stores as the engine's event worker would, and frees a replacement, as the engine's queue frees what it has
/// dispatched
class KeepingInQueues final : public Queues::InQueues
{
public:
    void push(const std::string &, const OrderEvent &evnt) override
    {
        order_.reset(evnt.order_);
    }
    void push(const std::string &, const OrderCancelEvent &) override {}
    void push(const std::string &, const OrderReplaceEvent &evnt) override
    {
        delete evnt.replacementOrder_;
    }
    void push(const std::string &, const OrderChangeStateEvent &) override {}
    void push(const std::string &, const ProcessEvent &) override {}
    void push(const std::string &, const TimerEvent &) override {}

    /// The new order last pushed, if one was pushed since the last take()
    std::unique_ptr<OrderEntry> take()
    {
        return std::move(order_);
    }

private:
    std::unique_ptr<OrderEntry> order_;
};

/// The directory BenchSession reads fix.cfg and its dictionaries from: OP_BENCH_CONFIG_DIR when it is set and not
/// empty, as benchmark-regression.sh sets it to the copy saved with a baseline (#415), and otherwise config/ in the tree
/// this binary was built from (#395). Nothing else in the benchmarks reads a source tree.
std::filesystem::path benchConfigDir()
{
    const char *dir = std::getenv("OP_BENCH_CONFIG_DIR");
    if ((nullptr != dir) && ('\0' != *dir))
    {
        return dir;
    }
    return std::filesystem::path(OP_SOURCE_DIR) / "config";
}

/// fix.cfg's session with BENCH_CLIENT, logged on, as FixtSessionTest makes CLIENT_A's: what it sends goes to this
/// Responder, which counts it and keeps nothing, and its store keeps nothing either
class BenchSession final : public FIX::Responder
{
public:
    explicit BenchSession(FIX::Application &application) : factory_(application, stores_, nullptr)
    {
        const std::filesystem::path config = benchConfigDir();
        try
        {
            const FIX::SessionSettings cfg((config / "fix.cfg").string());
            FIX::Dictionary settings;
            for (const auto &[key, value] : cfg.get())
            {
                // The session's times: a session that never ends has none
                if ((FIX::string_toUpper(FIX::START_TIME) != key) && (FIX::string_toUpper(FIX::END_TIME) != key))
                {
                    settings.setString(key, value);
                }
            }
            settings.setBool(FIX::NON_STOP_SESSION, true);
            for (const char *dictionary : { FIX::TRANSPORT_DATA_DICTIONARY, FIX::APP_DATA_DICTIONARY })
            {
                const std::filesystem::path file = std::filesystem::path(settings.getString(dictionary)).filename();
                settings.setString(dictionary, (config / "spec" / file).string());
            }
            sid_ = FIX::SessionID(settings.getString(FIX::BEGINSTRING), settings.getString(FIX::SENDERCOMPID),
                                  "BENCH_CLIENT");
            session_ = factory_.create(sid_, settings);
            session_->setResponder(this);

            FIXT11::Logon logon(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER), FIX::HeartBtInt(30),
                                FIX::DefaultApplVerID(FIX::ApplVerID_FIX50_SP2));
            FIX::Header &header = logon.getHeader();
            header.setField(FIX::BeginString(sid_.getBeginString().getValue()));
            header.setField(FIX::SenderCompID(sid_.getTargetCompID().getValue()));
            header.setField(FIX::TargetCompID(sid_.getSenderCompID().getValue()));
            header.setField(FIX::MsgSeqNum(1));
            header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
            session_->next(logon.toString(), FIX::UtcTimeStamp::now());
            if (!session_->isLoggedOn())
            {
                problem_ = "BENCH_CLIENT's Logon was refused";
            }
        }
        catch (const std::exception &ex)
        {
            problem_ = "no session from " + (config / "fix.cfg").string() + ": " + ex.what();
        }
    }
    ~BenchSession() override
    {
        if (nullptr != session_)
        {
            factory_.destroy(session_); // before the gateway, its application
        }
    }
    BenchSession(const BenchSession &) = delete;
    BenchSession &operator=(const BenchSession &) = delete;

    // FIX::Responder: the connection to BENCH_CLIENT
    bool send(const std::string &) override
    {
        ++sent_;
        return true;
    }
    void disconnect() override {}

    /// Why there is no logged-on session; empty if there is one
    const std::string &problem() const
    {
        return problem_;
    }
    const FIX::SessionID &sid() const
    {
        return sid_;
    }
    std::string source() const
    {
        return FixGateway::makeSourceString(sid_);
    }
    u64 sent() const
    {
        return sent_;
    }
    /// Hands what the session sends to connection from now on, or back to this when it is null: a warm-up reads what
    /// the session was sent there, while a timed send only counts the message (#423)
    void sendTo(FIX::Responder *connection)
    {
        session_->setResponder((nullptr != connection) ? connection : this);
    }

private:
    FIX::NullStoreFactory stores_;
    FIX::SessionFactory factory_;
    FIX::SessionID sid_;
    FIX::Session *session_ = nullptr;
    std::string problem_;
    u64 sent_ = 0;
};

} // anonymous namespace

/// The gateway's builder for an outright order's trade: what it reads from the order and the report, the enums it maps,
/// the session's key it takes off the ClOrdID, and the fields a trade's report has. With the figures the engine gives
/// every report it makes, as FixOutQueues builds one (#411).
static void BM_FixBuildExecutionReport(benchmark::State &state)
{
    FixBenchSetup setup;
    const OrderEntry *order = storeBenchOrder(setup, FixGateway::makeSourceString(BENCH_SID), LIMIT_ORDERTYPE);
    const TradeExecEntry trade = benchTrade(*order, SINGLE_LEG);
    const OrderFigures figures(*order);
    for (auto _ : state)
    {
        FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order, figures);
        benchmark::DoNotOptimize(report);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildExecutionReport);

/// The same for a swap's near leg, whose report also has a NoLegs group and TrdMatchID (880)
static void BM_FixBuildSwapLegReport(benchmark::State &state)
{
    FixBenchSetup setup;
    const OrderEntry *order = storeBenchOrder(setup, FixGateway::makeSourceString(BENCH_SID), FXSWAP_ORDERTYPE);
    const TradeExecEntry trade = benchTrade(*order, NEAR_LEG);
    const OrderFigures figures(*order);
    for (auto _ : state)
    {
        FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order, figures);
        benchmark::DoNotOptimize(report);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildSwapLegReport);

/// A trade's report on its way from the transaction worker that publishes it to a logged-on session: FixOutQueues finds
/// the order, the gateway finds its session and builds the report, and QuickFIX numbers it, writes it out and hands it
/// to the connection
static void BM_FixSendExecutionReport(benchmark::State &state)
{
    FixBenchSetup setup;
    IncomingQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    FixOutQueues outQueues(&gateway, OrderStorage::instance());
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const OrderEntry *order = storeBenchOrder(setup, session.source(), LIMIT_ORDERTYPE);
    TradeExecEntry trade = benchTrade(*order, SINGLE_LEG);
    ExecReportEvent evnt(&trade);
    evnt.figures_.emplace(*order); // as the engine gives every report it makes (#411)
    const std::string target;
    const u64 before = session.sent();
    for (auto _ : state)
    {
        outQueues.push(evnt, target);
    }
    if (session.sent() - before != static_cast<u64>(state.iterations()))
    {
        state.SkipWithError("the session wasn't sent a report on every iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixSendExecutionReport);

/// A new order through the gateway to the engine's queue, then its acknowledgement through FixOutQueues to the session:
/// what the session's thread and the transaction worker do for each order, without the engine's work between them. Each
/// order has a ClOrdID of its own, as a session's orders do, and is stored, untimed, as the engine's event worker stores
/// one, so that its acknowledgement finds it; onMessage() and the push are timed by hand. Every order had CLORD-001, a
/// stored order's ClOrdID, so each was a duplicate the engine refuses, acknowledged with the stored order's report (#402).
static void BM_FixOrderAndAcknowledgement(benchmark::State &state)
{
    FixBenchSetup setup;
    KeepingInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    FixOutQueues outQueues(&gateway, OrderStorage::instance());
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const std::string target;
    const u64 before = session.sent();
    int ordNum = 0;
    for (auto _ : state)
    {
        const FIX50SP2::NewOrderSingle nos =
            makeBenchNOS("CLORD-" + std::to_string(++ordNum), FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);
        auto start = std::chrono::steady_clock::now();
        gateway.onMessage(nos, session.sid());
        const double sent = secondsSince(start);

        // As the engine's event worker stores the order and accepts it
        const std::unique_ptr<OrderEntry> pushed = inQueues.take();
        OrderEntry *order =
            (nullptr == pushed) ? nullptr : OrderStorage::instance()->save(*pushed, IdTGenerator::instance());
        if (nullptr == order)
        {
            state.SkipWithError("the gateway's order wasn't pushed and stored");
            break;
        }
        order->status_ = NEW_ORDSTATUS;
        ExecutionEntry ack;
        ack.type_ = NEW_EXECTYPE;
        ack.orderStatus_ = NEW_ORDSTATUS;
        ack.orderId_ = order->orderId_;
        ack.execId_ = IdTGenerator::instance()->getId();
        ExecReportEvent evnt(&ack);
        evnt.figures_.emplace(*order); // as the engine gives every report it makes (#411)

        start = std::chrono::steady_clock::now();
        outQueues.push(evnt, target);
        state.SetIterationTime(sent + secondsSince(start));
    }
    // The order goes to the queue, and only its acknowledgement to the session
    if (!state.error_occurred() && (session.sent() - before != static_cast<u64>(state.iterations())))
    {
        state.SkipWithError("the session wasn't sent one report an iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixOrderAndAcknowledgement)->UseManualTime();

// =============================================================================
// 4. A session's requests about its order, and the refusals it is sent (#423)
// =============================================================================
//
// What the gateway does with a session's cancel, replace or status request for an order it sent, and with a new order
// that repeats the order's ClOrdID; and the refusals it sends: its own, and the engine's. Each benchmark stores one
// order of the session's, storeBenchOrder()'s limit buy with 40 of 100 filled, working. The engine isn't there: a
// cancel or replace goes to a stand-in for its queue, which counts it, and the order stays as it was stored. A
// benchmark whose answer goes to the session first checks, in a warm-up, what the session was sent, or fails
// (SkipWithError); one whose request goes to the engine checks what its queue was handed.

namespace
{

/// The engine's queue, as far as a session's cancels and replaces go: it counts what it is handed and keeps what the
/// last cancel and replacement named, and it frees each order it is handed, as the engine's queue frees what it has
/// dispatched
class RequestInQueues final : public Queues::InQueues
{
public:
    void push(const std::string &, const OrderEvent &evnt) override
    {
        ++orders_;
        delete evnt.order_;
    }
    void push(const std::string &, const OrderCancelEvent &evnt) override
    {
        ++cancels_;
        cancelled_ = evnt.id_;
        cancelClOrdId_ = evnt.requestClOrdId_;
    }
    void push(const std::string &, const OrderReplaceEvent &evnt) override
    {
        ++replaces_;
        replaced_ = evnt.id_;
        replacementPrice_ = evnt.replacementOrder_->price_;
        delete evnt.replacementOrder_;
    }
    void push(const std::string &, const OrderChangeStateEvent &) override {}
    void push(const std::string &, const ProcessEvent &) override {}
    void push(const std::string &, const TimerEvent &) override {}

    u64 orders_ = 0;
    u64 cancels_ = 0;
    u64 replaces_ = 0;
    IdT cancelled_;             // the order the last cancel named
    std::string cancelClOrdId_; // and the request's ClOrdID it carried (#98)
    IdT replaced_;              // the order the last replacement was to replace
    PriceT replacementPrice_ = 0;
};

/// A connection that keeps the last message it was sent. BenchSession sends to it in a warm-up, which reads what the
/// session was sent, and to itself otherwise, so that a timed send only counts the message.
class KeptMessage final : public FIX::Responder
{
public:
    bool send(const std::string &message) override
    {
        message_ = message;
        return true;
    }
    void disconnect() override {}

    /// Whether the message has each field, by tag and value: its header's or its body's. When it hasn't, *problem says
    /// which field it lacks or what it has instead.
    bool has(std::initializer_list<std::pair<int, std::string>> fields, std::string *problem) const
    {
        try
        {
            const FIX::Message sent(message_, false);
            const FIX::FieldMap &header = sent.getHeader();
            for (const auto &[tag, value] : fields)
            {
                const FIX::FieldMap &part = header.isSetField(tag) ? header : sent;
                const std::string found = part.isSetField(tag) ? part.getField(tag) : std::string("nothing");
                if (found != value)
                {
                    *problem = "the session was sent " + std::to_string(tag) + "=" + found + ", not " + value;
                    return false;
                }
            }
            return true;
        }
        catch (const std::exception &ex)
        {
            *problem = std::string("the session was sent no FIX message: ") + ex.what();
            return false;
        }
    }

private:
    std::string message_;
};

/// The session's order, stored and working: storeBenchOrder()'s limit buy, 40 of 100 filled
OrderEntry *storeWorkingOrder(const FixBenchSetup &setup, const std::string &source)
{
    OrderEntry *order = storeBenchOrder(setup, source, LIMIT_ORDERTYPE);
    order->status_ = PARTFILL_ORDSTATUS;
    return order;
}

/// A cancel request from the session, for the order origClOrdId, as clOrdId
FIX50SP2::OrderCancelRequest makeBenchCancel(const std::string &origClOrdId, const std::string &clOrdId)
{
    FIX50SP2::OrderCancelRequest msg;
    msg.set(FIX::OrigClOrdID(origClOrdId));
    msg.set(FIX::ClOrdID(clOrdId));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    msg.set(FIX::Symbol("BENCHFX"));
    return msg;
}

/// A replace request from the session, for its working order at a new price, as clOrdId: what a limit order's replace
/// repeats, and the price
FIX50SP2::OrderCancelReplaceRequest makeBenchReplace(const std::string &clOrdId, double price)
{
    FIX50SP2::OrderCancelReplaceRequest msg;
    msg.set(FIX::OrigClOrdID("CLORD-001"));
    msg.set(FIX::ClOrdID(clOrdId));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    msg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.set(FIX::Symbol("BENCHFX"));
    msg.set(FIX::Price(price));
    msg.set(FIX::OrderQty(100));
    return msg;
}

/// The engine's refusal of the session's replacement clOrdId of its working order, naming both as the engine does,
/// under the session's key (#71): a cancel or replace of the order was pending when the engine decided this one
Queues::OrderRejectEvent benchReplaceRefusal(const std::string &source, const std::string &clOrdId)
{
    Queues::OrderRejectEvent refusal;
    refusal.replacement_ = true;
    refusal.clOrderId_ = FixGateway::sessionClOrdId(source, clOrdId);
    refusal.origClOrderId_ = FixGateway::sessionClOrdId(source, "CLORD-001");
    refusal.symbol_ = "BENCHFX";
    refusal.side_ = BUY_SIDE;
    refusal.orderQty_ = 100;
    refusal.refusal_ = Queues::CancelRejectEvent::PENDING;
    refusal.origStatus_ = PARTFILL_ORDSTATUS;
    refusal.reason_ = "Replace refused: a cancel or replace of the order is pending";
    return refusal;
}

/// The price each replacement asks for, which the engine's queue must be handed
constexpr double BENCH_REPLACE_PRICE = 1.2660;

} // anonymous namespace

/// A cancel request for the session's working order, through onMessage() to the engine's queue: the gateway finds the
/// order under the session's key for its OrigClOrdID (41) (#71), one the engine has answered (#380), and hands the
/// engine its cancel with the request's ClOrdID (#98). The gateway keeps nothing of a cancel it hands on, so every
/// iteration sends the same request.
static void BM_FixCancelRequest(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const OrderEntry *order = storeWorkingOrder(setup, session.source());
    const FIX50SP2::OrderCancelRequest cancel = makeBenchCancel("CLORD-001", "CXL-001");
    const u64 before = session.sent();
    for (auto _ : state)
    {
        gateway.onMessage(cancel, session.sid());
    }
    // Each cancel to the engine, naming the order and the request, and nothing to the session
    if ((static_cast<u64>(state.iterations()) != inQueues.cancels_) || (order->orderId_ != inQueues.cancelled_) ||
        ("CXL-001" != inQueues.cancelClOrdId_) || (before != session.sent()))
    {
        state.SkipWithError("the gateway didn't hand the engine each cancel of the session's order");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixCancelRequest);

/// A replace request for the session's working order, at a new price, through onMessage() to the engine's queue: the
/// gateway finds the order, clones it under its read lock (#74), checks the request's fields, refuses a ClOrdID in use
/// (#403), stores the request's ClOrdID, and hands the engine the replacement, which it keeps as unanswered until the
/// engine answers it (#380). Each request has a ClOrdID of its own, as a session's do, and is built untimed; onMessage()
/// is timed by hand. Each is then answered, untimed, by the engine's refusal through FixOutQueues, as
/// BM_FixNewOrderSingleTranslation answers its orders, so that none is left unanswered: the order stays as it was
/// stored, and every iteration replaces it.
static void BM_FixReplaceRequest(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    FixOutQueues outQueues(&gateway, OrderStorage::instance());
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const OrderEntry *order = storeWorkingOrder(setup, session.source());
    const std::string source = session.source();
    const u64 before = session.sent();
    int reqNum = 0;
    for (auto _ : state)
    {
        const std::string clOrdId = "REPL-" + std::to_string(++reqNum);
        const FIX50SP2::OrderCancelReplaceRequest replace = makeBenchReplace(clOrdId, BENCH_REPLACE_PRICE);
        const auto start = std::chrono::steady_clock::now();
        gateway.onMessage(replace, session.sid());
        state.SetIterationTime(secondsSince(start));

        outQueues.push(benchReplaceRefusal(source, clOrdId), source);
    }
    // Each replacement to the engine, of the order at the new price, and to the session only the refusal of each
    if ((static_cast<u64>(state.iterations()) != inQueues.replaces_) || (order->orderId_ != inQueues.replaced_) ||
        (BENCH_REPLACE_PRICE != inQueues.replacementPrice_) ||
        (static_cast<u64>(state.iterations()) != session.sent() - before))
    {
        state.SkipWithError("the gateway didn't hand the engine each replacement of the session's order");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixReplaceRequest)->UseManualTime();

/// The engine's refusal of a replacement, from the transaction worker through FixOutQueues to the session: the gateway
/// answers the replacement's request (#380), remembers the replacement as refused (#398), finds the order it was to
/// replace, and sends the session an OrderCancelReject (35=9) for its replace request, with that order's OrderID and the
/// status the engine refused it in (#74). Each replacement is first sent, untimed, through onMessage(), as
/// BM_FixReplaceRequest times it, so that each refusal answers a request the gateway handed on; the push is timed by
/// hand.
static void BM_FixReplaceRefusal(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    FixOutQueues outQueues(&gateway, OrderStorage::instance());
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const OrderEntry *order = storeWorkingOrder(setup, session.source());
    const std::string source = session.source();

    KeptMessage kept;
    session.sendTo(&kept);
    gateway.onMessage(makeBenchReplace("REPL-0", BENCH_REPLACE_PRICE), session.sid());
    outQueues.push(benchReplaceRefusal(source, "REPL-0"), source);
    session.sendTo(nullptr);
    std::string problem;
    if (!kept.has({ { FIX::FIELD::MsgType, FIX::MsgType_OrderCancelReject },
                    { FIX::FIELD::CxlRejResponseTo, "2" },
                    { FIX::FIELD::CxlRejReason, "3" },
                    { FIX::FIELD::ClOrdID, "REPL-0" },
                    { FIX::FIELD::OrigClOrdID, "CLORD-001" },
                    { FIX::FIELD::OrderID, std::to_string(order->orderId_.id_) },
                    { FIX::FIELD::OrdStatus, "1" } },
                  &problem))
    {
        state.SkipWithError(problem.c_str());
        return;
    }

    const u64 before = session.sent();
    int reqNum = 0;
    for (auto _ : state)
    {
        const std::string clOrdId = "REPL-" + std::to_string(++reqNum);
        gateway.onMessage(makeBenchReplace(clOrdId, BENCH_REPLACE_PRICE), session.sid());
        const Queues::OrderRejectEvent refusal = benchReplaceRefusal(source, clOrdId);
        const auto start = std::chrono::steady_clock::now();
        outQueues.push(refusal, source);
        state.SetIterationTime(secondsSince(start));
    }
    if (static_cast<u64>(state.iterations()) != session.sent() - before)
    {
        state.SkipWithError("the session wasn't sent a refusal on every iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixReplaceRefusal)->UseManualTime();

/// A cancel request for an order the session never sent, refused by the gateway itself: it finds no order under the
/// session's key (#71), none on its way to the engine (#380) and no refused replacement by that ClOrdID (#398), and
/// sends the session an OrderCancelReject (35=9) for an unknown order, quoting the request's ClOrdIDs (#73). The gateway
/// logs each at Error, which the benchmarks can't turn off, so the time includes making the log line and handing it to
/// spdlog's thread, which writes it to exchange.log.
static void BM_FixCancelOfUnknownOrder(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    storeWorkingOrder(setup, session.source());
    const FIX50SP2::OrderCancelRequest cancel = makeBenchCancel("CLORD-404", "CXL-001");

    KeptMessage kept;
    session.sendTo(&kept);
    gateway.onMessage(cancel, session.sid());
    session.sendTo(nullptr);
    std::string problem;
    if (!kept.has({ { FIX::FIELD::MsgType, FIX::MsgType_OrderCancelReject },
                    { FIX::FIELD::CxlRejResponseTo, "1" },
                    { FIX::FIELD::CxlRejReason, "1" },
                    { FIX::FIELD::ClOrdID, "CXL-001" },
                    { FIX::FIELD::OrigClOrdID, "CLORD-404" },
                    { FIX::FIELD::OrderID, "NONE" },
                    { FIX::FIELD::OrdStatus, "8" } },
                  &problem))
    {
        state.SkipWithError(problem.c_str());
        return;
    }

    const u64 before = session.sent();
    for (auto _ : state)
    {
        gateway.onMessage(cancel, session.sid());
    }
    if ((static_cast<u64>(state.iterations()) != session.sent() - before) || (0 != inQueues.cancels_))
    {
        state.SkipWithError("the session wasn't sent a refusal on every iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixCancelOfUnknownOrder);

/// The engine's refusal of a cancel, from the transaction worker through FixOutQueues to the session that sent it: the
/// order had filled when the engine came to the cancel, too late (102=0). The gateway finds the session and the order's
/// OrderID and ClOrdID under its read lock, and sends an OrderCancelReject (35=9) with the request's ClOrdID (#98) and
/// the status the engine refused it in (#73).
static void BM_FixEngineCancelReject(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    FixOutQueues outQueues(&gateway, OrderStorage::instance());
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    OrderEntry *order = storeWorkingOrder(setup, session.source());
    // Filled before the engine came to the cancel
    order->cumQty_ = 100;
    order->leavesQty_ = 0;
    order->status_ = FILLED_ORDSTATUS;
    Queues::CancelRejectEvent reject;
    reject.id_ = order->orderId_;
    reject.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    reject.ordStatus_ = FILLED_ORDSTATUS;
    reject.requestClOrdId_ = "CXL-001";
    const std::string source = session.source();

    KeptMessage kept;
    session.sendTo(&kept);
    outQueues.push(reject, source);
    session.sendTo(nullptr);
    std::string problem;
    if (!kept.has({ { FIX::FIELD::MsgType, FIX::MsgType_OrderCancelReject },
                    { FIX::FIELD::CxlRejResponseTo, "1" },
                    { FIX::FIELD::CxlRejReason, "0" },
                    { FIX::FIELD::ClOrdID, "CXL-001" },
                    { FIX::FIELD::OrigClOrdID, "CLORD-001" },
                    { FIX::FIELD::OrderID, std::to_string(order->orderId_.id_) },
                    { FIX::FIELD::OrdStatus, "2" } },
                  &problem))
    {
        state.SkipWithError(problem.c_str());
        return;
    }

    const u64 before = session.sent();
    for (auto _ : state)
    {
        outQueues.push(reject, source);
    }
    if (static_cast<u64>(state.iterations()) != session.sent() - before)
    {
        state.SkipWithError("the session wasn't sent a refusal on every iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixEngineCancelReject);

/// A status request (35=H) for the session's working order, by its ClOrdID, answered on the session's thread (#383): the
/// gateway finds the order under the session's key, reads what it has, and sends an ExecutionReport of ExecType Order
/// Status (150=I) with the request's OrdStatusReqID (790). A status request only reads, so every iteration sends the
/// same one.
static void BM_FixOrderStatusRequest(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const OrderEntry *order = storeWorkingOrder(setup, session.source());
    FIX50SP2::OrderStatusRequest request;
    request.set(FIX::ClOrdID("CLORD-001"));
    request.set(FIX::OrdStatusReqID("STATUS-001"));
    request.set(FIX::Symbol("BENCHFX"));
    request.set(FIX::Side(FIX::Side_BUY));

    KeptMessage kept;
    session.sendTo(&kept);
    gateway.onMessage(request, session.sid());
    session.sendTo(nullptr);
    std::string problem;
    if (!kept.has({ { FIX::FIELD::MsgType, FIX::MsgType_ExecutionReport },
                    { FIX::FIELD::ExecType, "I" },
                    { FIX::FIELD::OrdStatus, "1" },
                    { FIX::FIELD::ClOrdID, "CLORD-001" },
                    { FIX::FIELD::OrderID, std::to_string(order->orderId_.id_) },
                    { FIX::FIELD::OrdStatusReqID, "STATUS-001" } },
                  &problem))
    {
        state.SkipWithError(problem.c_str());
        return;
    }

    const u64 before = session.sent();
    for (auto _ : state)
    {
        gateway.onMessage(request, session.sid());
    }
    if (static_cast<u64>(state.iterations()) != session.sent() - before)
    {
        state.SkipWithError("the session wasn't sent an answer on every iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixOrderStatusRequest);

/// A new order that repeats the ClOrdID of the session's working order, refused before the engine sees it (#382): the
/// gateway finds the order the ClOrdID names, reads what it has, and sends the session an ExecutionReport refusing the
/// new one as a duplicate (150=8, 103=6), with the status of the order that has the ClOrdID (EP284's F.1.a). The gateway
/// keeps nothing of an order it refuses, so every iteration sends the same one.
static void BM_FixRepeatedClOrdId(benchmark::State &state)
{
    FixBenchSetup setup;
    RequestInQueues inQueues;
    FixGateway gateway(&inQueues, WideDataStorage::instance(), OrderStorage::instance(), setup.clearingId_);
    BenchSession session(gateway);
    if (!session.problem().empty())
    {
        state.SkipWithError(session.problem().c_str());
        return;
    }
    const OrderEntry *order = storeWorkingOrder(setup, session.source());
    const FIX50SP2::NewOrderSingle nos = makeBenchNOS("CLORD-001", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.2650, 100);

    KeptMessage kept;
    session.sendTo(&kept);
    gateway.onMessage(nos, session.sid());
    session.sendTo(nullptr);
    std::string problem;
    if (!kept.has({ { FIX::FIELD::MsgType, FIX::MsgType_ExecutionReport },
                    { FIX::FIELD::ExecType, "8" },
                    { FIX::FIELD::OrdRejReason, "6" },
                    { FIX::FIELD::OrdStatus, "1" },
                    { FIX::FIELD::ClOrdID, "CLORD-001" },
                    { FIX::FIELD::OrderID, std::to_string(order->orderId_.id_) } },
                  &problem))
    {
        state.SkipWithError(problem.c_str());
        return;
    }

    const u64 before = session.sent();
    for (auto _ : state)
    {
        gateway.onMessage(nos, session.sid());
    }
    if ((static_cast<u64>(state.iterations()) != session.sent() - before) || (0 != inQueues.orders_))
    {
        state.SkipWithError("the session wasn't sent a refusal on every iteration");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixRepeatedClOrdId);

// =============================================================================
// 5. MultiOutQueues Fan-Out Overhead
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
// 6. FX Swap Matching vs Single-Leg Matching
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
// 7. End-to-End: FIX NOS → Processor → Match → Fill → OutQueues
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

    // The engine's reports go to the gateway too, after the counting queue, as main.cpp sends them to the WebSocket
    // server's queues and then to FIX's. No session is logged on, so the gateway sends nothing; but each order's first
    // report answers the order, as in production. Unanswered, every order stayed in the gateway's record of the orders
    // it has sent (#380), which grew by two keys a cycle (#400).
    auto gateway = std::make_unique<FixGateway>(inQueues.get(), WideDataStorage::instance(), OrderStorage::instance(),
                                                setup.clearingId_);
    auto fixOutQueues = std::make_unique<FixOutQueues>(gateway.get(), OrderStorage::instance());
    auto reports = std::make_unique<MultiOutQueues>();
    reports->addDelegate(outQueues.get());
    reports->addDelegate(fixOutQueues.get());

    TransactionMgrParams transParams(IdTGenerator::instance());
    auto transMgr = std::make_unique<TransactionMgr>();
    transMgr->init(transParams);

    ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook.get(), inQueues.get(),
                           reports.get(), inQueues.get(), transMgr.get());

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
    reports.reset();
    fixOutQueues.reset();
    outQueues.reset();
    inQueues.reset();
    orderBook.reset();
    gateway.reset();
    TaskManager::destroy();
}
BENCHMARK(BM_FixEndToEndFillCycle)->UseRealTime()->Unit(benchmark::kMicrosecond);

#endif // BUILD_FIX
