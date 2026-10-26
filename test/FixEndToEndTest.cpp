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
#include <quickfix/fix50sp2/NewOrderSingle.h>
#include <quickfix/fix50sp2/OrderCancelRequest.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include "FixGateway.h"
#include "FixOutQueues.h"
#include "MultiOutQueues.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

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
    std::string workingIndicator; // WorkingIndicator (636) of the report a session gets, or "(not set)" (#387)
};

class CapturingOutQueues : public Queues::OutQueues
{
public:
    void push(const ExecReportEvent &evnt, const std::string &target) override
    {
        const std::string workingIndicator = workingIndicatorOf(evnt);
        std::lock_guard<std::mutex> lock(mtx_);
        CapturedExecReport r;
        r.orderId = evnt.exec_->orderId_;
        r.execType = evnt.exec_->type_;
        r.orderStatus = evnt.exec_->orderStatus_;
        r.source = target;
        r.workingIndicator = workingIndicator;
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
    /// WorkingIndicator (636) of the report a session gets for evnt, built as it goes out, from the order as the
    /// transaction that publishes it leaves it, as FixOutQueues has the gateway build one for a session that is logged on
    /// (#387). This pipeline's gateway has none, so builds nothing.
    static std::string workingIndicatorOf(const ExecReportEvent &evnt)
    {
        const OrderEntry *order = OrderStorage::instance()->locateByOrderId(evnt.exec_->orderId_);
        if ((nullptr == order) || !FixGateway::reportsOverFix(evnt.exec_))
        {
            return "(not sent)";
        }
        const FIX50SP2::ExecutionReport report =
            FixGateway::buildExecutionReport(evnt.exec_, *order, evnt.requestClOrdId_);
        return report.isSetField(FIX::FIELD::WorkingIndicator) ? report.getField(FIX::FIELD::WorkingIndicator)
                                                               : std::string("(not set)");
    }

    mutable std::mutex mtx_;
    std::deque<CapturedExecReport> reports_;
    std::deque<std::pair<CancelRejectEvent, std::string>> cancelRejects_;
    std::deque<std::pair<OrderRejectEvent, std::string>> orderRejects_;
    std::atomic<int> totalEvents_{ 0 };
};

// =============================================================================
// Holding InQueues: the engine's input, which a test can hold back (#380)
// =============================================================================

/// Passes what the gateway pushes on to the engine's queues, or, while held, keeps the new orders, cancels and replaces
/// until the test lets them through: an order held so is one the engine hasn't stored yet, as when no event worker has
/// reached it
class HoldingInQueues : public Queues::InQueues
{
public:
    explicit HoldingInQueues(Queues::InQueues *engine) : engine_(engine) {}
    ~HoldingInQueues() override
    {
        // Never reached the engine, which would have freed them
        for (Held &held : held_)
        {
            if (auto *order = std::get_if<OrderEvent>(&held.event))
            {
                delete order->order_;
            }
            else if (auto *replace = std::get_if<OrderReplaceEvent>(&held.event))
            {
                delete replace->replacementOrder_;
            }
        }
    }

    void hold()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        holding_ = true;
    }
    /// Passes on what is held, in the order it came, and holds nothing more
    void release()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        holding_ = false;
        for (Held &held : held_)
        {
            std::visit(
                [&](const auto &evnt)
                {
                    engine_->push(held.source, evnt);
                },
                held.event);
        }
        held_.clear();
    }
    /// How many events it holds now
    size_t held()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return held_.size();
    }

    void push(const std::string &source, const OrderEvent &evnt) override
    {
        pushOrHold(source, evnt);
    }
    void push(const std::string &source, const OrderCancelEvent &evnt) override
    {
        pushOrHold(source, evnt);
    }
    void push(const std::string &source, const OrderReplaceEvent &evnt) override
    {
        pushOrHold(source, evnt);
    }
    void push(const std::string &source, const OrderChangeStateEvent &evnt) override
    {
        engine_->push(source, evnt);
    }
    void push(const std::string &source, const ProcessEvent &evnt) override
    {
        engine_->push(source, evnt);
    }
    void push(const std::string &source, const TimerEvent &evnt) override
    {
        engine_->push(source, evnt);
    }

private:
    struct Held
    {
        std::string source;
        std::variant<OrderEvent, OrderCancelEvent, OrderReplaceEvent> event;
    };

    template <typename Event> void pushOrHold(const std::string &source, const Event &evnt)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (holding_)
        {
            held_.push_back(Held{ source, evnt });
            return;
        }
        engine_->push(source, evnt);
    }

    Queues::InQueues *engine_;
    std::mutex mtx_; // guards both, and keeps what is held ahead of what comes after it
    bool holding_ = false;
    std::vector<Held> held_;
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

/// A field's value, or "(not set)", so that a field that isn't there fails a comparison rather than throws
std::string fieldOf(const FIX::FieldMap &fields, int tag)
{
    return fields.isSetField(tag) ? fields.getField(tag) : "(not set)";
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
        holdingInQueues_ = std::make_unique<HoldingInQueues>(inQueues_.get());
        outQueues_ = std::make_unique<CapturingOutQueues>();

        // The FIX gateway, pushing to the engine's real IncomingQueues through the holding queues, and hearing the
        // engine's reports, as the server's does through MultiOutQueues (main.cpp): a request waiting for its order
        // learns from them that the order is stored or refused (#380). It sends nothing, having no session. A request
        // that waits is ended by its order's answer, then, never by the minute's timeout.
        gateway_ =
            std::make_unique<FixGateway>(holdingInQueues_.get(), WideDataStorage::instance(), OrderStorage::instance(),
                                         clearingId_, SourceIdT(), std::chrono::seconds(60));
        fixOutQueues_ = std::make_unique<FixOutQueues>(gateway_.get(), OrderStorage::instance());
        reports_ = std::make_unique<MultiOutQueues>();
        reports_->addDelegate(outQueues_.get());
        reports_->addDelegate(fixOutQueues_.get());

        TransactionMgrParams transParams(IdTGenerator::instance());
        transMgr_ = std::make_unique<TransactionMgr>();
        transMgr_->init(transParams);

        // Create processor pool
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                               reports_.get(), inQueues_.get(), transMgr_.get());

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
    }

    void TearDown() override
    {
        // Stop the transaction manager only after ~TaskManager has waited for every task: processing an event
        // adds a transaction and finishing one removes it, and both require a started manager.
        EXPECT_TRUE(taskMgr_->waitUntilTransactionsFinished(60)) << "the pool didn't finish before the teardown";
        inQueues_->detach();
        transMgr_->detach();

        taskMgr_.reset();
        transMgr_->stop();
        transMgr_.reset();
        reports_.reset();
        fixOutQueues_.reset();
        outQueues_.reset();
        holdingInQueues_.reset();
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
    FIX50SP2::NewOrderSingle makeNOS(const std::string &clOrdId, char side, char ordType, double price, double qty,
                                     char tif = FIX::TimeInForce_DAY)
    {
        FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
        FIX50SP2::NewOrderSingle msg;
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
    FIX50SP2::OrderCancelReplaceRequest makeReplace(const std::string &origClOrdId, const std::string &clOrdId,
                                                    char side, double price, double qty)
    {
        FIX50SP2::OrderCancelReplaceRequest msg;
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

    /// The gateway's answer to a status request from fixSid_ for its buy of EURUSD with this ClOrdID (#383)
    FIX50SP2::ExecutionReport statusOf(const std::string &clOrdId)
    {
        FIX50SP2::OrderStatusRequest msg;
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(FIX::Side_BUY));
        msg.set(FIX::Symbol("EURUSD"));
        return gateway_->answerOrderStatus(msg, fixSid_);
    }

    /// A timeout fails here, as a timeout, not later as a wrong order state (#301)
    void waitForProcessing()
    {
        EXPECT_TRUE(taskMgr_->waitUntilTransactionsFinished(60)) << "the pool didn't finish the messages' work";
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

    FIX50SP2::OrderCancelRequest makeCancel(const std::string &origClOrdId, const std::string &clOrdId, char side)
    {
        FIX50SP2::OrderCancelRequest msg;
        msg.set(FIX::OrigClOrdID(origClOrdId));
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(side));
        msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        msg.set(FIX::Symbol("EURUSD"));
        return msg;
    }

    /// Sends msg from fixSid_ on a thread of its own, as a session's thread would, while the engine's input is held, and
    /// lets the input through once the request is waiting for its order (#380). Returns whether it waited.
    template <typename Message> bool sendWhileHeld(const Message &msg)
    {
        std::thread client(
            [&]
            {
                gateway_->onMessage(msg, fixSid_);
            });
        bool waited = false;
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
             !waited && (std::chrono::steady_clock::now() < deadline);)
        {
            waited = (1u == gateway_->waitingRequests());
            if (!waited)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        holdingInQueues_->release();
        client.join();
        return waited;
    }

    /// The ExecTypes of the reports of the order, in the order they went out
    std::vector<ExecType> reportsOf(const OrderEntry &order) const
    {
        std::vector<ExecType> types;
        for (const auto &r : outQueues_->reports())
        {
            if (order.orderId_ == r.orderId)
            {
                types.push_back(r.execType);
            }
        }
        return types;
    }

protected:
    SourceIdT instrId_;
    SourceIdT clearingId_;
    DummyOrderSaver saver_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<HoldingInQueues> holdingInQueues_; // what the gateway pushes to, in front of inQueues_ (#380)
    std::unique_ptr<CapturingOutQueues> outQueues_;
    std::unique_ptr<FixOutQueues> fixOutQueues_; // the engine's reports, to the gateway
    std::unique_ptr<MultiOutQueues> reports_;    // the engine's reports, to both
    std::unique_ptr<TransactionMgr> transMgr_;
    std::unique_ptr<TaskManager> taskMgr_;
    std::unique_ptr<FixGateway> gateway_;

    const FIX::SessionID fixSid_{ "FIXT.1.1", "TRADER_A", "ORDER_PROCESSOR", "" };
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

    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX50SP2::OrderCancelRequest cancelMsg;
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

    FIX50SP2::OrderCancelRequest cancelMsg;
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
    const FIX::SessionID otherSid{ "FIXT.1.1", "TRADER_B", "ORDER_PROCESSOR", "" };
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
    const FIX::SessionID otherSid{ "FIXT.1.1", "TRADER_B", "ORDER_PROCESSOR", "" };
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

// =============================================================================
// A cancel or replace sent before its order is stored (#380)
// =============================================================================

TEST_F(FixEndToEndTest, CancelSentBeforeItsOrderIsStored_CancelsIt)
{
    // The gateway looked the order up as soon as the cancel came, and an order the engine hadn't stored yet was
    // unknown: the cancel was refused, and the order was then acknowledged and rested in the book (#380). EP284's
    // B.1.d and B.1.e cancel it.
    holdingInQueues_->hold();
    gateway_->onMessage(makeNOS("EARLY-1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    ASSERT_EQ(nullptr, orderOf("EARLY-1")); // the engine hasn't seen it
    EXPECT_TRUE(sendWhileHeld(makeCancel("EARLY-1", "EARLY-2", FIX::Side_BUY)));
    waitForProcessing();

    OrderEntry *order = orderOf("EARLY-1");
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(CANCELED_ORDSTATUS, order->status_);
    EXPECT_EQ(0u, order->leavesQty_);
    // Acknowledged, then canceled, as B.1.e's order crosses its cancel
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, CANCEL_EXECTYPE }), reportsOf(*order));
    EXPECT_TRUE(outQueues_->cancelRejects().empty());
    EXPECT_EQ(0u, gateway_->waitingRequests());
}

TEST_F(FixEndToEndTest, ReplaceSentBeforeItsOrderIsStored_ReplacesIt)
{
    holdingInQueues_->hold();
    gateway_->onMessage(makeNOS("EARLY-R1", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    ASSERT_EQ(nullptr, orderOf("EARLY-R1"));
    EXPECT_TRUE(sendWhileHeld(makeReplace("EARLY-R1", "EARLY-R2", FIX::Side_BUY, 1.0840, 80)));
    waitForProcessing();

    OrderEntry *original = orderOf("EARLY-R1");
    OrderEntry *replacement = orderOf("EARLY-R2");
    ASSERT_NE(nullptr, original);
    ASSERT_NE(nullptr, replacement);
    EXPECT_EQ(REPLACED_ORDSTATUS, original->status_);
    EXPECT_EQ(NEW_ORDSTATUS, replacement->status_);
    EXPECT_DOUBLE_EQ(1.0840, replacement->price_);
    EXPECT_EQ(80u, replacement->leavesQty_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, REPLACE_EXECTYPE }), reportsOf(*original));
    EXPECT_TRUE(outQueues_->orderRejects().empty());
}

TEST_F(FixEndToEndTest, ReplaceOfAReplacementNotYetStored_IsDecidedAgainstIt)
{
    // EP284's D.2.a: Replace (Y,X), then at once Replace (Z,Y), naming Y "on an 'optimistic' basis". The second waits
    // for the engine to store Y, and is then decided against it: both are processed, in sequence.
    gateway_->onMessage(makeNOS("CHAIN-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    ASSERT_NE(nullptr, orderOf("CHAIN-X"));

    holdingInQueues_->hold();
    gateway_->onMessage(makeReplace("CHAIN-X", "CHAIN-Y", FIX::Side_BUY, 1.0840, 80), fixSid_);
    ASSERT_EQ(nullptr, orderOf("CHAIN-Y"));
    EXPECT_TRUE(sendWhileHeld(makeReplace("CHAIN-Y", "CHAIN-Z", FIX::Side_BUY, 1.0830, 70)));
    waitForProcessing();

    OrderEntry *x = orderOf("CHAIN-X");
    OrderEntry *y = orderOf("CHAIN-Y");
    OrderEntry *z = orderOf("CHAIN-Z");
    ASSERT_NE(nullptr, y);
    ASSERT_NE(nullptr, z);
    EXPECT_EQ(REPLACED_ORDSTATUS, x->status_);
    EXPECT_EQ(REPLACED_ORDSTATUS, y->status_);
    EXPECT_EQ(NEW_ORDSTATUS, z->status_);
    EXPECT_DOUBLE_EQ(1.0830, z->price_);
    EXPECT_EQ(70u, z->leavesQty_);
    EXPECT_EQ(y->orderId_, z->origOrderId_);
    EXPECT_TRUE(outQueues_->orderRejects().empty());
}

TEST_F(FixEndToEndTest, ReplaceOfARefusedReplacement_IsDecidedAgainstTheOrderItWasToReplace)
{
    // EP284's D.2.c: Replace (Y,X) of a filled order is refused, and Replace (Z,Y), sent at once, waits for that
    // refusal rather than the timeout, and is then decided against X, the order Y was to replace (#398): the engine
    // refuses it too, as too late, naming X. Before #398 the gateway answered it as unknown, and the engine never saw it.
    gateway_->onMessage(makeNOS("REF-S", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    gateway_->onMessage(makeNOS("REF-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    OrderEntry *x = orderOf("REF-X");
    ASSERT_NE(nullptr, x);
    ASSERT_EQ(FILLED_ORDSTATUS, x->status_);

    holdingInQueues_->hold();
    gateway_->onMessage(makeReplace("REF-X", "REF-Y", FIX::Side_BUY, 1.0840, 100), fixSid_);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(sendWhileHeld(makeReplace("REF-Y", "REF-Z", FIX::Side_BUY, 1.0830, 100)));
    // Ended by Y's refusal: the fixture's gateway would wait a minute
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30));
    waitForProcessing();

    const auto rejects = outQueues_->orderRejects();
    ASSERT_EQ(2u, rejects.size()); // Y's, then Z's
    const OrderRejectEvent y = FixGateway::clientView(rejects[0].first, rejects[0].second);
    const OrderRejectEvent z = FixGateway::clientView(rejects[1].first, rejects[1].second);
    EXPECT_EQ("REF-Y", y.clOrderId_);
    EXPECT_EQ("REF-X", y.origClOrderId_);
    EXPECT_EQ("REF-Z", z.clOrderId_);
    EXPECT_EQ("REF-X", z.origClOrderId_) << "decided against X, not Y";
    EXPECT_TRUE(z.replacement_);
    EXPECT_EQ(CancelRejectEvent::TOO_LATE, z.refusal_);
    EXPECT_EQ(FILLED_ORDSTATUS, z.origStatus_);
    EXPECT_EQ(nullptr, orderOf("REF-Y"));
    EXPECT_EQ(nullptr, orderOf("REF-Z"));
    EXPECT_EQ(FILLED_ORDSTATUS, x->status_);
    EXPECT_EQ(0u, gateway_->waitingRequests());
}

TEST_F(FixEndToEndTest, ReplaceOfARefusedReplacement_CanReplaceTheOrderItWasToReplace)
{
    // Decided against X, the replace that names a refused replacement is decided as any replace of X is (#398):
    // Replace (Y,X) changes the side, which the engine refuses, and Replace (Z,Y), sent at once, waits for that
    // refusal and then replaces X. The change it asked for is made, where before #398 it was answered as unknown.
    gateway_->onMessage(makeNOS("RRR-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0840, 100), fixSid_);
    waitForProcessing();
    OrderEntry *x = orderOf("RRR-X");
    ASSERT_NE(nullptr, x);
    ASSERT_EQ(NEW_ORDSTATUS, x->status_);

    holdingInQueues_->hold();
    gateway_->onMessage(makeReplace("RRR-X", "RRR-Y", FIX::Side_SELL, 1.0840, 100), fixSid_);
    EXPECT_TRUE(sendWhileHeld(makeReplace("RRR-Y", "RRR-Z", FIX::Side_BUY, 1.0830, 80)));
    waitForProcessing();

    const auto rejects = outQueues_->orderRejects();
    ASSERT_EQ(1u, rejects.size()); // Y's alone
    const OrderRejectEvent y = FixGateway::clientView(rejects[0].first, rejects[0].second);
    EXPECT_EQ("RRR-Y", y.clOrderId_);
    EXPECT_EQ(CancelRejectEvent::OTHER, y.refusal_);
    EXPECT_EQ(nullptr, orderOf("RRR-Y"));
    OrderEntry *z = orderOf("RRR-Z");
    ASSERT_NE(nullptr, z);
    EXPECT_EQ(REPLACED_ORDSTATUS, x->status_);
    EXPECT_EQ(NEW_ORDSTATUS, z->status_);
    EXPECT_EQ(x->orderId_, z->origOrderId_);
    EXPECT_DOUBLE_EQ(1.0830, z->price_);
    EXPECT_EQ(80u, z->leavesQty_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, REPLACE_EXECTYPE }), reportsOf(*x));
}

TEST_F(FixEndToEndTest, CancelOfARefusedReplacement_CancelsTheOrderItWasToReplace)
{
    // A cancel that names a refused replacement is decided against the order it was to replace, as a replace is
    // (#398): OrigClOrdID is "the last non rejected" ClOrdID for both. Replace (Y,X) is refused, and a cancel that
    // names Y, sent at once, waits for that refusal and then cancels X.
    gateway_->onMessage(makeNOS("CRR-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0840, 100), fixSid_);
    waitForProcessing();
    OrderEntry *x = orderOf("CRR-X");
    ASSERT_NE(nullptr, x);
    ASSERT_EQ(NEW_ORDSTATUS, x->status_);

    holdingInQueues_->hold();
    gateway_->onMessage(makeReplace("CRR-X", "CRR-Y", FIX::Side_SELL, 1.0840, 100), fixSid_);
    EXPECT_TRUE(sendWhileHeld(makeCancel("CRR-Y", "CRR-C", FIX::Side_BUY)));
    waitForProcessing();

    const auto rejects = outQueues_->orderRejects();
    ASSERT_EQ(1u, rejects.size()); // Y's
    EXPECT_EQ("CRR-Y", FixGateway::clientView(rejects[0].first, rejects[0].second).clOrderId_);
    EXPECT_EQ(nullptr, orderOf("CRR-Y"));
    EXPECT_EQ(CANCELED_ORDSTATUS, x->status_);
    EXPECT_EQ(0u, x->leavesQty_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, CANCEL_EXECTYPE }), reportsOf(*x));
    EXPECT_TRUE(outQueues_->cancelRejects().empty());
}

TEST_F(FixEndToEndTest, StatusRequests_FollowAnOrderThroughItsLife)
{
    // EP284's G.1.c through the engine (#383): Pending New until the order is acknowledged, then New and Partially
    // Filled, each with the order's quantities; then, after Replace (Y,X), a request for X is answered with Y, naming
    // X in OrigClOrdID (41), and one for Y with Y. The scenario replaces a filled order to raise its quantity, which the
    // engine refuses (#74), so here the order is replaced while partly filled.
    holdingInQueues_->hold();
    gateway_->onMessage(makeNOS("LIFE-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    const FIX50SP2::ExecutionReport pendingNew = statusOf("LIFE-X");
    holdingInQueues_->release();
    waitForProcessing();
    const FIX50SP2::ExecutionReport acknowledged = statusOf("LIFE-X");
    gateway_->onMessage(makeNOS("LIFE-S", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 40), fixSid_);
    waitForProcessing();
    const FIX50SP2::ExecutionReport partlyFilled = statusOf("LIFE-X");
    gateway_->onMessage(makeReplace("LIFE-X", "LIFE-Y", FIX::Side_BUY, 1.0850, 120), fixSid_);
    waitForProcessing();
    const FIX50SP2::ExecutionReport ofX = statusOf("LIFE-X");
    const FIX50SP2::ExecutionReport ofY = statusOf("LIFE-Y");

    OrderEntry *x = orderOf("LIFE-X");
    OrderEntry *y = orderOf("LIFE-Y");
    ASSERT_NE(nullptr, x);
    ASSERT_NE(nullptr, y);
    ASSERT_EQ(REPLACED_ORDSTATUS, x->status_);
    const std::string xId = std::to_string(x->orderId_.id_);
    const std::string yId = std::to_string(y->orderId_.id_);
    struct Expected
    {
        const FIX50SP2::ExecutionReport &answer;
        char ordStatus;
        std::string orderId, clOrdId, origClOrdId, orderQty, cumQty, leavesQty;
    };
    for (const Expected &e :
         { Expected{ pendingNew, FIX::OrdStatus_PENDING_NEW, "NONE", "LIFE-X", "(not set)", "100", "0", "100" },
           Expected{ acknowledged, FIX::OrdStatus_NEW, xId, "LIFE-X", "(not set)", "100", "0", "100" },
           Expected{ partlyFilled, FIX::OrdStatus_PARTIALLY_FILLED, xId, "LIFE-X", "(not set)", "100", "40", "60" },
           Expected{ ofX, FIX::OrdStatus_PARTIALLY_FILLED, yId, "LIFE-Y", "LIFE-X", "120", "40", "80" },
           Expected{ ofY, FIX::OrdStatus_PARTIALLY_FILLED, yId, "LIFE-Y", "(not set)", "120", "40", "80" } })
    {
        SCOPED_TRACE(e.clOrdId + " " + std::string(1, e.ordStatus));
        EXPECT_EQ(std::string(1, FIX::ExecType_ORDER_STATUS), fieldOf(e.answer, FIX::FIELD::ExecType));
        EXPECT_EQ("0", fieldOf(e.answer, FIX::FIELD::ExecID));
        EXPECT_EQ(std::string(1, e.ordStatus), fieldOf(e.answer, FIX::FIELD::OrdStatus));
        EXPECT_EQ(e.orderId, fieldOf(e.answer, FIX::FIELD::OrderID));
        EXPECT_EQ(e.clOrdId, fieldOf(e.answer, FIX::FIELD::ClOrdID));
        EXPECT_EQ(e.origClOrdId, fieldOf(e.answer, FIX::FIELD::OrigClOrdID));
        EXPECT_EQ(e.orderQty, fieldOf(e.answer, FIX::FIELD::OrderQty));
        EXPECT_EQ(e.cumQty, fieldOf(e.answer, FIX::FIELD::CumQty));
        EXPECT_EQ(e.leavesQty, fieldOf(e.answer, FIX::FIELD::LeavesQty));
        EXPECT_EQ("EURUSD", fieldOf(e.answer, FIX::FIELD::Symbol));
    }
}

TEST_F(FixEndToEndTest, DuplicateClOrdId_IsRefusedBeforeTheEngine)
{
    // EP284's F.1.a through the engine (#382): X acknowledged and partly filled, then a new order with X's ClOrdID. The
    // gateway refuses it itself and never hands it on: the engine's input, held, holds nothing, and the engine refuses
    // nothing. A status request for X meanwhile is answered with X's state, where the repeat's ClOrdID had looked
    // unanswered until the engine refused it, and X was Pending New (#383).
    gateway_->onMessage(makeNOS("DUP-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    gateway_->onMessage(makeNOS("DUP-S", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 40), fixSid_);
    waitForProcessing();
    holdingInQueues_->hold();
    gateway_->onMessage(makeNOS("DUP-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 150), fixSid_);
    EXPECT_EQ(0u, holdingInQueues_->held());
    const FIX50SP2::ExecutionReport status = statusOf("DUP-X");
    holdingInQueues_->release();
    waitForProcessing();

    OrderEntry *x = orderOf("DUP-X");
    ASSERT_NE(nullptr, x);
    EXPECT_EQ(std::string(1, FIX::ExecType_ORDER_STATUS), fieldOf(status, FIX::FIELD::ExecType));
    EXPECT_EQ(std::string(1, FIX::OrdStatus_PARTIALLY_FILLED), fieldOf(status, FIX::FIELD::OrdStatus));
    EXPECT_EQ(std::to_string(x->orderId_.id_), fieldOf(status, FIX::FIELD::OrderID));
    EXPECT_EQ("100", fieldOf(status, FIX::FIELD::OrderQty));
    EXPECT_EQ("40", fieldOf(status, FIX::FIELD::CumQty));
    EXPECT_EQ("60", fieldOf(status, FIX::FIELD::LeavesQty));
    EXPECT_TRUE(outQueues_->orderRejects().empty());
    EXPECT_EQ(PARTFILL_ORDSTATUS, x->status_);
    EXPECT_EQ(100u, x->orderQty_);
    EXPECT_EQ(60u, x->leavesQty_);
}

TEST_F(FixEndToEndTest, ReplaceWithTheClOrdIdOfAnOrderOnItsWay_IsRefusedBeforeTheEngine)
{
    // Replace (K,X) while a new order K is still on its way, held in the engine's input. The gateway refuses the replace
    // itself, as a ClOrdID in use (102=6), and never hands it on (#403). Before, both reached the engine, and with two
    // or more event processors the replacement could be stored first and the new order refused. Here K is stored and X
    // is untouched, and the engine refuses nothing. The gateway's own refusal goes to a session, which this fixture
    // has none of: FixtSessionTest.AReplaceWhoseClOrdIdIsInUseIsRefusedBeforeTheEngine reads it.
    gateway_->onMessage(makeNOS("RIU-X", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    holdingInQueues_->hold();
    gateway_->onMessage(makeNOS("RIU-K", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0840, 50), fixSid_);
    gateway_->onMessage(makeReplace("RIU-X", "RIU-K", FIX::Side_BUY, 1.0830, 80), fixSid_);
    EXPECT_EQ(1u, holdingInQueues_->held()) << "the new order alone";
    holdingInQueues_->release();
    waitForProcessing();

    EXPECT_TRUE(outQueues_->orderRejects().empty());
    OrderEntry *x = orderOf("RIU-X");
    ASSERT_NE(nullptr, x);
    EXPECT_EQ(NEW_ORDSTATUS, x->status_);
    EXPECT_DOUBLE_EQ(1.0850, x->price_);
    EXPECT_EQ(100u, x->orderQty_);
    OrderEntry *k = orderOf("RIU-K");
    ASSERT_NE(nullptr, k);
    EXPECT_EQ(NEW_ORDSTATUS, k->status_);
    EXPECT_DOUBLE_EQ(1.0840, k->price_);
    EXPECT_EQ(50u, k->orderQty_);
    EXPECT_FALSE(k->origOrderId_.isValid()) << "the new order, not a replacement";
}

// =============================================================================
// WorkingIndicator (636), as EP284's E.1.f sends it (#387)
// =============================================================================

TEST_F(FixEndToEndTest, StopOrder_IsNotWorkingUntilATradeReleasesIt)
{
    // A buy stop at 1.0850 is held off the book, and acknowledged with WorkingIndicator N; a trade at 1.0850 reaches
    // it, and its release (150=L) says Y; it then buys at the market. The trade's buy is held back until the stop's
    // acknowledgement is out, then let through, so that the release follows the acknowledgement at once: each report
    // says what its order was when it went out, not what the order is by the time the test reads it.
    gateway_->onMessage(makeNOS("WI-S", FIX::Side_SELL, FIX::OrdType_LIMIT, 1.0850, 100), fixSid_);
    waitForProcessing();
    auto stopMsg = makeNOS("WI-STOP", FIX::Side_BUY, FIX::OrdType_STOP, 0.0, 50);
    stopMsg.removeField(FIX::FIELD::Price);
    stopMsg.set(FIX::StopPx(1.0850));
    gateway_->onMessage(stopMsg, fixSid_);
    holdingInQueues_->hold();
    gateway_->onMessage(makeNOS("WI-B", FIX::Side_BUY, FIX::OrdType_LIMIT, 1.0850, 10), fixSid_);
    bool acknowledged = false;
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
         !acknowledged && (std::chrono::steady_clock::now() < deadline);)
    {
        const OrderEntry *stop = orderOf("WI-STOP");
        acknowledged = (nullptr != stop) && !reportsOf(*stop).empty();
        if (!acknowledged)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    holdingInQueues_->release();
    waitForProcessing();
    ASSERT_TRUE(acknowledged) << "the stop wasn't acknowledged";

    OrderEntry *stop = orderOf("WI-STOP");
    OrderEntry *sell = orderOf("WI-S");
    OrderEntry *buy = orderOf("WI-B");
    ASSERT_NE(nullptr, stop);
    ASSERT_NE(nullptr, sell);
    ASSERT_NE(nullptr, buy);
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_EQ(FILLED_ORDSTATUS, stop->status_);
    EXPECT_EQ(40u, sell->leavesQty_);
    // Each order's reports, with the WorkingIndicator each went out with
    auto reportsWithIndicator = [&](const OrderEntry &order)
    {
        std::vector<std::pair<ExecType, std::string>> found;
        for (const auto &r : outQueues_->reports())
        {
            if (order.orderId_ == r.orderId)
            {
                found.emplace_back(r.execType, r.workingIndicator);
            }
        }
        return found;
    };
    using Reports = std::vector<std::pair<ExecType, std::string>>;
    EXPECT_EQ((Reports{ { NEW_EXECTYPE, "N" }, { TRIGGERED_EXECTYPE, "Y" }, { TRADE_EXECTYPE, "(not set)" } }),
              reportsWithIndicator(*stop));
    // Booked at once
    EXPECT_EQ((Reports{ { NEW_EXECTYPE, "Y" }, { TRADE_EXECTYPE, "(not set)" } }), reportsWithIndicator(*buy));
    EXPECT_EQ((Reports{ { NEW_EXECTYPE, "Y" }, { TRADE_EXECTYPE, "(not set)" }, { TRADE_EXECTYPE, "(not set)" } }),
              reportsWithIndicator(*sell));
}

} // anonymous namespace

#endif // BUILD_FIX
