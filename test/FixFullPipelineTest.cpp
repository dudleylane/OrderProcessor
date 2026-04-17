/**
 Concurrent Order Processor — Full Pipeline Component Coverage Test

 A single test that submits two FIX orders (sell then buy) through the
 complete pipeline and asserts on artifacts from EVERY component:

   FixGateway → IncomingQueues → TaskManager → Processor → StateMachine
   → OrderStorage → WideDataStorage → OrderBookImpl → OrderMatcher
   → TransactionScope/Pool → TransactionMgr → DeferedEvents → TrOperations
   → OutQueues/MultiOutQueues → IdTGenerator → OrderCodec
*/

#ifdef BUILD_FIX

// QuickFIX headers MUST come before any <flat_map> include (see FixGateway.h)
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include "FixGateway.h"
#include "MultiOutQueues.h"

#include <gtest/gtest.h>
#include <atomic>
#include <deque>
#include <mutex>

#include "DataModelDef.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "IncomingQueues.h"
#include "TransactionMgr.h"
#include "TransactionScopePool.h"
#include "Processor.h"
#include "TaskManager.h"
#include "Logger.h"
#include "SubscrManager.h"
#include "OrderCodec.h"

#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::ACID;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Tasks;
using namespace COP::App;
using test::DummyOrderSaver;

namespace {

// Capturing OutQueues that records every event
struct CapturedReport {
    IdT orderId;
    IdT execId;
    ExecType execType;
    OrderStatus orderStatus;
};

class RecordingOutQueues : public Queues::OutQueues {
public:
    void push(const ExecReportEvent& evnt, const std::string&) override {
        std::lock_guard<std::mutex> lock(mtx_);
        CapturedReport r;
        r.orderId = evnt.exec_->orderId_;
        r.execId = evnt.exec_->execId_;
        r.execType = evnt.exec_->type_;
        r.orderStatus = evnt.exec_->orderStatus_;
        reports_.push_back(r);
    }
    void push(const CancelRejectEvent&, const std::string&) override {}
    void push(const BusinessRejectEvent&, const std::string&) override {}

    std::deque<CapturedReport> getReports() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return reports_;
    }

    size_t reportCount() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return reports_.size();
    }

private:
    mutable std::mutex mtx_;
    std::deque<CapturedReport> reports_;
};

// Counting OutQueues — verifies MultiOutQueues fan-out
class CountingOutQueues : public Queues::OutQueues {
public:
    void push(const ExecReportEvent&, const std::string&) override { count_++; }
    void push(const CancelRejectEvent&, const std::string&) override { count_++; }
    void push(const BusinessRejectEvent&, const std::string&) override { count_++; }
    std::atomic<int> count_{0};
};

SourceIdT addTestInstrument(const std::string& symbol) {
    auto* instr = new InstrumentEntry();
    instr->symbol_ = symbol;
    instr->securityId_ = "SEC1";
    instr->securityIdSource_ = "SRC1";
    instr->instrumentType_ = EQUITY_INSTRUMENTTYPE;
    return WideDataStorage::instance()->add(instr);
}

// =============================================================================
// Full Pipeline Test
// =============================================================================

TEST(FixFullPipelineTest, EveryComponentTouched) {
    // =========================================================================
    // SETUP: Create all real components — no mocks
    // =========================================================================

    WideDataStorage::create();
    SubscrMgr::SubscriptionMgr::create();
    IdTGenerator::create();
    OrderStorage::create();

    // [WideDataStorage] — reference data
    SourceIdT instrId = addTestInstrument("GBPUSD");

    auto* acct = new AccountEntry();
    acct->account_ = "PIPELINE_ACCT";
    acct->firm_ = "PIPELINE_FIRM";
    acct->type_ = PRINCIPAL_ACCOUNTTYPE;
    SourceIdT acctId = WideDataStorage::instance()->add(acct);

    auto* clearing = new ClearingEntry();
    clearing->firm_ = "PIPELINE_CLR";
    SourceIdT clearingId = WideDataStorage::instance()->add(clearing);

    // [OrderBookImpl]
    DummyOrderSaver saver;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(instrId);
    auto orderBook = std::make_unique<OrderBookImpl>();
    orderBook->init(instruments, &saver);

    // [IncomingQueues]
    auto inQueues = std::make_unique<IncomingQueues>();

    // [RecordingOutQueues + CountingOutQueues + MultiOutQueues]
    auto recordingOut = std::make_unique<RecordingOutQueues>();
    auto countingOut = std::make_unique<CountingOutQueues>();
    auto multiOut = std::make_unique<MultiOutQueues>();
    multiOut->addDelegate(recordingOut.get());
    multiOut->addDelegate(countingOut.get());

    // [TransactionMgr]
    TransactionMgrParams transParams(IdTGenerator::instance());
    auto transMgr = std::make_unique<TransactionMgr>();
    transMgr->init(transParams);

    // [Processor] with [TransactionScopePool]
    ProcessorParams params(
        IdTGenerator::instance(),
        OrderStorage::instance(),
        orderBook.get(),
        inQueues.get(),
        multiOut.get(),        // MultiOutQueues → fans to both recording + counting
        inQueues.get(),
        transMgr.get());

    TaskManagerParams tmparams;
    auto* evtProc = new Processor();
    evtProc->init(params);
    tmparams.evntProcessors_.push_back(evtProc);

    auto* trProc = new Processor();
    trProc->init(params);
    tmparams.transactProcessors_.push_back(trProc);

    tmparams.transactMgr_ = transMgr.get();
    tmparams.inQueues_ = inQueues.get();

    // [TaskManager]
    TaskManager::init(0);
    auto taskMgr = std::make_unique<TaskManager>(tmparams);

    // [FixGateway]
    auto gateway = std::make_unique<FixGateway>(
        inQueues.get(),
        WideDataStorage::instance(),
        OrderStorage::instance(),
        clearingId);

    const FIX::SessionID sid("FIX.4.4", "PIPELINE_CLIENT", "ORDER_PROCESSOR", "");

    auto waitForProcessing = [&]() {
        taskMgr->waitUntilTransactionsFinished(10);
    };

    // =========================================================================
    // ACT: Submit sell limit, then buy limit — full match
    // =========================================================================

    // --- STEP 1: Submit SELL via FixGateway ---
    {
        FIX::UtcTimeStamp now;
        FIX44::NewOrderSingle sellMsg;
        sellMsg.set(FIX::ClOrdID("PIPE-SELL-001"));
        sellMsg.set(FIX::Side(FIX::Side_SELL));
        sellMsg.set(FIX::TransactTime(now));
        sellMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
        sellMsg.set(FIX::Symbol("GBPUSD"));
        sellMsg.set(FIX::OrderQty(500));
        sellMsg.set(FIX::Price(1.2650));
        sellMsg.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
        sellMsg.set(FIX::Account("PIPELINE_ACCT"));
        sellMsg.set(FIX::Currency("GBP"));

        // [FixGateway] translates FIX → OrderEntry → pushes to IncomingQueues
        gateway->onMessage(sellMsg, sid);
    }
    waitForProcessing();

    // --- STEP 2: Submit BUY at same price ---
    {
        FIX::UtcTimeStamp now;
        FIX44::NewOrderSingle buyMsg;
        buyMsg.set(FIX::ClOrdID("PIPE-BUY-001"));
        buyMsg.set(FIX::Side(FIX::Side_BUY));
        buyMsg.set(FIX::TransactTime(now));
        buyMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
        buyMsg.set(FIX::Symbol("GBPUSD"));
        buyMsg.set(FIX::OrderQty(500));
        buyMsg.set(FIX::Price(1.2650));
        buyMsg.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
        buyMsg.set(FIX::Account("PIPELINE_ACCT"));
        buyMsg.set(FIX::Currency("GBP"));

        gateway->onMessage(buyMsg, sid);
    }
    waitForProcessing();

    // =========================================================================
    // ASSERT: Verify every component left its mark
    // =========================================================================

    // --- [FixGateway] Source string proves FIX gateway translated the order ---
    RawDataEntry sellKey(STRING_RAWDATATYPE, "PIPE-SELL-001", 13);
    RawDataEntry buyKey(STRING_RAWDATATYPE, "PIPE-BUY-001", 12);
    OrderEntry* sellOrder = OrderStorage::instance()->locateByClOrderId(sellKey);
    OrderEntry* buyOrder = OrderStorage::instance()->locateByClOrderId(buyKey);
    ASSERT_NE(nullptr, sellOrder) << "[FixGateway] sell order not found in storage";
    ASSERT_NE(nullptr, buyOrder)  << "[FixGateway] buy order not found in storage";

    std::string sellSource = sellOrder->source_.get();
    EXPECT_EQ("FIX:PIPELINE_CLIENT->ORDER_PROCESSOR", sellSource)
        << "[FixGateway] source string proves FIX gateway translation";

    // --- [WideDataStorage] Instrument, account, clearing resolved ---
    EXPECT_EQ("GBPUSD", sellOrder->instrument_.get().symbol_)
        << "[WideDataStorage] instrument resolved";
    EXPECT_EQ("PIPELINE_ACCT", sellOrder->account_.get().account_)
        << "[WideDataStorage] account resolved";
    EXPECT_EQ("PIPELINE_CLR", sellOrder->clearing_.get().firm_)
        << "[WideDataStorage] clearing resolved";

    // --- [IdTGenerator] Unique IDs assigned ---
    EXPECT_TRUE(sellOrder->orderId_.isValid()) << "[IdTGenerator] sell orderId assigned";
    EXPECT_TRUE(buyOrder->orderId_.isValid())  << "[IdTGenerator] buy orderId assigned";
    EXPECT_NE(sellOrder->orderId_.id_, buyOrder->orderId_.id_)
        << "[IdTGenerator] unique IDs for distinct orders";

    // --- [StateMachine] Full lifecycle: Rcvd_New → New → Filled ---
    EXPECT_EQ(FILLED_ORDSTATUS, sellOrder->status_)
        << "[StateMachine] sell reached FILLED";
    EXPECT_EQ(FILLED_ORDSTATUS, buyOrder->status_)
        << "[StateMachine] buy reached FILLED";

    // --- [OrderMatcher + DeferedEvents] Trade executed — qty decremented ---
    EXPECT_EQ(500u, sellOrder->cumQty_)
        << "[OrderMatcher] sell cumQty shows fill";
    EXPECT_EQ(0u,   sellOrder->leavesQty_)
        << "[OrderMatcher] sell leavesQty shows complete fill";
    EXPECT_EQ(500u, buyOrder->cumQty_)
        << "[OrderMatcher] buy cumQty shows fill";
    EXPECT_EQ(0u,   buyOrder->leavesQty_)
        << "[OrderMatcher] buy leavesQty shows complete fill";

    // --- [OrderStorage] Retrievable by BOTH orderId and clOrderId ---
    OrderEntry* sellById = OrderStorage::instance()->locateByOrderId(sellOrder->orderId_);
    EXPECT_EQ(sellOrder, sellById)
        << "[OrderStorage] locateByOrderId works";
    OrderEntry* sellByClId = OrderStorage::instance()->locateByClOrderId(sellKey);
    EXPECT_EQ(sellOrder, sellByClId)
        << "[OrderStorage] locateByClOrderId works";

    // --- [TrOperations] Execution entries created ---
    EXPECT_GT(sellOrder->executions_.get()->size(), 0u)
        << "[TrOperations] sell has execution entries";
    EXPECT_GT(buyOrder->executions_.get()->size(), 0u)
        << "[TrOperations] buy has execution entries";

    // Verify at least one execution entry is a TRADE in OrderStorage
    bool foundTradeExec = false;
    for (const auto& ev : *sellOrder->executions_.get()) {
        ExecutionEntry* ex = OrderStorage::instance()->locateByExecId(ev.eventId_);
        if (ex && ex->type_ == TRADE_EXECTYPE) {
            foundTradeExec = true;
            EXPECT_TRUE(ex->execId_.isValid())
                << "[TrOperations] trade execution has valid execId";
            break;
        }
    }
    EXPECT_TRUE(foundTradeExec)
        << "[TrOperations] sell order has TRADE execution in storage";

    // --- [OrderBookImpl] Orders removed from book after fill ---
    IdT topBid = orderBook->getTop(instrId, BUY_SIDE);
    IdT topAsk = orderBook->getTop(instrId, SELL_SIDE);
    EXPECT_FALSE(topBid.isValid())
        << "[OrderBookImpl] no bids remaining after full fill";
    EXPECT_FALSE(topAsk.isValid())
        << "[OrderBookImpl] no asks remaining after full fill";

    // --- [TransactionScopePool] Arena allocation used (zero cache misses) ---
    const TransactionScopePool* pool = evtProc->scopePool();
    ASSERT_NE(nullptr, pool);
    EXPECT_EQ(0u, pool->cacheMisses())
        << "[TransactionScopePool] zero heap fallback — arena allocation worked";

    // --- [MultiOutQueues] Fan-out verified — both delegates received events ---
    EXPECT_GT(recordingOut->reportCount(), 0u)
        << "[MultiOutQueues] recording delegate received reports";
    EXPECT_GT(countingOut->count_.load(), 0)
        << "[MultiOutQueues] counting delegate received reports";
    EXPECT_EQ(recordingOut->reportCount(), static_cast<size_t>(countingOut->count_.load()))
        << "[MultiOutQueues] both delegates received same count";

    // --- [OutQueues] Verify trade execution reports have correct content ---
    auto reports = recordingOut->getReports();
    int tradeReports = 0;
    for (const auto& r : reports) {
        if (r.execType == TRADE_EXECTYPE) {
            tradeReports++;
            EXPECT_TRUE(r.execId.isValid())
                << "[OutQueues] trade report has valid execId";
            EXPECT_TRUE(r.orderId.isValid())
                << "[OutQueues] trade report has valid orderId";
        }
    }
    EXPECT_GE(tradeReports, 2)
        << "[OutQueues] at least 2 trade reports (one per side)";

    // --- [OrderCodec] Round-trip encode/decode preserves filled order ---
    {
        std::string buf;
        IdT codecId;
        u32 version;
        COP::Codec::OrderCodec codec;
        codec.encode(*sellOrder, &buf, &codecId, &version);

        EXPECT_EQ(sellOrder->orderId_, codecId)
            << "[OrderCodec] encoded orderId matches";

        OrderEntry* decoded = codec.decode(codecId, version, buf.data(), buf.size());
        ASSERT_NE(nullptr, decoded) << "[OrderCodec] decode succeeded";

        EXPECT_EQ(FILLED_ORDSTATUS, decoded->status_)
            << "[OrderCodec] round-trip preserves FILLED status";
        EXPECT_EQ(500u, decoded->cumQty_)
            << "[OrderCodec] round-trip preserves cumQty";
        EXPECT_EQ(0u, decoded->leavesQty_)
            << "[OrderCodec] round-trip preserves leavesQty";
        EXPECT_EQ(SELL_SIDE, decoded->side_)
            << "[OrderCodec] round-trip preserves side";
        EXPECT_DOUBLE_EQ(1.2650, decoded->price_)
            << "[OrderCodec] round-trip preserves price";

        delete decoded;
    }

    // =========================================================================
    // TEARDOWN
    // =========================================================================
    transMgr->stop();
    taskMgr->waitUntilTransactionsFinished(5);
    inQueues->detach();
    transMgr->detach();

    taskMgr.reset();
    transMgr.reset();
    multiOut.reset();
    recordingOut.reset();
    countingOut.reset();
    inQueues.reset();
    orderBook.reset();
    gateway.reset();

    OrderStorage::destroy();
    IdTGenerator::destroy();
    SubscrMgr::SubscriptionMgr::destroy();
    WideDataStorage::destroy();
    TaskManager::destroy();
}

} // anonymous namespace

#endif // BUILD_FIX
