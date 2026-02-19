/**
 * Concurrent Order Processor library - Google Test
 *
 * Authors: dudleylane, Claude
 * Test Migration: 2026
 *
 * Copyright (C) 2026 dudleylane
 *
 * Distributed under the GNU General Public License (GPL).
 *
 * Migrated from testIntegral.cpp
 */

#include <gtest/gtest.h>
#include <memory>
#include <deque>
#include <atomic>

#include "TestAux.h"
#include "DataModelDef.h"
#include "SubscrManager.h"
#include "IdTGenerator.h"
#include "TransactionScope.h"
#include "StateMachineHelper.h"
#include "OrderStorage.h"
#include "QueuesDef.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "OrderBookImpl.h"
#include "TransactionMgr.h"
#include "Processor.h"
#include "Logger.h"
#include "TaskManager.h"
#include "StorageRecordDispatcher.h"
#include "FileStorage.h"
#include "ExchUtils.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::OrdState;
using namespace COP::SubscrMgr;
using namespace COP::ACID;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Tasks;
using test::DummyOrderSaver;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

std::unique_ptr<OrderEntry> createCorrectOrder(SourceIdT instr) {
    SourceIdT srcId, destId, accountId, clearingId, clOrdId, origClOrderID, execList;

    srcId = WideDataStorage::instance()->add(new StringT("CLNT"));
    destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));
    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
    clOrdId = WideDataStorage::instance()->add(clOrd.release());

    std::unique_ptr<AccountEntry> account(new AccountEntry());
    account->type_ = PRINCIPAL_ACCOUNTTYPE;
    account->firm_ = "ACTFirm";
    account->account_ = "ACT";
    account->id_ = IdT();
    accountId = WideDataStorage::instance()->add(account.release());

    std::unique_ptr<ClearingEntry> clearing(new ClearingEntry());
    clearing->firm_ = "CLRFirm";
    clearingId = WideDataStorage::instance()->add(clearing.release());

    std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
    execList = WideDataStorage::instance()->add(execLst.release());

    std::unique_ptr<OrderEntry> ptr(
        new OrderEntry(srcId, destId, clOrdId, origClOrderID, instr, accountId, clearingId, execList));

    ptr->creationTime_ = 100;
    ptr->lastUpdateTime_ = 115;
    ptr->expireTime_ = 175;
    ptr->settlDate_ = 225;
    ptr->price_ = 1.46;

    ptr->status_ = RECEIVEDNEW_ORDSTATUS;
    ptr->side_ = BUY_SIDE;
    ptr->ordType_ = MARKET_ORDERTYPE;
    ptr->tif_ = DAY_TIF;
    ptr->settlType_ = _3_SETTLTYPE;
    ptr->capacity_ = PRINCIPAL_CAPACITY;
    ptr->currency_ = USD_CURRENCY;
    ptr->orderQty_ = 77;
    ptr->leavesQty_ = 77;

    return ptr;
}

static int clOrderIdCounter = 0;

void assignClOrderId(OrderEntry* order) {
    char buf[64];
    snprintf(buf, sizeof(buf), "TestClOrderId_%d", ++clOrderIdCounter);
    std::string val(buf);
    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
    order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
}

class TestInQueueObserver : public InQueueProcessor {
public:
    bool process() override { return false; }

    void onEvent(const std::string& source, const OrderEvent& evnt) override {
        orders_.push_back({source, evnt});
    }
    void onEvent(const std::string& source, const OrderCancelEvent& evnt) override {
        orderCancels_.push_back({source, evnt});
    }
    void onEvent(const std::string& source, const OrderReplaceEvent& evnt) override {
        orderReplaces_.push_back({source, evnt});
    }
    void onEvent(const std::string& source, const OrderChangeStateEvent& evnt) override {
        orderStates_.push_back({source, evnt});
    }
    void onEvent(const std::string& source, const ProcessEvent& evnt) override {
        processes_.push_back({source, evnt});
    }
    void onEvent(const std::string& source, const TimerEvent& evnt) override {
        timers_.push_back({source, evnt});
    }

    std::deque<std::pair<std::string, OrderEvent>> orders_;
    std::deque<std::pair<std::string, OrderCancelEvent>> orderCancels_;
    std::deque<std::pair<std::string, OrderReplaceEvent>> orderReplaces_;
    std::deque<std::pair<std::string, OrderChangeStateEvent>> orderStates_;
    std::deque<std::pair<std::string, ProcessEvent>> processes_;
    std::deque<std::pair<std::string, TimerEvent>> timers_;
};

SourceIdT addInstrument(const std::string& name) {
    std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
    instr->symbol_ = name;
    instr->securityId_ = "AAA";
    instr->securityIdSource_ = "AAASrc";
    return WideDataStorage::instance()->add(instr.release());
}

class TestOutQueues : public Queues::OutQueues {
public:
    TestOutQueues() : events_(0) {}

    void push(const ExecReportEvent&, const std::string&) override {
        events_.fetch_add(1, std::memory_order_relaxed);
    }

    void push(const CancelRejectEvent&, const std::string&) override {
        events_.fetch_add(1, std::memory_order_relaxed);
    }

    void push(const BusinessRejectEvent&, const std::string&) override {
        events_.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int> events_;
};

} // namespace

// =============================================================================
// Integration Test Fixture
// =============================================================================

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Note: ExchLogger is created globally by TestMain.cpp
        WideDataStorage::create();
        SubscriptionMgr::create();
        IdTGenerator::create();
        OrderStorage::create();
        clOrderIdCounter = 0;
    }

    void TearDown() override {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        SubscriptionMgr::destroy();
        WideDataStorage::destroy();
        // Note: ExchLogger is destroyed globally by TestMain.cpp
    }
};

// =============================================================================
// Market Order Tests
// =============================================================================

TEST_F(IntegrationTest, MarketOrderNotMatched) {
    aux::ExchLogger::instance()->setDebugOn(true);
    aux::ExchLogger::instance()->setNoteOn(true);

    DummyOrderSaver saver;
    TestInQueueObserver obs;
    IncomingQueues inQueues;
    OutgoingQueues outQueues;

    OrderBookImpl books;
    OrderBookImpl::InstrumentsT instr;
    SourceIdT instrId1 = addInstrument("aaa");
    instr.insert(instrId1);
    SourceIdT instrId2 = addInstrument("bbb");
    instr.insert(instrId2);
    books.init(instr, &saver);

    TransactionMgrParams transParams(IdTGenerator::instance());
    TransactionMgr transMgr;
    transMgr.init(transParams);

    TaskManagerParams tmparams;
    {
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues,
            &outQueues, &inQueues, &transMgr);
        std::unique_ptr<Processor> proc(new Processor());
        proc->init(params);
        tmparams.evntProcessors_.push_back(proc.release());

        std::unique_ptr<Processor> proc1(new Processor());
        proc1->init(params);
        tmparams.transactProcessors_.push_back(proc1.release());

        tmparams.transactMgr_ = &transMgr;
        tmparams.inQueues_ = &inQueues;
    }
    TaskManager manager(tmparams);

    // Insert small limit order first
    std::unique_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
    RawDataEntry ordClOrdId = ord->clOrderId_.get();
    ord->ordType_ = LIMIT_ORDERTYPE;
    ord->orderQty_ = 5;
    ord->leavesQty_ = 5;
    ord->side_ = SELL_SIDE;
    inQueues.push("test", OrderEvent(ord.release()));

    // Wait until order accepted or rejected
    manager.waitUntilTransactionsFinished(5);
    OrderEntry* order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(NEW_ORDSTATUS, order->status_);

    // Then add market order
    std::unique_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
    assignClOrderId(ord2.get());
    ord2->orderQty_ = 50;
    ord2->leavesQty_ = 50;
    RawDataEntry ord2ClOrdId = ord2->clOrderId_.get();
    inQueues.push("test", OrderEvent(ord2.release()));

    // Wait until order canceled or rejected
    manager.waitUntilTransactionsFinished(5);
    order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(CANCELED_ORDSTATUS, order->status_);

    transMgr.stop();
}

// =============================================================================
// Order Book Tests
// =============================================================================

TEST_F(IntegrationTest, LimitOrderAccepted) {
    aux::ExchLogger::instance()->setDebugOn(false);
    aux::ExchLogger::instance()->setNoteOn(false);

    DummyOrderSaver saver;
    IncomingQueues inQueues;
    OutgoingQueues outQueues;

    OrderBookImpl books;
    OrderBookImpl::InstrumentsT instr;
    SourceIdT instrId1 = addInstrument("test_instr");
    instr.insert(instrId1);
    books.init(instr, &saver);

    TransactionMgrParams transParams(IdTGenerator::instance());
    TransactionMgr transMgr;
    transMgr.init(transParams);

    TaskManagerParams tmparams;
    {
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues,
            &outQueues, &inQueues, &transMgr);
        std::unique_ptr<Processor> proc(new Processor());
        proc->init(params);
        tmparams.evntProcessors_.push_back(proc.release());

        std::unique_ptr<Processor> proc1(new Processor());
        proc1->init(params);
        tmparams.transactProcessors_.push_back(proc1.release());

        tmparams.transactMgr_ = &transMgr;
        tmparams.inQueues_ = &inQueues;
    }
    TaskManager manager(tmparams);

    // Submit a limit order
    std::unique_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
    assignClOrderId(ord.get());
    ord->ordType_ = LIMIT_ORDERTYPE;
    ord->orderQty_ = 100;
    ord->leavesQty_ = 100;
    ord->side_ = BUY_SIDE;
    ord->price_ = 10.50;
    RawDataEntry clOrdId = ord->clOrderId_.get();
    inQueues.push("test", OrderEvent(ord.release()));

    // Wait for processing
    manager.waitUntilTransactionsFinished(5);

    OrderEntry* order = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(NEW_ORDSTATUS, order->status_);
    EXPECT_EQ(100, order->orderQty_);
    EXPECT_EQ(100, order->leavesQty_);
    EXPECT_EQ(0, order->cumQty_);

    transMgr.stop();
}

TEST_F(IntegrationTest, MatchingBuyAndSellOrders) {
    aux::ExchLogger::instance()->setDebugOn(false);
    aux::ExchLogger::instance()->setNoteOn(false);

    DummyOrderSaver saver;
    IncomingQueues inQueues;
    OutgoingQueues outQueues;

    OrderBookImpl books;
    OrderBookImpl::InstrumentsT instr;
    SourceIdT instrId1 = addInstrument("match_test");
    instr.insert(instrId1);
    books.init(instr, &saver);

    TransactionMgrParams transParams(IdTGenerator::instance());
    TransactionMgr transMgr;
    transMgr.init(transParams);

    TaskManagerParams tmparams;
    {
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues,
            &outQueues, &inQueues, &transMgr);
        std::unique_ptr<Processor> proc(new Processor());
        proc->init(params);
        tmparams.evntProcessors_.push_back(proc.release());

        std::unique_ptr<Processor> proc1(new Processor());
        proc1->init(params);
        tmparams.transactProcessors_.push_back(proc1.release());

        tmparams.transactMgr_ = &transMgr;
        tmparams.inQueues_ = &inQueues;
    }
    TaskManager manager(tmparams);

    // Submit a sell limit order
    std::unique_ptr<OrderEntry> sellOrd(createCorrectOrder(instrId1));
    assignClOrderId(sellOrd.get());
    sellOrd->ordType_ = LIMIT_ORDERTYPE;
    sellOrd->orderQty_ = 100;
    sellOrd->leavesQty_ = 100;
    sellOrd->side_ = SELL_SIDE;
    sellOrd->price_ = 10.00;
    RawDataEntry sellClOrdId = sellOrd->clOrderId_.get();
    inQueues.push("test", OrderEvent(sellOrd.release()));

    manager.waitUntilTransactionsFinished(5);

    // Verify sell order is on the book
    OrderEntry* sellOrder = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    ASSERT_NE(nullptr, sellOrder);
    EXPECT_EQ(NEW_ORDSTATUS, sellOrder->status_);

    // Submit a buy limit order at same price
    std::unique_ptr<OrderEntry> buyOrd(createCorrectOrder(instrId1));
    assignClOrderId(buyOrd.get());
    buyOrd->ordType_ = LIMIT_ORDERTYPE;
    buyOrd->orderQty_ = 100;
    buyOrd->leavesQty_ = 100;
    buyOrd->side_ = BUY_SIDE;
    buyOrd->price_ = 10.00;
    RawDataEntry buyClOrdId = buyOrd->clOrderId_.get();
    inQueues.push("test", OrderEvent(buyOrd.release()));

    manager.waitUntilTransactionsFinished(5);

    // Both orders should be filled
    sellOrder = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    OrderEntry* buyOrder = OrderStorage::instance()->locateByClOrderId(buyClOrdId);

    ASSERT_NE(nullptr, sellOrder);
    ASSERT_NE(nullptr, buyOrder);

    // Verify fills
    EXPECT_EQ(FILLED_ORDSTATUS, sellOrder->status_);
    EXPECT_EQ(FILLED_ORDSTATUS, buyOrder->status_);
    EXPECT_EQ(100, sellOrder->cumQty_);
    EXPECT_EQ(100, buyOrder->cumQty_);
    EXPECT_EQ(0, sellOrder->leavesQty_);
    EXPECT_EQ(0, buyOrder->leavesQty_);

    transMgr.stop();
}

// =============================================================================
// Multiple Instrument Tests
// =============================================================================

TEST_F(IntegrationTest, MultipleInstruments) {
    aux::ExchLogger::instance()->setDebugOn(false);
    aux::ExchLogger::instance()->setNoteOn(false);

    DummyOrderSaver saver;
    IncomingQueues inQueues;
    OutgoingQueues outQueues;

    OrderBookImpl books;
    OrderBookImpl::InstrumentsT instrSet;
    SourceIdT instrId1 = addInstrument("AAPL");
    SourceIdT instrId2 = addInstrument("GOOG");
    instrSet.insert(instrId1);
    instrSet.insert(instrId2);
    books.init(instrSet, &saver);

    TransactionMgrParams transParams(IdTGenerator::instance());
    TransactionMgr transMgr;
    transMgr.init(transParams);

    TaskManagerParams tmparams;
    {
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues,
            &outQueues, &inQueues, &transMgr);
        std::unique_ptr<Processor> proc(new Processor());
        proc->init(params);
        tmparams.evntProcessors_.push_back(proc.release());

        std::unique_ptr<Processor> proc1(new Processor());
        proc1->init(params);
        tmparams.transactProcessors_.push_back(proc1.release());

        tmparams.transactMgr_ = &transMgr;
        tmparams.inQueues_ = &inQueues;
    }
    TaskManager manager(tmparams);

    // Submit order for first instrument
    std::unique_ptr<OrderEntry> ord1(createCorrectOrder(instrId1));
    assignClOrderId(ord1.get());
    ord1->ordType_ = LIMIT_ORDERTYPE;
    ord1->orderQty_ = 50;
    ord1->leavesQty_ = 50;
    ord1->side_ = BUY_SIDE;
    ord1->price_ = 150.00;
    RawDataEntry clOrdId1 = ord1->clOrderId_.get();
    inQueues.push("test", OrderEvent(ord1.release()));

    // Submit order for second instrument
    std::unique_ptr<OrderEntry> ord2(createCorrectOrder(instrId2));
    assignClOrderId(ord2.get());
    ord2->ordType_ = LIMIT_ORDERTYPE;
    ord2->orderQty_ = 25;
    ord2->leavesQty_ = 25;
    ord2->side_ = SELL_SIDE;
    ord2->price_ = 2800.00;
    RawDataEntry clOrdId2 = ord2->clOrderId_.get();
    inQueues.push("test", OrderEvent(ord2.release()));

    manager.waitUntilTransactionsFinished(5);

    // Both orders should be on the book
    OrderEntry* order1 = OrderStorage::instance()->locateByClOrderId(clOrdId1);
    OrderEntry* order2 = OrderStorage::instance()->locateByClOrderId(clOrdId2);

    ASSERT_NE(nullptr, order1);
    ASSERT_NE(nullptr, order2);
    EXPECT_EQ(NEW_ORDSTATUS, order1->status_);
    EXPECT_EQ(NEW_ORDSTATUS, order2->status_);

    transMgr.stop();
}

// =============================================================================
// Queue Processing Tests
// =============================================================================

TEST_F(IntegrationTest, QueueProcessesMultipleOrders) {
    aux::ExchLogger::instance()->setDebugOn(false);
    aux::ExchLogger::instance()->setNoteOn(false);

    DummyOrderSaver saver;
    IncomingQueues inQueues;
    TestOutQueues outQueues;

    OrderBookImpl books;
    OrderBookImpl::InstrumentsT instrSet;
    SourceIdT instrId = addInstrument("queue_test");
    instrSet.insert(instrId);
    books.init(instrSet, &saver);

    TransactionMgrParams transParams(IdTGenerator::instance());
    TransactionMgr transMgr;
    transMgr.init(transParams);

    TaskManagerParams tmparams;
    {
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues,
            &outQueues, &inQueues, &transMgr);
        std::unique_ptr<Processor> proc(new Processor());
        proc->init(params);
        tmparams.evntProcessors_.push_back(proc.release());

        std::unique_ptr<Processor> proc1(new Processor());
        proc1->init(params);
        tmparams.transactProcessors_.push_back(proc1.release());

        tmparams.transactMgr_ = &transMgr;
        tmparams.inQueues_ = &inQueues;
    }
    TaskManager manager(tmparams);

    const int numOrders = 10;
    std::vector<RawDataEntry> clOrdIds;

    // Submit multiple orders
    for (int i = 0; i < numOrders; ++i) {
        std::unique_ptr<OrderEntry> ord(createCorrectOrder(instrId));
        assignClOrderId(ord.get());
        ord->ordType_ = LIMIT_ORDERTYPE;
        ord->orderQty_ = 100 + i;
        ord->leavesQty_ = 100 + i;
        ord->side_ = (i % 2 == 0) ? BUY_SIDE : SELL_SIDE;
        ord->price_ = 100.0 + i * 0.1;
        clOrdIds.push_back(ord->clOrderId_.get());
        inQueues.push("test", OrderEvent(ord.release()));
    }

    manager.waitUntilTransactionsFinished(10);

    // Verify all orders were processed
    for (const auto& clOrdId : clOrdIds) {
        OrderEntry* order = OrderStorage::instance()->locateByClOrderId(clOrdId);
        ASSERT_NE(nullptr, order);
        // Orders should be either NEW, FILLED, or PARTFILL depending on matching
        EXPECT_NE(RECEIVEDNEW_ORDSTATUS, order->status_);
    }

    // Should have generated execution reports
    EXPECT_GT(outQueues.events_.load(), 0);

    transMgr.stop();
}
