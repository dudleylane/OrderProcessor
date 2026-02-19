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
 * Migrated from testStateMachine.cpp (3,709 lines)
 * Tests state machine transitions for order lifecycle
 */

#include <gtest/gtest.h>
#include <memory>

#include "StateMachine.h"
#include "StateMachineHelper.h"
#include "TestAux.h"
#include "DataModelDef.h"
#include "SubscrManager.h"
#include "IdTGenerator.h"
#include "TransactionScope.h"
#include "OrderStorage.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::OrdState;
using namespace COP::SubscrMgr;
using namespace COP::ACID;
using test::DummyOrderSaver;
using test::TestTransactionContext;
using test::OrderStateWrapper;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

class TestOrderBook : public OrderBook {
public:
    void add(const OrderEntry&) override {}
    void remove(const OrderEntry&) override {}
    IdT find(const OrderFunctor&) const override { return IdT(); }
    void findAll(const OrderFunctor&, OrdersT*) const override {}
    IdT getTop(const SourceIdT&, const Side&) const override { return IdT(); }
    void restore(const OrderEntry&) override {}
};

std::unique_ptr<OrderEntry> createCorrectOrder() {
    SourceIdT srcId, destId, accountId, clearingId, instrument, clOrdId, origClOrderID, execList;

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

    std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
    instr->symbol_ = "AAASymbl";
    instr->securityId_ = "AAA";
    instr->securityIdSource_ = "AAASrc";
    instrument = WideDataStorage::instance()->add(instr.release());

    std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
    execList = WideDataStorage::instance()->add(execLst.release());

    std::unique_ptr<OrderEntry> ptr(
        new OrderEntry(srcId, destId, clOrdId, origClOrderID, instrument, accountId, clearingId, execList));

    ptr->creationTime_ = 100;
    ptr->lastUpdateTime_ = 115;
    ptr->expireTime_ = 175;
    ptr->settlDate_ = 225;
    ptr->price_ = 1.46;

    ptr->status_ = RECEIVEDNEW_ORDSTATUS;
    ptr->side_ = BUY_SIDE;
    ptr->ordType_ = LIMIT_ORDERTYPE;
    ptr->tif_ = DAY_TIF;
    ptr->settlType_ = _3_SETTLTYPE;
    ptr->capacity_ = PRINCIPAL_CAPACITY;
    ptr->currency_ = USD_CURRENCY;
    ptr->orderQty_ = 77;

    return ptr;
}

std::unique_ptr<TradeExecEntry> createTradeExec(const OrderEntry& order, const IdT& execId) {
    std::unique_ptr<TradeExecEntry> ptr(new TradeExecEntry);

    ptr->execId_ = execId;
    ptr->orderId_ = order.orderId_;

    ptr->type_ = TRADE_EXECTYPE;
    ptr->orderStatus_ = PARTFILL_ORDSTATUS;
    ptr->transactTime_ = 1;

    ptr->lastQty_ = 100;
    ptr->lastPx_ = 10.25;
    ptr->currency_ = order.currency_;
    ptr->tradeDate_ = 1;

    OrderStorage::instance()->save(ptr.get());
    return ptr;
}

std::unique_ptr<ExecCorrectExecEntry> createCorrectExec(const OrderEntry& order, const IdT& execId) {
    std::unique_ptr<ExecCorrectExecEntry> ptr(new ExecCorrectExecEntry);

    ptr->execRefId_ = IdT();
    ptr->execId_ = execId;
    ptr->orderId_ = order.orderId_;

    ptr->type_ = TRADE_EXECTYPE;
    ptr->orderStatus_ = PARTFILL_ORDSTATUS;
    ptr->transactTime_ = 1;

    ptr->cumQty_ = 30;
    ptr->leavesQty_ = 20;
    ptr->lastQty_ = 10;
    ptr->lastPx_ = 10.34;
    ptr->currency_ = order.currency_;
    ptr->tradeDate_ = 1;

    OrderStorage::instance()->save(ptr.get());
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

} // namespace

// =============================================================================
// State Machine Test Fixture
// =============================================================================

class StateMachineTest : public ::testing::Test {
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
// Basic State Transitions - Received New to Rejected
// =============================================================================

TEST_F(StateMachineTest, RcvdNew_To_Rejected_OnInvalidSide) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    recvevnt.order_->side_ = INVALID_SIDE;

    p.processEvent(recvevnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_EQ(CREATE_REJECT_EXECREPORT_TROPERATION, trCntxt.op_.front()->type());
}

// =============================================================================
// Full Order Lifecycle - New to Filled
// =============================================================================

TEST_F(StateMachineTest, FullLifecycle_New_PartFill_Filled) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    TestOrderBook books;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    // Transition: Rcvd_New -> New
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    recvevnt.order_->side_ = BUY_SIDE;

    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    EXPECT_FALSE(recvevnt.order_->orderId_.isValid());
    EXPECT_EQ(3u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_TRUE(ord->orderId_.isValid());

    // Transition: New -> PartFill (first trade)
    std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(101, 5)));
    onTradeExecution tradeevnt(tradeParams1.get());
    tradeevnt.orderId_ = order->orderId_;
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    ord->leavesQty_ = 100;
    ord->orderQty_ = 100;
    tradeevnt.generator_ = IdTGenerator::instance();
    tradeevnt.transaction_ = &trCntxt;
    tradeevnt.orderStorage_ = OrderStorage::instance();
    tradeParams1->lastQty_ = 50;

    p.processEvent(tradeevnt);
    p.checkStates("PartFill", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
    trCntxt.clear();

    // Transition: PartFill -> PartFill (second trade)
    tradeevnt.orderId_ = order->orderId_;
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    ord->leavesQty_ = 50;
    tradeevnt.generator_ = IdTGenerator::instance();
    tradeevnt.transaction_ = &trCntxt;
    tradeevnt.orderStorage_ = OrderStorage::instance();
    tradeParams1->lastQty_ = 20;

    p.processEvent(tradeevnt);
    p.checkStates("PartFill", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
    trCntxt.clear();

    // Transition: PartFill -> Filled (final trade)
    tradeevnt.orderId_ = order->orderId_;
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    ord->leavesQty_ = 30;
    tradeevnt.generator_ = IdTGenerator::instance();
    tradeevnt.transaction_ = &trCntxt;
    tradeevnt.orderStorage_ = OrderStorage::instance();
    tradeParams1->lastQty_ = 30;
    tradeevnt.orderBook_ = &books;

    p.processEvent(tradeevnt);
    p.checkStates("Filled", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
}

// =============================================================================
// Trade Correction Tests
// =============================================================================

TEST_F(StateMachineTest, Filled_To_PartFill_OnTradeCrctCncl) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    TestOrderBook books;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    // Step 1: Get to New state via onOrderReceived
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.order_->side_ = BUY_SIDE;
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    ASSERT_TRUE(ord->orderId_.isValid());

    // Step 2: First partial fill (100 -> 50 leaves)
    std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(101, 5)));
    onTradeExecution tradeevnt1(tradeParams1.get());
    tradeevnt1.orderId_ = ord->orderId_;
    tradeevnt1.generator_ = IdTGenerator::instance();
    tradeevnt1.transaction_ = &trCntxt;
    tradeevnt1.orderStorage_ = OrderStorage::instance();
    ord->leavesQty_ = 100;
    ord->orderQty_ = 100;
    tradeParams1->lastQty_ = 50;
    p.processEvent(tradeevnt1);
    p.checkStates("PartFill", "NoCnlReplace");
    trCntxt.clear();

    // Step 3: Second partial fill (50 -> 0 leaves = Filled)
    ord->leavesQty_ = 50;
    tradeParams1->lastQty_ = 50;
    tradeevnt1.orderBook_ = &books;
    p.processEvent(tradeevnt1);
    p.checkStates("Filled", "NoCnlReplace");
    trCntxt.clear();

    // Step 4: Trade correction that reduces cumQty, restoring leavesQty -> PartFill
    ord->leavesQty_ = 0;
    std::unique_ptr<ExecCorrectExecEntry> correctParams(createCorrectExec(*order.get(), IdT(100, 5)));
    onTradeCrctCncl tradeCrctevnt(correctParams.get());
    tradeCrctevnt.orderId_ = ord->orderId_;
    tradeCrctevnt.generator_ = IdTGenerator::instance();
    tradeCrctevnt.transaction_ = &trCntxt;
    tradeCrctevnt.orderStorage_ = OrderStorage::instance();
    correctParams->lastQty_ = 20;
    correctParams->cumQty_ = ord->cumQty_;
    correctParams->leavesQty_ = 10;

    p.processEvent(tradeCrctevnt);
    p.checkStates("PartFill", "NoCnlReplace");
    EXPECT_GE(trCntxt.op_.size(), 1u);
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
}

// =============================================================================
// Expiration Tests
// =============================================================================

TEST_F(StateMachineTest, New_To_Expired_OnExpired) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();

    // Get to New state
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_TRUE(ord->orderId_.isValid());

    // Apply expiration
    onExpired expire;
    expire.orderId_ = ord->orderId_;
    expire.generator_ = IdTGenerator::instance();
    expire.transaction_ = &trCntxt;
    expire.orderStorage_ = OrderStorage::instance();

    p.processEvent(expire);
    p.checkStates("Expired", "NoCnlReplace");
    EXPECT_EQ(2u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
}

// =============================================================================
// Rejection Tests
// =============================================================================

TEST_F(StateMachineTest, New_To_Rejected_OnOrderRejected) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();

    // Get to New state
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    // Apply rejection
    onOrderRejected reject;
    reject.orderId_ = ord->orderId_;
    reject.generator_ = IdTGenerator::instance();
    reject.transaction_ = &trCntxt;
    reject.orderStorage_ = OrderStorage::instance();

    p.processEvent(reject);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_EQ(2u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
}

// =============================================================================
// DoneForDay / NewDay Tests
// =============================================================================

TEST_F(StateMachineTest, New_To_DoneForDay_OnFinished) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();

    // Get to New state
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    // Apply finish for day
    onFinished finish;
    finish.orderId_ = ord->orderId_;
    finish.generator_ = IdTGenerator::instance();
    finish.transaction_ = &trCntxt;
    finish.orderStorage_ = OrderStorage::instance();

    p.processEvent(finish);
    p.checkStates("DoneForDay", "NoCnlReplace");
}

// =============================================================================
// Suspended / Continue Tests
// =============================================================================

TEST_F(StateMachineTest, New_To_Suspended_OnSuspended) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();

    // Get to New state
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    // Apply suspend
    onSuspended suspend;
    suspend.orderId_ = ord->orderId_;
    suspend.generator_ = IdTGenerator::instance();
    suspend.transaction_ = &trCntxt;
    suspend.orderStorage_ = OrderStorage::instance();

    p.processEvent(suspend);
    p.checkStates("Suspended", "NoCnlReplace");
}

// =============================================================================
// Cancel/Replace Sub-State Machine Tests
// =============================================================================

TEST_F(StateMachineTest, NoCnlReplace_To_GoingCancel_OnCancelReceived) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();

    // Get to New state
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    // Receive cancel request
    onCancelReceived cancel;
    cancel.orderId_ = ord->orderId_;
    cancel.generator_ = IdTGenerator::instance();
    cancel.transaction_ = &trCntxt;
    cancel.orderStorage_ = OrderStorage::instance();

    p.processEvent(cancel);
    p.checkStates("New", "GoingCancel");
}

TEST_F(StateMachineTest, NoCnlReplace_To_GoingReplace_OnReplaceReceived) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();

    // Get to New state
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    // Receive replace request - create replacement order ID
    IdT replId = IdTGenerator::instance()->getId();
    onReplaceReceived replace(replId);
    replace.orderId_ = ord->orderId_;
    replace.generator_ = IdTGenerator::instance();
    replace.transaction_ = &trCntxt;
    replace.orderStorage_ = OrderStorage::instance();

    p.processEvent(replace);
    p.checkStates("New", "GoingReplace");
}

// =============================================================================
// Multiple State Transition Test (Complex Lifecycle)
// =============================================================================

TEST_F(StateMachineTest, ComplexLifecycle_WithCorrections) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    TestOrderBook books;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    assignClOrderId(order.get());
    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    // Step 1: Rcvd_New -> New
    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();
    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    trCntxt.clear();

    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    // Step 2: New -> PartFill
    std::unique_ptr<TradeExecEntry> tradeParams(createTradeExec(*order.get(), IdT(101, 5)));
    onTradeExecution tradeevnt(tradeParams.get());
    tradeevnt.orderId_ = ord->orderId_;
    ord->leavesQty_ = 100;
    ord->orderQty_ = 100;
    tradeevnt.generator_ = IdTGenerator::instance();
    tradeevnt.transaction_ = &trCntxt;
    tradeevnt.orderStorage_ = OrderStorage::instance();
    tradeParams->lastQty_ = 50;

    p.processEvent(tradeevnt);
    p.checkStates("PartFill", "NoCnlReplace");
    trCntxt.clear();

    // Step 3: PartFill -> Filled
    ord->leavesQty_ = 50;
    tradeParams->lastQty_ = 50;
    tradeevnt.orderBook_ = &books;

    p.processEvent(tradeevnt);
    p.checkStates("Filled", "NoCnlReplace");
    trCntxt.clear();

    // Step 4: Filled -> New (full correction)
    std::unique_ptr<ExecCorrectExecEntry> correctParams(createCorrectExec(*order.get(), IdT(100, 5)));
    onTradeCrctCncl tradeCrctevnt(correctParams.get());
    tradeCrctevnt.orderId_ = ord->orderId_;
    tradeCrctevnt.generator_ = IdTGenerator::instance();
    tradeCrctevnt.transaction_ = &trCntxt;
    tradeCrctevnt.orderStorage_ = OrderStorage::instance();
    correctParams->lastQty_ = 0;
    correctParams->cumQty_ = 0;
    correctParams->leavesQty_ = ord->orderQty_;

    p.processEvent(tradeCrctevnt);
    p.checkStates("New", "NoCnlReplace");
    EXPECT_EQ(3u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
    trCntxt.clear();

    // Step 5: New -> Rejected
    onOrderRejected reject;
    reject.orderId_ = ord->orderId_;
    reject.generator_ = IdTGenerator::instance();
    reject.transaction_ = &trCntxt;
    reject.orderStorage_ = OrderStorage::instance();

    p.processEvent(reject);
    p.checkStates("Rejected", "NoCnlReplace");
}

// =============================================================================
// State Validation Tests
// =============================================================================

TEST_F(StateMachineTest, StateWrapper_InitialState) {
    OrderStateWrapper p;
    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
}

TEST_F(StateMachineTest, MultipleOrders_IndependentStateTracking) {
    TestTransactionContext trCntxt1, trCntxt2;
    OrderStateWrapper p1, p2;

    std::unique_ptr<OrderEntry> order1(createCorrectOrder());
    std::unique_ptr<OrderEntry> order2(createCorrectOrder());
    assignClOrderId(order1.get());
    assignClOrderId(order2.get());

    p1.start();
    p2.start();

    // Both start in Rcvd_New
    p1.checkStates("Rcvd_New", "NoCnlReplace");
    p2.checkStates("Rcvd_New", "NoCnlReplace");

    // Process first order only
    onOrderReceived recvevnt1;
    recvevnt1.order_ = order1.get();
    recvevnt1.generator_ = IdTGenerator::instance();
    recvevnt1.transaction_ = &trCntxt1;
    recvevnt1.orderStorage_ = OrderStorage::instance();
    p1.processEvent(recvevnt1);

    // First order is New, second still Rcvd_New
    p1.checkStates("New", "NoCnlReplace");
    p2.checkStates("Rcvd_New", "NoCnlReplace");

    // Process second order with rejection
    onOrderReceived recvevnt2;
    recvevnt2.order_ = order2.get();
    recvevnt2.generator_ = IdTGenerator::instance();
    recvevnt2.transaction_ = &trCntxt2;
    recvevnt2.orderStorage_ = OrderStorage::instance();
    recvevnt2.order_->side_ = INVALID_SIDE;
    p2.processEvent(recvevnt2);

    // First still New, second is Rejected
    p1.checkStates("New", "NoCnlReplace");
    p2.checkStates("Rejected", "NoCnlReplace");
}
