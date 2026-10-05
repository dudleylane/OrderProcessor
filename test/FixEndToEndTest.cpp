/**
 Concurrent Order Processor library - FIX End-to-End Test Suite

 Tests the complete FIX order flow:
   FIX NewOrderSingle → FixGateway → IncomingQueues → Processor → StateMachine
   → OrderMatcher → ExecutionReport → OutQueues (captured)

 Verifies order lifecycle, matching, fills, and source-string routing
 without requiring a live TCP FIX session.
*/

#ifdef BUILD_FIX

// QuickFIX headers MUST come before any <flat_map> include (see FixGateway.h)
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include "FixGateway.h"

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
#include "Processor.h"
#include "TaskManager.h"
#include "Logger.h"
#include "SubscrManager.h"

#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::ACID;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Tasks;
using namespace COP::App;
using test::DummyOrderSaver;

namespace
{

// =============================================================================
// Capturing OutQueues — records all outbound events for verification
// =============================================================================

struct CapturedExecReport
{
    IdT orderId;
    ExecType execType;
    OrderStatus orderStatus;
    std::string source;
};

class CapturingOutQueues : public Queues::OutQueues
{
public:
    void push(const ExecReportEvent &evnt, const std::string &target) override
    {
        std::lock_guard<std::mutex> lock(mtx_);
        CapturedExecReport r;
        r.orderId = evnt.exec_->orderId_;
        r.execType = evnt.exec_->type_;
        r.orderStatus = evnt.exec_->orderStatus_;
        r.source = target;
        reports_.push_back(r);
        totalEvents_.fetch_add(1, std::memory_order_relaxed);
    }

    void push(const CancelRejectEvent &evnt, const std::string &target) override
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            cancelRejects_.emplace_back(evnt, target);
        }
        totalEvents_.fetch_add(1, std::memory_order_relaxed);
    }

    void push(const BusinessRejectEvent &, const std::string &) override
    {
        totalEvents_.fetch_add(1, std::memory_order_relaxed);
    }

    void push(const OrderRejectEvent &evnt, const std::string &target) override
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            orderRejects_.emplace_back(evnt, target);
        }
        totalEvents_.fetch_add(1, std::memory_order_relaxed);
    }

    std::deque<std::pair<OrderRejectEvent, std::string>> orderRejects() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return orderRejects_;
    }

    std::deque<CapturedExecReport> reports() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return reports_;
    }

    std::deque<std::pair<CancelRejectEvent, std::string>> cancelRejects() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return cancelRejects_;
    }

    int totalEvents() const
    {
        return totalEvents_.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mtx_;
    std::deque<CapturedExecReport> reports_;
    std::deque<std::pair<CancelRejectEvent, std::string>> cancelRejects_;
    std::deque<std::pair<OrderRejectEvent, std::string>> orderRejects_;
    std::atomic<int> totalEvents_{ 0 };
};

// =============================================================================
// Helper: add instrument to WideDataStorage
// =============================================================================

SourceIdT addTestInstrument(const std::string &symbol)
{
    auto *instr = new InstrumentEntry();
    instr->symbol_ = symbol;
    instr->securityId_ = "SEC1";
    instr->securityIdSource_ = "SRC1";
    return WideDataStorage::instance()->add(instr);
}

// =============================================================================
// Test Fixture
// =============================================================================

class FixEndToEndTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        WideDataStorage::create();
        SubscrMgr::SubscriptionMgr::create();
        IdTGenerator::create();
        OrderStorage::create();

        instrId_ = addTestInstrument("EURUSD");

        // Set up test account and clearing (required by OrderEntry::isValid)
        auto *acct = new AccountEntry();
        acct->account_ = "TESTACCT";
        acct->firm_ = "TESTFIRM";
        acct->type_ = PRINCIPAL_ACCOUNTTYPE;
        WideDataStorage::instance()->add(acct);

        auto *clearing = new ClearingEntry();
        clearing->firm_ = "CLRFIRM";
        clearingId_ = WideDataStorage::instance()->add(clearing);

        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(instrId_);

        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments);

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<CapturingOutQueues>();

        TransactionMgrParams transParams(IdTGenerator::instance());
        transMgr_ = std::make_unique<TransactionMgr>();
        transMgr_->init(transParams);

        // Create processor pool
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                               outQueues_.get(), inQueues_.get(), transMgr_.get());

        TaskManagerParams tmparams;
        auto *evtProc = new Processor();
        evtProc->init(params);
        tmparams.evntProcessors_.push_back(evtProc);

        auto *trProc = new Processor();
        trProc->init(params);
        tmparams.transactProcessors_.push_back(trProc);

        tmparams.transactMgr_ = transMgr_.get();
        tmparams.inQueues_ = inQueues_.get();

        TaskManager::init(0);
        taskMgr_ = std::make_unique<TaskManager>(tmparams);

        // Create FIX gateway pointing at real IncomingQueues
        gateway_ = std::make_unique<FixGateway>(inQueues_.get(), WideDataStorage::instance(), OrderStorage::instance(),
                                                clearingId_);
    }

    void TearDown() override
    {
        // Stop the transaction manager only after ~TaskManager has waited for every task: processing an event
        // adds a transaction and finishing one removes it, and both require a started manager.
        taskMgr_->waitUntilTransactionsFinished(5);
        inQueues_->detach();
        transMgr_->detach();

        taskMgr_.reset();
        transMgr_->stop();
        transMgr_.reset();
        outQueues_.reset();
        inQueues_.reset();
        orderBook_.reset();
        gateway_.reset();

        OrderStorage::destroy();
        IdTGenerator::destroy();
        SubscrMgr::SubscriptionMgr::destroy();
        WideDataStorage::destroy();
        TaskManager::destroy();
    }

    // Helper: build a FIX NewOrderSingle
    FIX44::NewOrderSingle makeNOS(const std::string &clOrdId, char side, char ordType, double price, double qty,
                                  char tif = FIX::TimeInForce_DAY)
    {
        FIX::UtcTimeStamp now;
        FIX44::NewOrderSingle msg;
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(side));
        msg.set(FIX::TransactTime(now));
        msg.set(FIX::OrdType(ordType));
        msg.set(FIX::Symbol("EURUSD"));
        msg.set(FIX::OrderQty(qty));
        msg.set(FIX::TimeInForce(tif));
        if (ordType != FIX::OrdType_MARKET)
        {
            msg.set(FIX::Price(price));
        }
        msg.set(FIX::Account("TESTACCT"));
        msg.set(FIX::Currency("USD"));
        return msg;
    }

    // Helper: build a FIX OrderCancelReplaceRequest for a limit order
    FIX44::OrderCancelReplaceRequest makeReplace(const std::string &origClOrdId, const std::string &clOrdId, char side,
                                                 double price, double qty)
    {
        FIX44::OrderCancelReplaceRequest msg;
        msg.set(FIX::OrigClOrdID(origClOrdId));
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(side));
        msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        msg.set(FIX::OrdType(FIX::OrdType_LIMIT));
        msg.set(FIX::Symbol("EURUSD"));
        msg.set(FIX::Price(price));
        msg.set(FIX::OrderQty(qty));
        return msg;
    }

    void waitForProcessing()
    {
        taskMgr_->waitUntilTransactionsFinished(10);
    }

    /// The order sid placed with this ClOrdID, which is stored under the session's key (#71)
    OrderEntry *orderOf(const std::string &clOrdId, const FIX::SessionID &sid) const
    {
        const std::string key = FixGateway::sessionClOrdId(FixGateway::makeSourceString(sid), clOrdId);
        RawDataEntry rawKey(STRING_RAWDATATYPE, key.c_str(), static_cast<u32>(key.size()));
        return OrderStorage::instance()->locateByClOrderId(rawKey);
    }

    OrderEntry *orderOf(const std::string &clOrdId) const
    {
        return orderOf(clOrdId, fixSid_);
    }

    FIX44::OrderCancelRequest makeCancel(const std::string &origClOrdId, const std::string &clOrdId, char side)
    {
        FIX44::OrderCancelRequest msg;
        msg.set(FIX::OrigClOrdID(origClOrdId));
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(side));
        msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        msg.set(FIX::Symbol("EURUSD"));
        return msg;
    }

protected:
    SourceIdT instrId_;
    SourceIdT clearingId_;
    DummyOrderSaver saver_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<CapturingOutQueues> outQueues_;
    std::unique_ptr<TransactionMgr> transMgr_;
    std::unique_ptr<TaskManager> taskMgr_;
    std::unique_ptr<FixGateway> gateway_;

    const FIX::SessionID fixSid_{ "FIX.4.4", "TRADER_A", "ORDER_PROCESSOR", "" };
};

// =============================================================================
// End-to-End Tests
// =============================================================================

TEST_F(FixEndToEndTest, LimitOrder_Accepted)
{
    auto msg = makeNOS("FIX-001", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(msg, fixSid_);

    waitForProcessing();

    // Find the order by scanning storage
    OrderEntry *order = orderOf("FIX-001");
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(NEW_ORDSTATUS, order->status_);
    EXPECT_EQ(BUY_SIDE, order->side_);
    EXPECT_EQ(LIMIT_ORDERTYPE, order->ordType_);
    EXPECT_DOUBLE_EQ(1.0850, order->price_);
    EXPECT_EQ(100u, order->orderQty_);
    EXPECT_EQ(100u, order->leavesQty_);
    EXPECT_EQ(0u, order->cumQty_);

    // Order accepted — no exec report generated for simple acceptance
    // (exec reports are generated on fills, cancels, rejects)
}

TEST_F(FixEndToEndTest, TwoOrders_MatchAndFill)
{
    // 1. Submit sell limit via FIX
    auto sellMsg = makeNOS("FIX-SELL-001", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(sellMsg, fixSid_);
    waitForProcessing();

    OrderEntry *sellOrder = orderOf("FIX-SELL-001");
    ASSERT_NE(nullptr, sellOrder);
    EXPECT_EQ(NEW_ORDSTATUS, sellOrder->status_);

    // 2. Submit buy limit at same price via FIX
    auto buyMsg = makeNOS("FIX-BUY-001", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(buyMsg, fixSid_);
    waitForProcessing();

    OrderEntry *buyOrder = orderOf("FIX-BUY-001");
    ASSERT_NE(nullptr, buyOrder);

    // 3. Both should be filled
    EXPECT_EQ(FILLED_ORDSTATUS, sellOrder->status_);
    EXPECT_EQ(FILLED_ORDSTATUS, buyOrder->status_);
    EXPECT_EQ(100u, sellOrder->cumQty_);
    EXPECT_EQ(100u, buyOrder->cumQty_);
    EXPECT_EQ(0u, sellOrder->leavesQty_);
    EXPECT_EQ(0u, buyOrder->leavesQty_);

    // 4. Verify trade execution reports generated for both orders
    auto reports = outQueues_->reports();
    int tradeReports = 0;
    for (const auto &r : reports)
    {
        if (r.execType == TRADE_EXECTYPE)
        {
            tradeReports++;
        }
    }
    EXPECT_GE(tradeReports, 2) << "Expected at least 2 TRADE execution reports (one per side)";
}

TEST_F(FixEndToEndTest, PartialFill)
{
    // Sell 50 on book
    auto sellMsg = makeNOS("FIX-SELL-P1", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 50);
    gateway_->onMessage(sellMsg, fixSid_);
    waitForProcessing();

    // Buy 100 — should partially fill (50) and rest
    auto buyMsg = makeNOS("FIX-BUY-P1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(buyMsg, fixSid_);
    waitForProcessing();

    OrderEntry *sellOrder = orderOf("FIX-SELL-P1");
    OrderEntry *buyOrder = orderOf("FIX-BUY-P1");

    ASSERT_NE(nullptr, sellOrder);
    ASSERT_NE(nullptr, buyOrder);

    EXPECT_EQ(FILLED_ORDSTATUS, sellOrder->status_);
    EXPECT_EQ(PARTFILL_ORDSTATUS, buyOrder->status_);
    EXPECT_EQ(50u, buyOrder->cumQty_);
    EXPECT_EQ(50u, buyOrder->leavesQty_);
}

TEST_F(FixEndToEndTest, CancelOrder_ViaFix_Completes)
{
    // The bug this covers: a FIX cancel only reached Pending Cancel (39=6). The order stayed in the book, and could
    // still fill (#73).
    auto msg = makeNOS("FIX-CNL-001", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(msg, fixSid_);
    waitForProcessing();

    OrderEntry *order = orderOf("FIX-CNL-001");
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(NEW_ORDSTATUS, order->status_);

    FIX::UtcTimeStamp now;
    FIX44::OrderCancelRequest cancelMsg;
    cancelMsg.set(FIX::OrigClOrdID("FIX-CNL-001"));
    cancelMsg.set(FIX::ClOrdID("FIX-CNL-002"));
    cancelMsg.set(FIX::Side(FIX::Side_BUY));
    cancelMsg.set(FIX::TransactTime(now));
    cancelMsg.set(FIX::Symbol("EURUSD"));
    gateway_->onMessage(cancelMsg, fixSid_);
    waitForProcessing();

    EXPECT_EQ(CANCELED_ORDSTATUS, order->status_);
    EXPECT_EQ(0u, order->leavesQty_);
    int cancelReports = 0;
    for (const auto &r : outQueues_->reports())
    {
        if ((order->orderId_ == r.orderId) && (CANCEL_EXECTYPE == r.execType))
        {
            ++cancelReports;
            EXPECT_EQ(CANCELED_ORDSTATUS, r.orderStatus);
            EXPECT_EQ("FIX:TRADER_A->ORDER_PROCESSOR", r.source);
        }
    }
    EXPECT_EQ(1, cancelReports);
    EXPECT_TRUE(outQueues_->cancelRejects().empty());
}

TEST_F(FixEndToEndTest, CancelOfAFilledOrder_ViaFix_IsRefusedToTheSender)
{
    // A cancel that comes too late is refused, to the session that sent it, with the order's status (#73)
    gateway_->onMessage(makeNOS("FIX-CNL-S1", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    gateway_->onMessage(makeNOS("FIX-CNL-B1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    OrderEntry *sell = orderOf("FIX-CNL-S1");
    ASSERT_NE(nullptr, sell);
    ASSERT_EQ(FILLED_ORDSTATUS, sell->status_);

    FIX44::OrderCancelRequest cancelMsg;
    cancelMsg.set(FIX::OrigClOrdID("FIX-CNL-S1"));
    cancelMsg.set(FIX::ClOrdID("FIX-CNL-S2"));
    cancelMsg.set(FIX::Side(FIX::Side_SELL));
    cancelMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    cancelMsg.set(FIX::Symbol("EURUSD"));
    gateway_->onMessage(cancelMsg, fixSid_);
    waitForProcessing();

    auto rejects = outQueues_->cancelRejects();
    ASSERT_EQ(1u, rejects.size());
    EXPECT_EQ(sell->orderId_, rejects[0].first.id_);
    EXPECT_EQ(CancelRejectEvent::TOO_LATE, rejects[0].first.reason_);
    EXPECT_EQ(FILLED_ORDSTATUS, rejects[0].first.ordStatus_);
    EXPECT_EQ("FIX:TRADER_A->ORDER_PROCESSOR", rejects[0].second);
    EXPECT_EQ(FILLED_ORDSTATUS, sell->status_);
}

TEST_F(FixEndToEndTest, ReplaceOrder_ViaFix_Completes)
{
    // The bug this covers: a FIX replace kept the original's id and ClOrdID, so the engine refused it as a duplicate
    // ClOrdID (35=9 102=6), and a well-formed one would have crashed it (#74)
    gateway_->onMessage(makeNOS("FIX-RPL-001", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    OrderEntry *original = orderOf("FIX-RPL-001");
    ASSERT_NE(nullptr, original);
    ASSERT_EQ(NEW_ORDSTATUS, original->status_);

    gateway_->onMessage(makeReplace("FIX-RPL-001", "FIX-RPL-002", FIX::Side_BUY, 1.0840, 80), fixSid_);
    waitForProcessing();

    EXPECT_EQ(REPLACED_ORDSTATUS, original->status_);
    EXPECT_EQ(0u, original->leavesQty_);
    OrderEntry *replacement = orderOf("FIX-RPL-002");
    ASSERT_NE(nullptr, replacement);
    EXPECT_EQ(NEW_ORDSTATUS, replacement->status_);
    EXPECT_DOUBLE_EQ(1.0840, replacement->price_);
    EXPECT_EQ(80u, replacement->orderQty_);
    EXPECT_EQ(80u, replacement->leavesQty_);
    EXPECT_EQ(original->orderId_, replacement->origOrderId_);
    EXPECT_EQ("FIX:TRADER_A->ORDER_PROCESSOR", replacement->source_.get()); // its reports go to the same session
    int originalReplaced = 0;
    int replacementNew = 0;
    for (const auto &r : outQueues_->reports())
    {
        if (REPLACE_EXECTYPE == r.execType)
        {
            originalReplaced += ((original->orderId_ == r.orderId) && (REPLACED_ORDSTATUS == r.orderStatus)) ? 1 : 0;
            replacementNew += ((replacement->orderId_ == r.orderId) && (NEW_ORDSTATUS == r.orderStatus)) ? 1 : 0;
        }
    }
    EXPECT_EQ(1, originalReplaced);
    EXPECT_EQ(1, replacementNew);
    EXPECT_TRUE(outQueues_->orderRejects().empty());
}

TEST_F(FixEndToEndTest, ReplaceOfAFilledOrder_ViaFix_IsRefusedToTheSender)
{
    // A replace that comes too late is refused, to the session that sent it, with the order's status and the
    // request's ClOrdIDs (#74)
    gateway_->onMessage(makeNOS("FIX-RPL-S1", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    gateway_->onMessage(makeNOS("FIX-RPL-B1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    OrderEntry *sell = orderOf("FIX-RPL-S1");
    ASSERT_NE(nullptr, sell);
    ASSERT_EQ(FILLED_ORDSTATUS, sell->status_);

    gateway_->onMessage(makeReplace("FIX-RPL-S1", "FIX-RPL-S2", FIX::Side_SELL, 1.0860, 100), fixSid_);
    waitForProcessing();

    auto rejects = outQueues_->orderRejects();
    ASSERT_EQ(1u, rejects.size());
    EXPECT_TRUE(rejects[0].first.replacement_);
    EXPECT_EQ(CancelRejectEvent::TOO_LATE, rejects[0].first.refusal_);
    EXPECT_EQ(FILLED_ORDSTATUS, rejects[0].first.origStatus_);
    // The engine's event names the ClOrdIDs as they are stored, under the session's key; the session gets its own
    // (#71)
    const OrderRejectEvent shown = FixGateway::clientView(rejects[0].first, rejects[0].second);
    EXPECT_EQ("FIX-RPL-S2", shown.clOrderId_);
    EXPECT_EQ("FIX-RPL-S1", shown.origClOrderId_);
    EXPECT_EQ("FIX:TRADER_A->ORDER_PROCESSOR", rejects[0].second);
    EXPECT_EQ(FILLED_ORDSTATUS, sell->status_);
    EXPECT_EQ(nullptr, orderOf("FIX-RPL-S2"));
}

TEST_F(FixEndToEndTest, SourceStringPreserved)
{
    // Submit via FIX — verify order's source matches the FIX session
    auto msg = makeNOS("FIX-SRC-001", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(msg, fixSid_);
    waitForProcessing();

    OrderEntry *order = orderOf("FIX-SRC-001");
    ASSERT_NE(nullptr, order);

    std::string source = order->source_.get();
    EXPECT_EQ("FIX:TRADER_A->ORDER_PROCESSOR", source);
}

TEST_F(FixEndToEndTest, MarketOrder_FillsImmediately)
{
    // Place a resting sell limit
    auto sellMsg = makeNOS("FIX-MKT-S1", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 100);
    gateway_->onMessage(sellMsg, fixSid_);
    waitForProcessing();

    // Verify sell is on book before sending market
    OrderEntry *sellVerify = orderOf("FIX-MKT-S1");
    ASSERT_NE(nullptr, sellVerify);
    ASSERT_EQ(NEW_ORDSTATUS, sellVerify->status_);

    // Market buy — enters as NEW then matches via deferred events
    auto buyMsg = makeNOS("FIX-MKT-B1", FIX::Side_BUY, FIX::OrdType_MARKET, 0.0, 100);
    gateway_->onMessage(buyMsg, fixSid_);
    waitForProcessing();
    waitForProcessing();
    waitForProcessing(); // market orders chain: match → execution → cancel-if-unfilled

    OrderEntry *sellOrder = orderOf("FIX-MKT-S1");
    OrderEntry *buyOrder = orderOf("FIX-MKT-B1");

    ASSERT_NE(nullptr, sellOrder);
    ASSERT_NE(nullptr, buyOrder);

    // Market order was accepted and entered the pipeline
    // Full market fill lifecycle is async (deferred event chain) and tested
    // separately in IntegrationTest::MarketOrderNotMatched.
    // Here we verify the FIX gateway correctly translated and queued the order.
    EXPECT_EQ(MARKET_ORDERTYPE, buyOrder->ordType_);
    EXPECT_EQ(BUY_SIDE, buyOrder->side_);
    EXPECT_EQ(100u, buyOrder->orderQty_);
}

// =============================================================================
// ClOrdIDs belong to the session that sent them (#71)
// =============================================================================

TEST_F(FixEndToEndTest, TwoSessions_CanUseTheSameClOrdId)
{
    // FIX scopes a ClOrdID to the client that sent it. The engine refused the second order as a duplicate (#67), and
    // before that crashed on it.
    const FIX::SessionID otherSid{ "FIX.4.4", "TRADER_B", "ORDER_PROCESSOR", "" };
    gateway_->onMessage(makeNOS("SAME-1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0800, 100), fixSid_);
    waitForProcessing();
    gateway_->onMessage(makeNOS("SAME-1", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0900, 100), otherSid);
    waitForProcessing();

    OrderEntry *mine = orderOf("SAME-1");
    OrderEntry *theirs = orderOf("SAME-1", otherSid);
    ASSERT_NE(nullptr, mine);
    ASSERT_NE(nullptr, theirs);
    EXPECT_NE(mine, theirs);
    EXPECT_EQ(NEW_ORDSTATUS, mine->status_);
    EXPECT_EQ(NEW_ORDSTATUS, theirs->status_);
    EXPECT_TRUE(outQueues_->orderRejects().empty());

    // Each session's cancel finds its own order
    gateway_->onMessage(makeCancel("SAME-1", "SAME-2", FIX::Side_SELL), otherSid);
    waitForProcessing();
    EXPECT_EQ(CANCELED_ORDSTATUS, theirs->status_);
    EXPECT_EQ(NEW_ORDSTATUS, mine->status_);
}

TEST_F(FixEndToEndTest, AnotherSessionsCancelAndReplace_ViaFix_DoNotReachTheEngine)
{
    // Before #71 any session could cancel or replace an order whose ClOrdID it knew or guessed. The gateway answers
    // them as for an unknown order, and the engine never sees them.
    const FIX::SessionID otherSid{ "FIX.4.4", "TRADER_B", "ORDER_PROCESSOR", "" };
    gateway_->onMessage(makeNOS("MINE-1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0800, 100), fixSid_);
    waitForProcessing();
    OrderEntry *mine = orderOf("MINE-1");
    ASSERT_NE(nullptr, mine);
    ASSERT_EQ(NEW_ORDSTATUS, mine->status_);

    gateway_->onMessage(makeCancel("MINE-1", "THEIRS-1", FIX::Side_BUY), otherSid);
    gateway_->onMessage(makeReplace("MINE-1", "THEIRS-2", FIX::Side_BUY, 1.0700, 100), otherSid);
    waitForProcessing();

    EXPECT_EQ(NEW_ORDSTATUS, mine->status_);
    EXPECT_DOUBLE_EQ(1.0800, mine->price_);
    EXPECT_EQ(nullptr, orderOf("THEIRS-2", otherSid));
    EXPECT_TRUE(outQueues_->cancelRejects().empty());
    EXPECT_TRUE(outQueues_->orderRejects().empty());
}

} // anonymous namespace

#endif // BUILD_FIX
