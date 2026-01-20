/**
 * Concurrent Order Processor library - Google Test
 *
 * Author: Sergey Mikhailik
 * Test Migration: 2026
 *
 * Copyright (C) 2009-2026 Sergey Mikhailik
 *
 * Distributed under the GNU General Public License (GPL).
 *
 * Migrated from testStates.cpp
 */

#include <gtest/gtest.h>
#include <memory>

#include "OrderStates.h"
#include "TestAux.h"
#include "DataModelDef.h"
#include "SubscrManager.h"
#include "IdTGenerator.h"
#include "TransactionScope.h"
#include "StateMachineHelper.h"
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

std::unique_ptr<OrderEntry> createReplOrder() {
    SourceIdT srcId, destId, accountId, clearingId, instrument, clOrdId, origClOrderID, execList;

    srcId = WideDataStorage::instance()->add(new StringT("CLNT1"));
    destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));
    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestReplClOrderId", 17));
    clOrdId = WideDataStorage::instance()->add(clOrd.release());
    std::unique_ptr<RawDataEntry> origclOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
    origClOrderID = WideDataStorage::instance()->add(origclOrd.release());

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

    ptr->creationTime_ = 130;
    ptr->lastUpdateTime_ = 135;
    ptr->expireTime_ = 185;
    ptr->settlDate_ = 225;
    ptr->price_ = 2.46;

    ptr->status_ = RECEIVEDNEW_ORDSTATUS;
    ptr->side_ = BUY_SIDE;
    ptr->ordType_ = LIMIT_ORDERTYPE;
    ptr->tif_ = DAY_TIF;
    ptr->settlType_ = _3_SETTLTYPE;
    ptr->capacity_ = PRINCIPAL_CAPACITY;
    ptr->currency_ = USD_CURRENCY;
    ptr->orderQty_ = 300;

    return ptr;
}

static int clOrderIdCounter = 0;

void assignClOrderId(OrderEntry* order) {
    char buf[64];
    snprintf(buf, sizeof(buf), "TestClOrderId_%d", ++clOrderIdCounter);
    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, buf, static_cast<u32>(strlen(buf))));
    order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
}

void assignClOrderIdWithValue(OrderEntry* order, const std::string& val) {
    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
    order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
}

void assignOrigClOrderId(OrderEntry* order, const std::string& val) {
    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
    order->origClOrderId_ = WideDataStorage::instance()->add(clOrd.release());
}

} // namespace

// =============================================================================
// States Test Fixture
// =============================================================================

class StatesTest : public ::testing::Test {
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
// RcvdNew to New - onOrderReceived Tests
// =============================================================================

TEST_F(StatesTest, RcvdNew2New_OnOrderReceived_NewOrderProcessed) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    EXPECT_EQ(0u, order->orderId_.id_);
    EXPECT_EQ(0u, order->orderId_.date_);
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(recvevnt);
    p.checkStates("New", "NoCnlReplace");
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
    ASSERT_NE(nullptr, ord);
    EXPECT_TRUE(ord->orderId_.isValid());
    EXPECT_EQ(3u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
    EXPECT_EQ(NEW_ORDSTATUS, ord->status_);
}

TEST_F(StatesTest, RcvdNew2Rejected_OnOrderReceived_DuplicateClOrdId) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());

    // First order to create duplicate
    {
        TestTransactionContext setupTrCntxt;
        OrderStateWrapper setupP;
        std::unique_ptr<OrderEntry> setupOrder(createCorrectOrder());
        setupP.start();
        onOrderReceived setupEvnt;
        setupEvnt.order_ = setupOrder.get();
        setupEvnt.generator_ = IdTGenerator::instance();
        setupEvnt.transaction_ = &setupTrCntxt;
        setupEvnt.orderStorage_ = OrderStorage::instance();
        setupP.processEvent(setupEvnt);
    }

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);

    onOrderReceived recvevnt;
    recvevnt.order_ = order.get();
    recvevnt.generator_ = IdTGenerator::instance();
    recvevnt.transaction_ = &trCntxt;
    recvevnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(recvevnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(recvevnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

TEST_F(StatesTest, RcvdNew2Rejected_OnOrderReceived_InvalidSide) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

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
    EXPECT_FALSE(recvevnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

TEST_F(StatesTest, RcvdNew2Rejected_OnOrderReceived_EmptyClOrderId) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderIdWithValue(order.get(), "");

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
    EXPECT_FALSE(recvevnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

// =============================================================================
// RcvdNew to Rejected - onRecvOrderRejected Tests
// =============================================================================

TEST_F(StatesTest, RcvdNew2Rejected_OnRecvOrderRejected_NewOrderProcessed) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

    onRecvOrderRejected evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_EQ(REJECTED_ORDSTATUS, ord->status_);
}

TEST_F(StatesTest, RcvdNew2Rejected_OnRecvOrderRejected_EmptyClOrderId) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderIdWithValue(order.get(), "");

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onRecvOrderRejected evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
    EXPECT_EQ(REJECTED_ORDSTATUS, evnt.order_->status_);
}

TEST_F(StatesTest, RcvdNew2Rejected_OnRecvOrderRejected_InvalidValues) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onRecvOrderRejected evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();
    evnt.order_->side_ = INVALID_SIDE;

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

// =============================================================================
// RcvdNew to PendReplace - onRplOrderReceived Tests
// =============================================================================

TEST_F(StatesTest, RcvdNew2PendReplace_OnRplOrderReceived_CorrectOrderProcessed) {
    // First create an order to be replaced
    {
        TestTransactionContext setupTrCntxt;
        OrderStateWrapper setupP;
        std::unique_ptr<OrderEntry> setupOrder(createCorrectOrder());
        setupP.start();
        onOrderReceived setupEvnt;
        setupEvnt.order_ = setupOrder.get();
        setupEvnt.generator_ = IdTGenerator::instance();
        setupEvnt.transaction_ = &setupTrCntxt;
        setupEvnt.orderStorage_ = OrderStorage::instance();
        setupP.processEvent(setupEvnt);
    }

    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createReplOrder());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    EXPECT_FALSE(order->orderId_.isValid());
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

    onRplOrderReceived evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Pend_Replace", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_EQ(PENDINGREPLACE_ORDSTATUS, ord->status_);
}

TEST_F(StatesTest, RcvdNew2Rejected_OnRplOrderReceived_EmptyOrigClOrderId) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createReplOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onRplOrderReceived evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();
    evnt.order_->origClOrderId_ = IdT();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

TEST_F(StatesTest, RcvdNew2Rejected_OnRplOrderReceived_UnknownOrigClOrderId) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createReplOrder());
    assignClOrderId(order.get());
    assignOrigClOrderId(order.get(), "Unknown");

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onRplOrderReceived evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

// =============================================================================
// RcvdNew to Rejected - onRecvRplOrderRejected Tests
// =============================================================================

TEST_F(StatesTest, RcvdNew2Rejected_OnRecvRplOrderRejected_CorrectOrderProcessed) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createReplOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

    onRecvRplOrderRejected evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_EQ(REJECTED_ORDSTATUS, ord->status_);
}

// =============================================================================
// RcvdNew to Rejected - onExternalOrderRejected Tests
// =============================================================================

TEST_F(StatesTest, RcvdNew2Rejected_OnExternalOrderRejected_CorrectOrderProcessed) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

    onExternalOrderRejected evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_EQ(REJECTED_ORDSTATUS, ord->status_);
}

TEST_F(StatesTest, RcvdNew2Rejected_OnExternalOrderRejected_InvalidSide) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onExternalOrderRejected evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();
    evnt.order_->side_ = INVALID_SIDE;

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_FALSE(evnt.order_->orderId_.isValid());
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

// =============================================================================
// RcvdNew to New - onExternalOrder Tests
// =============================================================================

TEST_F(StatesTest, RcvdNew2New_OnExternalOrder_CorrectOrderProcessed) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");
    EXPECT_FALSE(order->orderId_.isValid());
    OrderEntry* ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    EXPECT_EQ(nullptr, ord);

    onExternalOrder evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("New", "NoCnlReplace");
    EXPECT_EQ(3u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
    EXPECT_TRUE(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
    ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
    ASSERT_NE(nullptr, ord);
    EXPECT_EQ(NEW_ORDSTATUS, ord->status_);
}

TEST_F(StatesTest, RcvdNew2Rejected_OnExternalOrder_InvalidSide) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderId(order.get());

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onExternalOrder evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();
    evnt.order_->side_ = INVALID_SIDE;

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
}

TEST_F(StatesTest, RcvdNew2Rejected_OnExternalOrder_EmptyClOrderId) {
    TestTransactionContext trCntxt;
    OrderStateWrapper p;
    std::unique_ptr<OrderEntry> order(createCorrectOrder());
    assignClOrderIdWithValue(order.get(), "");

    p.start();
    p.checkStates("Rcvd_New", "NoCnlReplace");

    onExternalOrder evnt;
    evnt.order_ = order.get();
    evnt.generator_ = IdTGenerator::instance();
    evnt.transaction_ = &trCntxt;
    evnt.orderStorage_ = OrderStorage::instance();

    p.processEvent(evnt);
    p.checkStates("Rejected", "NoCnlReplace");
    EXPECT_EQ(1u, trCntxt.op_.size());
    EXPECT_TRUE(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
    EXPECT_EQ(REJECTED_ORDSTATUS, evnt.order_->status_);
}
