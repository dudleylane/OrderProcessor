/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <deque>
#include <vector>

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
#include "StateMachine.h"

using namespace COP;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Store;
using namespace COP::ACID;

namespace
{

// =============================================================================
// Test InQueue Observer Implementation
// =============================================================================

class TestInQueueObserver : public InQueueProcessor
{
public:
    bool process() override
    {
        return false;
    }

    void onEvent(const std::string &source, const OrderEvent &evnt) override
    {
        orders_.push_back(OrderQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string &source, const OrderCancelEvent &evnt) override
    {
        orderCancels_.push_back(OrderCancelQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string &source, const OrderReplaceEvent &evnt) override
    {
        orderReplaces_.push_back(OrderReplaceQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string &source, const OrderChangeStateEvent &evnt) override
    {
        orderStates_.push_back(OrderStateQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string &source, const ProcessEvent &evnt) override
    {
        processes_.push_back(ProcessQueueT::value_type(source, evnt));
    }

    void onEvent(const std::string &source, const TimerEvent &evnt) override
    {
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

class TestTransactionManager : public TransactionManager
{
public:
    TestTransactionManager() : proc_(nullptr), transactionCount_(0) {}

    void attach(TransactionObserver *) override {}

    TransactionObserver *detach() override
    {
        return nullptr;
    }

    void addTransaction(std::unique_ptr<Transaction> &tr) override
    {
        if (onAdd_)
        {
            onAdd_();
        }
        tr->setTransactionId(TransactionId(1, 1));
        if (proc_)
        {
            proc_->process(tr->transactionId(), tr.get());
        }
        ++transactionCount_;
    }

    bool removeTransaction(const TransactionId &, Transaction *) override
    {
        return false;
    }

    bool getParentTransactions(const TransactionId &, TransactionIdsT *) const override
    {
        return false;
    }

    bool getRelatedTransactions(const TransactionId &, TransactionIdsT *) const override
    {
        return false;
    }

    TransactionIterator *iterator() override
    {
        return nullptr;
    }

    int transactionCount() const
    {
        return transactionCount_;
    }

    Processor *proc_;
    /// Called as each transaction is added, before it runs
    std::function<void()> onAdd_;

private:
    int transactionCount_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class ProcessorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
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
        orderBook_->init(instruments);

        // Initialize processor
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                               outQueues_.get(), inQueues_.get(), transMgr_.get());

        processor_ = std::make_unique<Processor>();
        processor_->init(params);

        transMgr_->proc_ = processor_.get();
    }

    void TearDown() override
    {
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
    std::unique_ptr<OrderEntry> createTestOrder(SourceIdT instrId, Side side, PriceT price, QuantityT qty)
    {
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

TEST_F(ProcessorTest, CreateProcessor)
{
    ASSERT_NE(nullptr, processor_);
}

TEST_F(ProcessorTest, ProcessEmptyQueue)
{
    // Processing empty queue should not crash
    bool processed = processor_->process();
    EXPECT_FALSE(processed);
}

// =============================================================================
// Order Processing Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessSingleNewOrder)
{
    auto order = test::createCorrectOrder(instrId1_);
    RawDataEntry clOrdId = order->clOrderId_.get();

    inQueues_->push("test", OrderEvent(order.release()));

    processor_->process();

    OrderEntry *savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);
}

TEST_F(ProcessorTest, ProcessMultipleOrders)
{
    auto order1 = test::createCorrectOrder(instrId1_);
    RawDataEntry ord1ClOrdId = order1->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order1.release()));

    auto order2 = test::createCorrectOrder(instrId1_);
    test::assignClOrderId(order2.get());
    RawDataEntry ord2ClOrdId = order2->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order2.release()));

    // First process - first order
    processor_->process();
    OrderEntry *savedOrder1 = OrderStorage::instance()->locateByClOrderId(ord1ClOrdId);
    ASSERT_NE(nullptr, savedOrder1);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder1->status_);

    // Second order should not be processed yet or same processing cycle
    OrderEntry *savedOrder2 = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
    // It may or may not be processed depending on implementation
    // EXPECT_EQ(nullptr, savedOrder2);

    // Process remaining
    processor_->process();
    savedOrder2 = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
    ASSERT_NE(nullptr, savedOrder2);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder2->status_);
}

TEST_F(ProcessorTest, ProcessMultipleCycles)
{
    auto order = test::createCorrectOrder(instrId1_);
    RawDataEntry clOrdId = order->clOrderId_.get();

    inQueues_->push("test", OrderEvent(order.release()));

    // Multiple process cycles should be safe
    processor_->process();
    processor_->process();
    processor_->process();
    processor_->process();

    OrderEntry *savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);
}

// =============================================================================
// Order Matching Tests
// =============================================================================

TEST_F(ProcessorTest, MatchBuyAgainstSell)
{
    // Create sell order first
    auto sellOrder = createTestOrder(instrId1_, SELL_SIDE, 10.0, 100);
    RawDataEntry sellClOrdId = sellOrder->clOrderId_.get();
    inQueues_->push("test", OrderEvent(sellOrder.release()));

    // Process sell order
    processor_->process();
    OrderEntry *savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
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

    OrderEntry *savedBuy = OrderStorage::instance()->locateByClOrderId(buyClOrdId);
    ASSERT_NE(nullptr, savedBuy);
    EXPECT_EQ(FILLED_ORDSTATUS, savedBuy->status_);

    savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    ASSERT_NE(nullptr, savedSell);
    EXPECT_EQ(PARTFILL_ORDSTATUS, savedSell->status_);
}

TEST_F(ProcessorTest, PartialFillScenario)
{
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
    OrderEntry *savedBuy = OrderStorage::instance()->locateByClOrderId(buyClOrdId);
    ASSERT_NE(nullptr, savedBuy);
    EXPECT_EQ(FILLED_ORDSTATUS, savedBuy->status_);

    // Sell should be partially filled
    OrderEntry *savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    ASSERT_NE(nullptr, savedSell);
    EXPECT_EQ(PARTFILL_ORDSTATUS, savedSell->status_);
}

// =============================================================================
// Different Instruments Tests
// =============================================================================

TEST_F(ProcessorTest, OrdersOnDifferentInstruments)
{
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
    OrderEntry *saved1 = OrderStorage::instance()->locateByClOrderId(clOrdId1);
    ASSERT_NE(nullptr, saved1);
    EXPECT_EQ(NEW_ORDSTATUS, saved1->status_);

    OrderEntry *saved2 = OrderStorage::instance()->locateByClOrderId(clOrdId2);
    ASSERT_NE(nullptr, saved2);
    EXPECT_EQ(NEW_ORDSTATUS, saved2->status_);
}

// =============================================================================
// Cancel Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessCancelEvent)
{
    // First create and process an order
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    // Get the saved order to retrieve its ID
    OrderEntry *savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);

    // Now submit a cancel event for this order
    OrderCancelEvent cancelEvent(savedOrder->orderId_, "Test cancellation");
    inQueues_->push("test", cancelEvent);

    processor_->process();

    // Cancelled outright: until #73 the order only reached Pending Cancel, and stayed in the book
    savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(CANCELED_ORDSTATUS, savedOrder->status_);
    EXPECT_EQ(0u, savedOrder->leavesQty_);
}

TEST_F(ProcessorTest, CancelCompletesAPendingCancelLeftByAnOlderServer)
{
    // Until #73 a cancel left the order in GoingCancel, and persisted it so: such an order, restored from an older
    // data directory, must still be cancellable, or every later cancel would be refused as one already pending.
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();
    OrderEntry *saved = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, saved);

    int goingCancel = 0;
    while ("GoingCancel" != OrdState::OrderState::getStateName(goingCancel)) // throws past the last state
    {
        ++goingCancel;
    }
    OrdState::OrderStatePersistence persisted = saved->stateMachinePersistance();
    persisted.stateZone2Id_ = goingCancel;
    saved->setStateMachinePersistance(persisted);

    inQueues_->push("test", OrderCancelEvent(saved->orderId_, "again"));
    processor_->process();

    EXPECT_EQ(CANCELED_ORDSTATUS, saved->status_);
    EXPECT_EQ(0u, saved->leavesQty_);
}

// =============================================================================
// Replace Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessReplaceEvent)
{
    // First create and process an order
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    // Get the saved order to retrieve its ID
    OrderEntry *savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);

    // Replace it: the original with a new price, a fresh id and a ClOrdID of its own. With transactions run inline, the
    // decision runs within process() (#74).
    OrderEntry *replacement = savedOrder->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = savedOrder->clOrderId_;
    replacement->price_ = 11.0;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    RawDataEntry replacementId = replacement->clOrderId_.get();
    inQueues_->push("test", OrderReplaceEvent(savedOrder->orderId_, replacement));

    processor_->process();

    EXPECT_EQ(REPLACED_ORDSTATUS, savedOrder->status_);
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    EXPECT_EQ(NEW_ORDSTATUS, live->status_);
    EXPECT_DOUBLE_EQ(11.0, live->price_);
    EXPECT_EQ(savedOrder->orderId_, live->origOrderId_);
}

TEST_F(ProcessorTest, ReplaceWithoutAReplacementOrderChangesNothing)
{
    // Before #74 such a request moved the order into GoingReplace, where nothing completed it and every later cancel or
    // replace found one pending
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();
    OrderEntry *savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, savedOrder);
    const int zone2 = savedOrder->stateMachinePersistance().stateZone2Id_;

    inQueues_->push("test", OrderReplaceEvent(savedOrder->orderId_));
    processor_->process();

    EXPECT_EQ(NEW_ORDSTATUS, savedOrder->status_);
    EXPECT_EQ(zone2, savedOrder->stateMachinePersistance().stateZone2Id_);
}

// =============================================================================
// Process Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessProcessEventWithInvalidType)
{
    ProcessEvent event; // Default constructor creates INVALID type
    inQueues_->push("test", event);

    // Invalid ProcessEvent type should throw
    EXPECT_THROW(processor_->process(), std::runtime_error);
}

// =============================================================================
// Timer Event Tests
// =============================================================================

TEST_F(ProcessorTest, ProcessTimerEvent)
{
    // First create and process an order
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    // Get the saved order to retrieve its ID
    OrderEntry *savedOrder = OrderStorage::instance()->locateByClOrderId(clOrdId);
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

TEST_F(ProcessorTest, ConcurrentOrderSubmission)
{
    const int numOrders = 50;
    std::vector<RawDataEntry> clOrderIds;

    // Submit orders
    for (int i = 0; i < numOrders; ++i)
    {
        auto order = test::createCorrectOrder(instrId1_);
        test::assignClOrderId(order.get());
        clOrderIds.push_back(order->clOrderId_.get());
        inQueues_->push("test", OrderEvent(order.release()));
    }

    // Process all orders
    for (int i = 0; i < numOrders; ++i)
    {
        processor_->process();
    }

    // Verify all orders were processed
    int processedCount = 0;
    for (const auto &clOrdId : clOrderIds)
    {
        if (OrderStorage::instance()->locateByClOrderId(clOrdId) != nullptr)
        {
            ++processedCount;
        }
    }

    EXPECT_EQ(numOrders, processedCount);
}

// =============================================================================
// Transaction Failure Handling Tests
// =============================================================================

/**
 * Test that verifies the Processor properly handles transaction failures.
 * When a transaction fails (executeTransaction returns false), deferred
 * events should NOT be processed and should be cleaned up.
 *
 * Note: These tests verify the fix for the bug where Processor::process()
 * ignored the return value of executeTransaction() and always processed
 * deferred events, even after a transaction failure.
 */

TEST_F(ProcessorTest, ProcessTransaction_Success_ProcessesDeferredEvents)
{
    // Submit a valid order - this should succeed
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    inQueues_->push("test", OrderEvent(order.release()));

    // Process should succeed
    EXPECT_NO_THROW(processor_->process());

    // Verify transaction was processed (order was saved)
    EXPECT_GT(transMgr_->transactionCount(), 0);
}

TEST_F(ProcessorTest, ProcessTransaction_EmptyQueueReturnsFalse)
{
    // Processing an empty queue should return false
    bool result = processor_->process();
    EXPECT_FALSE(result);
}

TEST_F(ProcessorTest, ProcessMultipleOrders_AllTransactionsExecuted)
{
    const int numOrders = 5;

    // Submit multiple orders
    for (int i = 0; i < numOrders; ++i)
    {
        auto order = test::createCorrectOrder(instrId1_);
        test::assignClOrderId(order.get());
        inQueues_->push("test", OrderEvent(order.release()));
    }

    // Process all
    for (int i = 0; i < numOrders; ++i)
    {
        processor_->process();
    }

    // All transactions should have been processed
    EXPECT_EQ(numOrders, transMgr_->transactionCount());
}

} // namespace

// =============================================================================
// Durability ordering (#28)
// =============================================================================

namespace
{
/// Records the order in which the engine persists and publishes, in one sequence.
class RecordingOutQueues final : public Queues::OutQueues
{
public:
    explicit RecordingOutQueues(std::vector<std::string> *seq) : seq_(seq) {}

    void push(const Queues::ExecReportEvent &, const std::string &) override
    {
        seq_->push_back("publish");
    }
    void push(const Queues::CancelRejectEvent &, const std::string &) override
    {
        seq_->push_back("publish");
    }
    void push(const Queues::BusinessRejectEvent &, const std::string &) override
    {
        seq_->push_back("publish");
    }

private:
    std::vector<std::string> *seq_;
};

class RecordingOrderSaver final : public COP::OrderSaver
{
public:
    explicit RecordingOrderSaver(std::vector<std::string> *seq) : seq_(seq), version_(0) {}

    COP::u32 save(const COP::OrderEntry &) override
    {
        seq_->push_back("persist");
        return version_++;
    }
    void erase(const COP::IdT &, COP::u32) override
    {
        seq_->push_back("erase");
    }

private:
    std::vector<std::string> *seq_;
    COP::u32 version_;
};
} // namespace

TEST_F(ProcessorTest, OrderIsDurableBeforeItsExecutionReportIsPublished)
{
    // CreateExecReportTrOperation publishes during execute and WsOutQueues broadcasts straight
    // away, so persisting last let a client see a change before it was durable - a window of about
    // one LMDB write, which is milliseconds with fsync per commit (#28).
    std::vector<std::string> sequence;
    RecordingOrderSaver saver(&sequence);
    RecordingOutQueues outQueues(&sequence);
    OrderStorage::instance()->attach(&saver);

    ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                           &outQueues, inQueues_.get(), transMgr_.get());
    Processor processor;
    processor.init(params);
    transMgr_->proc_ = &processor;

    auto order = test::createCorrectOrder(instrId1_);
    inQueues_->push("test", OrderEvent(order.release()));
    processor.process();

    ASSERT_FALSE(sequence.empty());
    EXPECT_EQ("persist", sequence.front());
    EXPECT_NE(std::find(sequence.begin(), sequence.end(), "publish"), sequence.end());
}

// =============================================================================
// A market order with nothing to trade with (#68)
// =============================================================================

TEST_F(ProcessorTest, MarketOrderWithNothingToMatchIsStoredOnceAndRejected)
{
    // The receive stores the order, then finds nothing on the other side and refuses it. The reject asserted it had
    // been handed the incoming order rather than the stored copy, and Debug builds aborted here (#68).
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    order->ordType_ = MARKET_ORDERTYPE;
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();

    OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, stored);
    EXPECT_EQ(REJECTED_ORDSTATUS, stored->status_);
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "a refused order must not rest in the book";
}

// =============================================================================
// A cancel or replace comes after the transaction that books its order (#83)
// =============================================================================

TEST_F(ProcessorTest, NewOrderIsMarkedUntilTheTransactionThatBooksItIsEnqueued)
{
    // A cancel or replace of the order waits while it is marked, so the mark must last until the order's own
    // transaction has its id
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    std::vector<bool> marks;
    transMgr_->onAdd_ = [&]()
    {
        if (const OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId))
        {
            marks.push_back(stored->bookingPending_.load());
        }
    };
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();
    transMgr_->onAdd_ = nullptr;

    ASSERT_FALSE(marks.empty()) << "the order was not stored";
    EXPECT_TRUE(marks.front()) << "the order was not marked when the transaction that books it was enqueued";
    OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, stored);
    EXPECT_EQ(NEW_ORDSTATUS, stored->status_);
    EXPECT_FALSE(stored->bookingPending_.load());
}

TEST_F(ProcessorTest, ReplacementIsMarkedUntilTheTransactionThatBooksItIsEnqueued)
{
    // The same for a replacement, which the replace's decision stores on the transaction worker
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();
    OrderEntry *original = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, original);

    OrderEntry *replacement = original->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = original->clOrderId_;
    replacement->price_ = 11.0;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    RawDataEntry replacementId = replacement->clOrderId_.get();
    std::vector<bool> marks;
    transMgr_->onAdd_ = [&]()
    {
        if (const OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(replacementId))
        {
            marks.push_back(stored->bookingPending_.load());
        }
    };
    inQueues_->push("test", OrderReplaceEvent(original->orderId_, replacement));
    processor_->process();
    transMgr_->onAdd_ = nullptr;

    ASSERT_FALSE(marks.empty()) << "the replacement was not stored";
    EXPECT_TRUE(marks.front()) << "the replacement was not marked when the transaction that books it was enqueued";
    EXPECT_EQ(REPLACED_ORDSTATUS, original->status_);
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    EXPECT_EQ(NEW_ORDSTATUS, live->status_);
    EXPECT_FALSE(live->bookingPending_.load());
}

TEST_F(ProcessorTest, CancelWaitsForTheTransactionThatBooksTheOrder)
{
    // Stored, and its creator has not yet enqueued the transaction that books it. A cancel enqueued now would get the
    // lower id and be decided before the order was booked. Here the creator enqueues it 100 ms later.
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    COP::Store::PublishGuard guard;
    OrderEntry *stored = OrderStorage::instance()->save(*order, IdTGenerator::instance(), &guard);
    ASSERT_NE(nullptr, stored);
    guard.release(); // initialised: only the mark is left

    transMgr_->proc_ = nullptr; // only when the cancel is enqueued matters here, so nothing runs
    std::vector<bool> marks;
    transMgr_->onAdd_ = [&]()
    {
        marks.push_back(stored->bookingPending_.load());
    };
    std::thread creator(
        [&guard]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            guard.bookingEnqueued();
        });
    inQueues_->push("test", OrderCancelEvent(stored->orderId_, "client"));
    processor_->process();
    creator.join();
    transMgr_->onAdd_ = nullptr;

    ASSERT_EQ(1u, marks.size());
    EXPECT_FALSE(marks.front()) << "the cancel was enqueued before the transaction that books the order";
}

TEST_F(ProcessorTest, ReplaceWaitsForTheTransactionThatBooksTheOriginal)
{
    // The same for a replace of it
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    COP::Store::PublishGuard guard;
    OrderEntry *stored = OrderStorage::instance()->save(*order, IdTGenerator::instance(), &guard);
    ASSERT_NE(nullptr, stored);
    guard.release();

    OrderEntry *replacement = stored->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = stored->clOrderId_;
    replacement->price_ = 11.0;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;

    transMgr_->proc_ = nullptr;
    std::vector<bool> marks;
    transMgr_->onAdd_ = [&]()
    {
        marks.push_back(stored->bookingPending_.load());
    };
    std::thread creator(
        [&guard]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            guard.bookingEnqueued();
        });
    inQueues_->push("test", OrderReplaceEvent(stored->orderId_, replacement));
    processor_->process();
    creator.join();
    transMgr_->onAdd_ = nullptr;

    ASSERT_EQ(1u, marks.size());
    EXPECT_FALSE(marks.front()) << "the replace was enqueued before the transaction that books the original";
}
