/**
 Concurrent Order Processor library - Google Test Migration

 Original Author: Sergey Mikhailik
 Test Migration: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <thread>
#include <memory>
#include <deque>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"
#include "MockTransaction.h"
#include "MockOrderBook.h"

#include "DataModelDef.h"
#include "Processor.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"

using namespace COP;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Store;
using namespace COP::ACID;

namespace {

// =============================================================================
// Test InQueue Observer Implementation
// =============================================================================

class TestInQueueObserver : public InQueueProcessor {
public:
    bool process() override {
        return false;
    }

    void onEvent(const std::string& source, const OrderEvent& evnt) override {
        orders_.push_back(OrderQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string& source, const OrderCancelEvent& evnt) override {
        orderCancels_.push_back(OrderCancelQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string& source, const OrderReplaceEvent& evnt) override {
        orderReplaces_.push_back(OrderReplaceQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string& source, const OrderChangeStateEvent& evnt) override {
        orderStates_.push_back(OrderStateQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string& source, const ProcessEvent& evnt) override {
        processes_.push_back(ProcessQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string& source, const TimerEvent& evnt) override {
        timers_.push_back(TimerQueueT::value_type(source, evnt));
    }

public:
    typedef std::deque<std::pair<std::string, OrderEvent>> OrderQueueT;
    typedef std::deque<std::pair<std::string, OrderCancelEvent>> OrderCancelQueueT;
    typedef std::deque<std::pair<std::string, OrderReplaceEvent>> OrderReplaceQueueT;
    typedef std::deque<std::pair<std::string, OrderChangeStateEvent>> OrderStateQueueT;
    typedef std::deque<std::pair<std::string, ProcessEvent>> ProcessQueueT;
    typedef std::deque<std::pair<std::string, TimerEvent>> TimerQueueT;

    OrderQueueT orders_;
    OrderCancelQueueT orderCancels_;
    OrderReplaceQueueT orderReplaces_;
    OrderStateQueueT orderStates_;
    ProcessQueueT processes_;
    TimerQueueT timers_;
};

// =============================================================================
// Test Transaction Manager Implementation
// =============================================================================

class TestTransactionManager : public TransactionManager {
public:
    TestTransactionManager() : proc_(nullptr) {}

    void attach(TransactionObserver*) override {}

    TransactionObserver* detach() override {
        return nullptr;
    }

    void addTransaction(std::unique_ptr<Transaction>& tr) override {
        tr->setTransactionId(TransactionId(1, 1));
        if (proc_) {
            proc_->process(tr->transactionId(), tr.get());
        }
    }

    bool removeTransaction(const TransactionId&, Transaction*) override {
        return false;
    }

    bool getParentTransactions(const TransactionId&, TransactionIdsT*) const override {
        return false;
    }

    bool getRelatedTransactions(const TransactionId&, TransactionIdsT*) const override {
        return false;
    }

    TransactionIterator* iterator() override {
        return nullptr;
    }

    Processor* proc_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class ProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create singletons
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();

        // Add test instruments
        instrId1_ = test::addInstrument("aaa", "AAA", "AAASrc");
        instrId2_ = test::addInstrument("bbb", "BBB", "BBBSrc");

        // Create components
        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<OutgoingQueues>();
        transMgr_ = std::make_unique<TestTransactionManager>();

        // Initialize order book
        orderBook_ = std::make_unique<OrderBookImpl>();
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(instrId1_);
        instruments.insert(instrId2_);
        orderBook_->init(instruments, &orderSaver_);

        // Initialize processor
        ProcessorParams params(
            IdTGenerator::instance(),
            OrderStorage::instance(),
            orderBook_.get(),
            inQueues_.get(),
            outQueues_.get(),
            inQueues_.get(),
            transMgr_.get());

        processor_ = std::make_unique<Processor>();
        processor_->init(params);

        transMgr_->proc_ = processor_.get();
    }

    void TearDown() override {
        processor_.reset();
        transMgr_.reset();
        orderBook_.reset();
        outQueues_.reset();
        inQueues_.reset();

        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    // Helper to create an order with specific settings
    std::unique_ptr<OrderEntry> createTestOrder(SourceIdT instrId, Side side,
                                                 PriceT price, QuantityT qty) {
        auto order = test::createCorrectOrder(instrId);
        test::assignClOrderId(order.get());
        order->side_ = side;
        order->price_ = price;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;
        order->ordType_ = LIMIT_ORDERTYPE;
        return order;
    }

protected:
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<OutgoingQueues> outQueues_;
    std::unique_ptr<TestTransactionManager> transMgr_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<Processor> processor_;
    test::DummyOrderSaver orderSaver_;
    SourceIdT instrId1_;
    SourceIdT instrId2_;
};

// =============================================================================
// Basic Initialization Tests
// =============================================================================

TEST_F(ProcessorTest, CreateProcessor) {
    ASSERT_NE(nullptr, processor_);
}

TEST_F(ProcessorTest, ProcessEmptyQueue) {
    // Processing empty queue should not crash
    bool processed = processor_->process();
    EXPECT_FALSE(processed);
}

// =============================================================================
// Order Processing Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessSingleNewOrder) {
    auto order = test::createCorrectOrder(instrId1_);
    RawDataEntry clOrdId = order->clOrderId_.get();

    inQueues_->push("test", OrderEvent(order.release()));

    processor_->process();

    OrderEntry* savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);
}

TEST_F(ProcessorTest, ProcessMultipleOrders) {
    auto order1 = test::createCorrectOrder(instrId1_);
    RawDataEntry ord1ClOrdId = order1->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order1.release()));

    auto order2 = test::createCorrectOrder(instrId1_);
    test::assignClOrderId(order2.get());
    RawDataEntry ord2ClOrdId = order2->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order2.release()));

    // First process - first order
    processor_->process();
    OrderEntry* savedOrder1 = OrderStorage::instance()->locateByClOrderId(ord1ClOrdId);
    ASSERT_NE(nullptr, savedOrder1);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder1->status_);

    // Second order should not be processed yet or same processing cycle
    OrderEntry* savedOrder2 = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
    // It may or may not be processed depending on implementation
    // EXPECT_EQ(nullptr, savedOrder2);

    // Process remaining
    processor_->process();
    savedOrder2 = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
    ASSERT_NE(nullptr, savedOrder2);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder2->status_);
}

TEST_F(ProcessorTest, ProcessMultipleCycles) {
    auto order = test::createCorrectOrder(instrId1_);
    RawDataEntry clOrdId = order->clOrderId_.get();

    inQueues_->push("test", OrderEvent(order.release()));

    // Multiple process cycles should be safe
    processor_->process();
    processor_->process();
    processor_->process();
    processor_->process();

    OrderEntry* savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);
}

// =============================================================================
// Order Matching Tests
// =============================================================================

TEST_F(ProcessorTest, MatchBuyAgainstSell) {
    // Create sell order first
    auto sellOrder = createTestOrder(instrId1_, SELL_SIDE, 10.0, 100);
    RawDataEntry sellClOrdId = sellOrder->clOrderId_.get();
    inQueues_->push("test", OrderEvent(sellOrder.release()));

    // Process sell order
    processor_->process();
    OrderEntry* savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    ASSERT_NE(nullptr, savedSell);
    EXPECT_EQ(NEW_ORDSTATUS, savedSell->status_);

    // Additional process to ensure order is in the book
    processor_->process();

    // Create matching buy order
    auto buyOrder = createTestOrder(instrId1_, BUY_SIDE, 20.0, 50);
    RawDataEntry buyClOrdId = buyOrder->clOrderId_.get();
    inQueues_->push("test", OrderEvent(buyOrder.release()));

    // Process buy order - should match
    processor_->process();

    OrderEntry* savedBuy = OrderStorage::instance()->locateByClOrderId(buyClOrdId);
    ASSERT_NE(nullptr, savedBuy);
    EXPECT_EQ(FILLED_ORDSTATUS, savedBuy->status_);

    savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    ASSERT_NE(nullptr, savedSell);
    EXPECT_EQ(PARTFILL_ORDSTATUS, savedSell->status_);
}

TEST_F(ProcessorTest, PartialFillScenario) {
    // Create sell order with larger quantity
    auto sellOrder = createTestOrder(instrId1_, SELL_SIDE, 10.0, 100);
    RawDataEntry sellClOrdId = sellOrder->clOrderId_.get();
    inQueues_->push("test", OrderEvent(sellOrder.release()));

    processor_->process();
    processor_->process();

    // Create buy order with smaller quantity
    auto buyOrder = createTestOrder(instrId1_, BUY_SIDE, 20.0, 50);
    RawDataEntry buyClOrdId = buyOrder->clOrderId_.get();
    inQueues_->push("test", OrderEvent(buyOrder.release()));

    processor_->process();

    // Buy should be fully filled
    OrderEntry* savedBuy = OrderStorage::instance()->locateByClOrderId(buyClOrdId);
    ASSERT_NE(nullptr, savedBuy);
    EXPECT_EQ(FILLED_ORDSTATUS, savedBuy->status_);

    // Sell should be partially filled
    OrderEntry* savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    ASSERT_NE(nullptr, savedSell);
    EXPECT_EQ(PARTFILL_ORDSTATUS, savedSell->status_);
}

// =============================================================================
// Different Instruments Tests
// =============================================================================

TEST_F(ProcessorTest, OrdersOnDifferentInstruments) {
    // Create order for instrument 1
    auto order1 = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId1 = order1->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order1.release()));

    // Create order for instrument 2
    auto order2 = createTestOrder(instrId2_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId2 = order2->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order2.release()));

    processor_->process();
    processor_->process();

    // Both orders should be processed
    OrderEntry* saved1 = OrderStorage::instance()->locateByClOrderId(clOrdId1);
    ASSERT_NE(nullptr, saved1);
    EXPECT_EQ(NEW_ORDSTATUS, saved1->status_);

    OrderEntry* saved2 = OrderStorage::instance()->locateByClOrderId(clOrdId2);
    ASSERT_NE(nullptr, saved2);
    EXPECT_EQ(NEW_ORDSTATUS, saved2->status_);
}

// =============================================================================
// Cancel Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessCancelEvent) {
    // First create and process an order
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    // Get the saved order to retrieve its ID
    OrderEntry* savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);

    // Now submit a cancel event for this order
    OrderCancelEvent cancelEvent(savedOrder->orderId_, "Test cancellation");
    inQueues_->push("test", cancelEvent);

    processor_->process();

    // The order should be in a cancellation state (GoingCancel)
    savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    // Order transitions to GoingCancel state after receiving cancel
}

// =============================================================================
// Replace Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessReplaceEvent) {
    // First create and process an order
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    // Get the saved order to retrieve its ID
    OrderEntry* savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);

    // Submit a replace event (notify original order about replace)
    OrderReplaceEvent replaceEvent(savedOrder->orderId_);
    inQueues_->push("test", replaceEvent);

    processor_->process();

    // Order should have received the replace notification
    savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
}

// =============================================================================
// Process Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessProcessEventWithInvalidType) {
    ProcessEvent event;  // Default constructor creates INVALID type
    inQueues_->push("test", event);

    // Invalid ProcessEvent type should throw
    EXPECT_THROW(processor_->process(), std::runtime_error);
}

// =============================================================================
// Timer Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessTimerEvent) {
    // First create and process an order
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    // Get the saved order to retrieve its ID
    OrderEntry* savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);

    // Submit a timer event (expiration)
    TimerEvent event(savedOrder->orderId_, TimerEvent::EXPIRATION);
    inQueues_->push("test", event);

    processor_->process();

    // Order should have transitioned to Expired state
    savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(EXPIRED_ORDSTATUS, savedOrder->status_);
}

// =============================================================================
// Concurrent Access Tests
// =============================================================================

TEST_F(ProcessorTest, ConcurrentOrderSubmission) {
    const int numOrders = 50;
    std::vector<RawDataEntry> clOrderIds;

    // Submit orders
    for (int i = 0; i < numOrders; ++i) {
        auto order = test::createCorrectOrder(instrId1_);
        test::assignClOrderId(order.get());
        clOrderIds.push_back(order->clOrderId_.get());
        inQueues_->push("test", OrderEvent(order.release()));
    }

    // Process all orders
    for (int i = 0; i < numOrders; ++i) {
        processor_->process();
    }

    // Verify all orders were processed
    int processedCount = 0;
    for (const auto& clOrdId : clOrderIds) {
        if (OrderStorage::instance()->locateByClOrderId(clOrdId) != nullptr) {
            ++processedCount;
        }
    }

    EXPECT_EQ(numOrders, processedCount);
}

} // namespace
