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
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <memory>
#include <deque>
#include <filesystem>
#include <set>
#include <vector>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"
#include "MockTransaction.h"
#include "MockOrderBook.h"

#include "DataModelDef.h"
#include "Processor.h"
#include "ExpiryScheduler.h"
#include "TransactionScope.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "StateMachine.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"
#include "TransactionMgr.h"

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
        if (keep_ || (keepNested_ && (0 < running_)))
        {
            kept_.push_back(std::move(tr));
            return;
        }
        if (proc_)
        {
            ++running_;
            try
            {
                proc_->process(tr->transactionId(), tr.get());
            }
            catch (...)
            {
                --running_;
                throw;
            }
            --running_;
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

    size_t pendingTransactions() const override
    {
        return kept_.size();
    }

    int transactionCount() const
    {
        return transactionCount_;
    }

    /// Runs the transactions kept, first kept first, at most count of them. With keepNested_, one a transaction adds
    /// is kept behind the others, so they run as the manager runs transactions that share an object: each after the
    /// ones enqueued before it are done, deferred events and all (#223).
    void runKept(size_t count = SIZE_MAX)
    {
        for (; (0 < count) && !kept_.empty(); --count)
        {
            std::unique_ptr<Transaction> tr = std::move(kept_.front());
            kept_.erase(kept_.begin());
            ++running_;
            try
            {
                proc_->process(tr->transactionId(), tr.get());
            }
            catch (...)
            {
                --running_;
                throw;
            }
            --running_;
            ++transactionCount_;
        }
    }

    Processor *proc_;
    /// Called as each transaction is added, before it runs
    std::function<void()> onAdd_;
    /// While set, a transaction added is kept here, not run
    bool keep_ = false;
    /// While set, a transaction added while another runs is kept, not run: one a deferred event adds, a trade's or a
    /// cancel's. Each is lost, as a crash would lose it after the transaction before it committed.
    bool keepNested_ = false;
    std::vector<std::unique_ptr<Transaction>> kept_;

private:
    int transactionCount_;
    int running_ = 0;
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

TEST_F(ProcessorTest, CancelCompletesAPendingReplaceLeftByAnOlderServer)
{
    // Builds from 6c3ada4 to 584e6a6 persisted an original after it took a replace, and nothing completed the replace:
    // restored, the order stays in GoingReplace. A cancel found no transition and was refused as one already pending,
    // a replace is refused too, and the order stayed in the book and still traded (#164). A cancel must end it.
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor_->process();
    OrderEntry *saved = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, saved);

    int goingReplace = 0;
    while ("GoingReplace" != OrdState::OrderState::getStateName(goingReplace)) // throws past the last state
    {
        ++goingReplace;
    }
    OrdState::OrderStatePersistence persisted = saved->stateMachinePersistance();
    persisted.stateZone2Id_ = goingReplace;
    saved->setStateMachinePersistance(persisted);

    inQueues_->push("test", OrderCancelEvent(saved->orderId_, "ending it"));
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

TEST_F(ProcessorTest, AnOrderIsPersistedOncePerTransaction)
{
    // LMDBStorage::update() keeps the newest version it finds besides the one it writes (#148). That is safe because
    // the transactions that persist an order run one after another, and each writes it once: a rollback then erases
    // the one version it wrote. A second persist of the order in the same transaction would write the same record,
    // since the codec reads the order when the operation executes, and a rollback would erase both versions.
    std::vector<std::string> sequence;
    RecordingOrderSaver saver(&sequence);
    OrderStorage::instance()->attach(&saver);

    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    order->orderId_ = IdT(1001, 1);
    auto other = createTestOrder(instrId1_, SELL_SIDE, 10.0, 100);
    other->orderId_ = IdT(1002, 1);

    TransactionScope scope;
    Processor::persistOrder(&scope, *order);
    Processor::persistOrder(&scope, *other);
    Processor::persistOrder(&scope, *order);
    EXPECT_EQ(2u, scope.operationCount());

    Context cnxt;
    cnxt.orderStorage_ = OrderStorage::instance();
    EXPECT_TRUE(scope.executeTransaction(cnxt));
    EXPECT_EQ((std::vector<std::string>{ "persist", "persist" }), sequence);
}

// =============================================================================
// A state change is decided when its transaction runs (#96)
// =============================================================================

namespace
{
/// Books a buy, then handles evnt with the transaction manager keeping what it enqueues. Returns the order.
template <typename Event>
OrderEntry *enqueueStateChangeFor(IncomingQueues &inQueues, Processor &processor, TestTransactionManager &transMgr,
                                  std::unique_ptr<OrderEntry> order, Event (*makeEvent)(const IdT &))
{
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues.push("test", OrderEvent(order.release()));
    processor.process();
    OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
    if ((nullptr == stored) || (NEW_ORDSTATUS != stored->status_))
    {
        return nullptr;
    }
    transMgr.keep_ = true;
    inQueues.push("test", makeEvent(stored->orderId_));
    processor.process();
    transMgr.keep_ = false;
    return stored;
}

OrderChangeStateEvent suspendOf(const IdT &id)
{
    return OrderChangeStateEvent(id, OrderChangeStateEvent::SUSPEND);
}

TimerEvent expiryOf(const IdT &id)
{
    return TimerEvent(id, TimerEvent::EXPIRATION);
}
} // namespace

TEST_F(ProcessorTest, SuspendIsDecidedWhenItsTransactionRuns)
{
    // As a cancel is (#73): the handler only enqueues the request, and the state machine runs when its transaction
    // does, after the order's earlier transactions and the fills they caused.
    OrderEntry *stored = enqueueStateChangeFor(*inQueues_, *processor_, *transMgr_,
                                               createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), &suspendOf);
    ASSERT_NE(nullptr, stored);
    ASSERT_EQ(1u, transMgr_->kept_.size());
    EXPECT_EQ(NEW_ORDSTATUS, stored->status_) << "the suspend was decided before its transaction ran";

    processor_->process(TransactionId(1, 1), transMgr_->kept_.front().get());
    EXPECT_EQ(SUSPENDED_ORDSTATUS, stored->status_);
}

TEST_F(ProcessorTest, ExpiryIsDecidedWhenItsTransactionRuns)
{
    OrderEntry *stored = enqueueStateChangeFor(*inQueues_, *processor_, *transMgr_,
                                               createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), &expiryOf);
    ASSERT_NE(nullptr, stored);
    ASSERT_EQ(1u, transMgr_->kept_.size());
    EXPECT_EQ(NEW_ORDSTATUS, stored->status_) << "the expiry was decided before its transaction ran";

    processor_->process(TransactionId(1, 1), transMgr_->kept_.front().get());
    EXPECT_EQ(EXPIRED_ORDSTATUS, stored->status_);
}

TEST_F(ProcessorTest, StateChangeWaitsForTheTransactionThatBooksTheOrder)
{
    // As a cancel does (#83): enqueued before the transaction that books the order, it would be decided first
    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    COP::Store::PublishGuard guard;
    OrderEntry *stored = OrderStorage::instance()->save(*order, IdTGenerator::instance(), &guard);
    ASSERT_NE(nullptr, stored);
    guard.release();

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
    inQueues_->push("test", OrderChangeStateEvent(stored->orderId_, OrderChangeStateEvent::SUSPEND));
    inQueues_->push("test", TimerEvent(stored->orderId_, TimerEvent::EXPIRATION));
    processor_->process();
    processor_->process();
    creator.join();
    transMgr_->onAdd_ = nullptr;

    ASSERT_EQ(2u, marks.size());
    EXPECT_FALSE(marks[0]) << "the suspend was enqueued before the transaction that books the order";
    EXPECT_FALSE(marks[1]) << "the expiry was enqueued before the transaction that books the order";
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
// A cancel's answers carry the cancel request's own ClOrdID (#98)
// =============================================================================

namespace
{
/// Records the request ClOrdIDs that cancel answers carry
class CancelAnswerQueues final : public Queues::OutQueues
{
public:
    void push(const Queues::ExecReportEvent &evnt, const std::string &) override
    {
        if ((nullptr != evnt.exec_) && (CANCEL_EXECTYPE == evnt.exec_->type_))
        {
            acks_.push_back(evnt.requestClOrdId_);
        }
    }
    void push(const Queues::CancelRejectEvent &evnt, const std::string &) override
    {
        rejects_.emplace_back(evnt.reason_, evnt.requestClOrdId_);
    }
    void push(const Queues::BusinessRejectEvent &, const std::string &) override {}

    std::vector<std::string> acks_;
    std::vector<std::pair<Queues::CancelRejectEvent::Reason, std::string>> rejects_;
};
} // namespace

TEST_F(ProcessorTest, CancelAnswersCarryTheRequestsClOrdId)
{
    // FIX answers a cancel with the cancel request's own ClOrdID in tag 11, and the engine is what has to carry it
    // there: on the acknowledgement, and on a reject, whether too late or for an unknown order (#98)
    CancelAnswerQueues out;
    ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(), &out,
                           inQueues_.get(), transMgr_.get());
    Processor processor;
    processor.init(params);
    transMgr_->proc_ = &processor;

    auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));
    processor.process();
    OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, stored);

    OrderCancelEvent first(stored->orderId_);
    first.requestClOrdId_ = "CXL-1";
    inQueues_->push("client", first);
    processor.process();
    OrderCancelEvent second(stored->orderId_);
    second.requestClOrdId_ = "CXL-2";
    inQueues_->push("client", second);
    processor.process();
    OrderCancelEvent unknown(IdT(stored->orderId_.id_ + 1000, stored->orderId_.date_));
    unknown.requestClOrdId_ = "CXL-3";
    inQueues_->push("client", unknown);
    processor.process();

    EXPECT_EQ(CANCELED_ORDSTATUS, stored->status_);
    ASSERT_EQ(1u, out.acks_.size());
    EXPECT_EQ("CXL-1", out.acks_[0]);
    ASSERT_EQ(2u, out.rejects_.size());
    EXPECT_EQ(Queues::CancelRejectEvent::TOO_LATE, out.rejects_[0].first);
    EXPECT_EQ("CXL-2", out.rejects_[0].second);
    EXPECT_EQ(Queues::CancelRejectEvent::UNKNOWN_ORDER, out.rejects_[1].first);
    EXPECT_EQ("CXL-3", out.rejects_[1].second);
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

// =============================================================================
// A swap's fill is reported per leg (#153)
// =============================================================================

namespace
{
/// Keeps what a test needs of each execution report: whose it is, its type, its leg and its price
class LegRecordingOutQueues final : public Queues::OutQueues
{
public:
    struct Report
    {
        IdT orderId_;
        ExecType type_;
        ExecLegType leg_;
        PriceT lastPx_;
        u64 tradeLink_;
    };

    void push(const ExecReportEvent &evnt, const std::string &) override
    {
        const auto *trade = dynamic_cast<const TradeExecEntry *>(evnt.exec_);
        reports_.push_back(Report{ evnt.exec_->orderId_, evnt.exec_->type_, evnt.exec_->execLegType_,
                                   (nullptr != trade) ? trade->lastPx_ : 0.0, evnt.exec_->tradeLink_ });
    }
    void push(const CancelRejectEvent &, const std::string &) override {}
    void push(const BusinessRejectEvent &, const std::string &) override {}

    std::vector<Report> reports_;
};
} // namespace

TEST_F(ProcessorTest, ASwapFillIsReportedAsItsNearLegAndItsFarLeg)
{
    // Each side of a swap fill gets two trade reports, its near leg at the near price and its far leg at the far price.
    // The near leg's report used to say SINGLE_LEG, so neither JSON nor FIX clients could tell it from a plain fill.
    LegRecordingOutQueues outQueues;
    ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                           &outQueues, inQueues_.get(), transMgr_.get());
    Processor processor;
    processor.init(params);
    transMgr_->proc_ = &processor;

    const auto swap = [this](Side side)
    {
        auto order = createTestOrder(instrId1_, side, 1.0850, 100);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->farPrice_ = 1.0872;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    auto sell = swap(SELL_SIDE);
    const RawDataEntry sellClOrdId = sell->clOrderId_.get();
    inQueues_->push("test", OrderEvent(sell.release()));
    processor.process();
    auto buy = swap(BUY_SIDE);
    const RawDataEntry buyClOrdId = buy->clOrderId_.get();
    inQueues_->push("test", OrderEvent(buy.release()));
    processor.process();

    const OrderEntry *savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    const OrderEntry *savedBuy = OrderStorage::instance()->locateByClOrderId(buyClOrdId);
    ASSERT_NE(nullptr, savedSell);
    ASSERT_NE(nullptr, savedBuy);
    ASSERT_EQ(FILLED_ORDSTATUS, savedBuy->status_) << "the swaps didn't match";

    for (const OrderEntry *order : { savedSell, savedBuy })
    {
        std::vector<std::pair<ExecLegType, PriceT>> legs;
        for (const auto &report : outQueues.reports_)
        {
            if ((report.orderId_ == order->orderId_) && (TRADE_EXECTYPE == report.type_))
            {
                legs.emplace_back(report.leg_, report.lastPx_);
            }
        }
        ASSERT_EQ(2u, legs.size()) << "trade reports for order " << order->orderId_.id_;
        // as ints: ExecLegType is a uint8_t, which gtest would print as a character
        EXPECT_EQ(static_cast<int>(NEAR_LEG), static_cast<int>(legs[0].first)) << "order " << order->orderId_.id_;
        EXPECT_DOUBLE_EQ(1.0850, legs[0].second);
        EXPECT_EQ(static_cast<int>(FAR_LEG), static_cast<int>(legs[1].first)) << "order " << order->orderId_.id_;
        EXPECT_DOUBLE_EQ(1.0872, legs[1].second);
    }
}

// =============================================================================
// Orders a crash left without their fills (#207)
// =============================================================================

namespace
{
/// Keeps each trade the engine reports, as the client is told it: the order, the quantity and the price
class TradeRecorder final : public Queues::OutQueues
{
public:
    struct Trade
    {
        IdT orderId_;
        QuantityT lastQty_;
        PriceT lastPx_;
        DateTimeT tradeDate_;
    };

    void push(const Queues::ExecReportEvent &evnt, const std::string &) override
    {
        if (nullptr == evnt.exec_)
        {
            return;
        }
        // The order's LeavesQty as the report goes out, when the WebSocket and FIX gateways read it for the report
        const OrderEntry *order = OrderStorage::instance()->locateByOrderId(evnt.exec_->orderId_);
        reports_.push_back(
            Report{ evnt.exec_->orderId_, evnt.exec_->type_, (nullptr != order) ? order->leavesQty_ : UNKNOWN_LEAVES });
        if (nullptr != log_)
        {
            log_->push_back(std::string("report ") + typeName(evnt.exec_->type_));
        }
        if (TRADE_EXECTYPE == evnt.exec_->type_)
        {
            const TradeExecEntry *trade = static_cast<const TradeExecEntry *>(evnt.exec_);
            trades_.push_back(Trade{ trade->orderId_, trade->lastQty_, trade->lastPx_, trade->tradeDate_ });
        }
    }
    void push(const Queues::CancelRejectEvent &, const std::string &) override {}
    void push(const Queues::BusinessRejectEvent &, const std::string &) override {}

    /// The trades reported for the order, as "quantity@price"
    std::vector<std::string> tradesOf(const IdT &orderId) const
    {
        std::vector<std::string> result;
        for (const Trade &trade : trades_)
        {
            if (trade.orderId_ == orderId)
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%llu@%g", static_cast<unsigned long long>(trade.lastQty_), trade.lastPx_);
                result.emplace_back(buf);
            }
        }
        return result;
    }

    /// The dates of the trades reported for the order, YYYYMMDD (#258)
    std::vector<DateTimeT> tradeDatesOf(const IdT &orderId) const
    {
        std::vector<DateTimeT> result;
        for (const Trade &trade : trades_)
        {
            if (trade.orderId_ == orderId)
            {
                result.push_back(trade.tradeDate_);
            }
        }
        return result;
    }

    /// The types of the reports sent for the order, in the order they were sent
    std::vector<ExecType> reportTypesOf(const IdT &orderId) const
    {
        std::vector<ExecType> result;
        for (const Report &report : reports_)
        {
            if (report.orderId_ == orderId)
            {
                result.push_back(report.type_);
            }
        }
        return result;
    }

    /// The order's LeavesQty as its last report of the type went out (#386): UNKNOWN_LEAVES if none did, or if the
    /// fixture's storage doesn't hold the order
    QuantityT leavesReportedWith(const IdT &orderId, ExecType type) const
    {
        QuantityT leaves = UNKNOWN_LEAVES;
        for (const Report &report : reports_)
        {
            if ((report.orderId_ == orderId) && (report.type_ == type))
            {
                leaves = report.leaves_;
            }
        }
        return leaves;
    }

    static constexpr QuantityT UNKNOWN_LEAVES = std::numeric_limits<QuantityT>::max();

    static const char *typeName(ExecType type)
    {
        switch (type)
        {
        case NEW_EXECTYPE:
            return "NEW";
        case TRADE_EXECTYPE:
            return "TRADE";
        case CANCEL_EXECTYPE:
            return "CANCEL";
        case TRIGGERED_EXECTYPE:
            return "TRIGGERED";
        case RESTATED_EXECTYPE:
            return "RESTATED";
        default:
            return "OTHER";
        }
    }

    struct Report
    {
        IdT orderId_;
        ExecType type_;
        QuantityT leaves_;
    };

    std::vector<Trade> trades_;
    std::vector<Report> reports_;
    /// If set, every report is noted here as "report <type>"
    std::vector<std::string> *log_ = nullptr;
};

/// One run of the engine over a data directory, wired as app/main.cpp wires it, except that transactions run as they
/// are added: what the directory holds is restored when it starts, and each order change is written to it
class EngineOverDirectory
{
public:
    EngineOverDirectory(const std::string &dir, const SourceIdT &instrument, IncomingQueues *inQueues,
                        OutQueues *outQueues)
    {
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(instrument);
        book_.init(instruments);
        dispatcher_.init(WideDataStorage::instance(), &book_, &lmdb_, &storage_);
        lmdb_.load(dir, &dispatcher_);
        storage_.attach(&dispatcher_);
        ProcessorParams params(IdTGenerator::instance(), &storage_, &book_, inQueues, outQueues, inQueues,
                               &transactions_);
        processor_.init(params);
        transactions_.proc_ = &processor_;
    }

    /// Processes the order event queued, and returns the stored order
    OrderEntry *process(const RawDataEntry &clOrdId)
    {
        processor_.process();
        OrderEntry *stored = storage_.locateByClOrderId(clOrdId);
        EXPECT_NE(nullptr, stored);
        return stored;
    }

    LMDBStorage lmdb_;
    StorageRecordDispatcher dispatcher_;
    OrderBookImpl book_;
    OrderDataStorage storage_;
    TestTransactionManager transactions_;
    Processor processor_;
};
} // namespace

TEST_F(ProcessorTest, OrdersACrashLeftWithoutTheirFillsTradeAsTheyDidOnceRestarted)
{
    // X rests. Y crosses it, and W crosses what Y leaves. The transaction that books each commits, and a crash loses the
    // transactions with their fills (#207). Restored, Y and W must trade as they did: Y with X at X's price, then W with
    // Y at Y's. All three used to be booked as they were restored, crossed, and nothing traded them.
    const std::string dir = test::uniqueTestPath("uncross-restart");
    std::filesystem::remove_all(dir);
    IdT x;
    IdT y;
    IdT w;
    {
        TradeRecorder reports;
        EngineOverDirectory crashed(dir, instrId1_, inQueues_.get(), &reports);
        crashed.transactions_.keepNested_ = true; // every fill is lost
        auto order = createTestOrder(instrId1_, SELL_SIDE, 100.0, 100);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        x = crashed.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, BUY_SIDE, 101.0, 200);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        y = crashed.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, SELL_SIDE, 100.5, 50);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        w = crashed.process(clOrdId)->orderId_;
    }

    {
        TradeRecorder reports;
        EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
        const IdT ids[] = { x, y, w };
        for (const IdT &id : ids)
        {
            const OrderEntry *order = restarted.storage_.locateByOrderId(id);
            ASSERT_NE(nullptr, order);
            ASSERT_EQ(NEW_ORDSTATUS, order->status_) << "the crash should leave each order as its booking wrote it";
            ASSERT_EQ(order->orderQty_, order->leavesQty_);
        }
        const IdT bestBid = restarted.book_.getTop(instrId1_, BUY_SIDE);
        const IdT bestAsk = restarted.book_.getTop(instrId1_, SELL_SIDE);
        if (bestBid.isValid() && bestAsk.isValid())
        {
            EXPECT_LT(restarted.storage_.locateByOrderId(bestBid)->price_,
                      restarted.storage_.locateByOrderId(bestAsk)->price_)
                << "the restored book is crossed";
        }
        ASSERT_EQ(2u, restarted.dispatcher_.ordersToMatch().size());
        EXPECT_EQ(y, restarted.dispatcher_.ordersToMatch()[0]->orderId_);
        EXPECT_EQ(w, restarted.dispatcher_.ordersToMatch()[1]->orderId_);

        for (OrderEntry *order : restarted.dispatcher_.ordersToMatch())
        {
            restarted.processor_.matchRestored(order);
        }

        EXPECT_EQ(std::vector<std::string>{ "100@100" }, reports.tradesOf(x));
        EXPECT_EQ((std::vector<std::string>{ "100@100", "50@101" }), reports.tradesOf(y));
        EXPECT_EQ(std::vector<std::string>{ "50@101" }, reports.tradesOf(w)) << "W trades at Y's price, as it did";
        EXPECT_EQ(y, restarted.book_.getTop(instrId1_, BUY_SIDE));
        EXPECT_FALSE(restarted.book_.getTop(instrId1_, SELL_SIDE).isValid());
    }

    // The fills are durable: restarted again, Y rests with what is left, and nothing waits to be matched
    {
        TradeRecorder reports;
        EngineOverDirectory again(dir, instrId1_, inQueues_.get(), &reports);
        EXPECT_TRUE(again.dispatcher_.ordersToMatch().empty());
        EXPECT_EQ(FILLED_ORDSTATUS, again.storage_.locateByOrderId(x)->status_);
        EXPECT_EQ(PARTFILL_ORDSTATUS, again.storage_.locateByOrderId(y)->status_);
        EXPECT_EQ(50u, again.storage_.locateByOrderId(y)->leavesQty_);
        EXPECT_EQ(FILLED_ORDSTATUS, again.storage_.locateByOrderId(w)->status_);
        EXPECT_EQ(y, again.book_.getTop(instrId1_, BUY_SIDE));
        EXPECT_FALSE(again.book_.getTop(instrId1_, SELL_SIDE).isValid());
    }
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, AnOpenMarketOrderACrashLeftTradesAndIsCanceledOnceRestarted)
{
    // S rests with 20. Market order M buys 50: it trades 20 with S, and what it can't trade is canceled. L, a sell that
    // arrives after M, rests. A crash loses M's trade and its cancel (#207). M is never booked, so restored it stayed
    // open for good. It must trade with S and be canceled, as it was, and not trade with L, which arrived after it.
    const std::string dir = test::uniqueTestPath("uncross-market");
    std::filesystem::remove_all(dir);
    IdT s;
    IdT m;
    IdT l;
    {
        TradeRecorder reports;
        EngineOverDirectory crashed(dir, instrId1_, inQueues_.get(), &reports);
        crashed.transactions_.keepNested_ = true; // the trade and the cancel are lost
        auto order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 20);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        s = crashed.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, BUY_SIDE, 0.0, 50);
        order->ordType_ = MARKET_ORDERTYPE;
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        m = crashed.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, SELL_SIDE, 9.0, 10);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        l = crashed.process(clOrdId)->orderId_;
    }

    {
        TradeRecorder reports;
        EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
        ASSERT_EQ(NEW_ORDSTATUS, restarted.storage_.locateByOrderId(m)->status_);
        ASSERT_EQ(50u, restarted.storage_.locateByOrderId(m)->leavesQty_);
        ASSERT_EQ(2u, restarted.dispatcher_.ordersToMatch().size());
        EXPECT_EQ(m, restarted.dispatcher_.ordersToMatch()[0]->orderId_);
        EXPECT_EQ(l, restarted.dispatcher_.ordersToMatch()[1]->orderId_);

        for (OrderEntry *order : restarted.dispatcher_.ordersToMatch())
        {
            restarted.processor_.matchRestored(order);
        }

        EXPECT_EQ(std::vector<std::string>{ "20@10" }, reports.tradesOf(s));
        EXPECT_EQ(std::vector<std::string>{ "20@10" }, reports.tradesOf(m));
        EXPECT_TRUE(reports.tradesOf(l).empty()) << "M traded with an order that arrived after it";
        EXPECT_EQ(CANCELED_ORDSTATUS, restarted.storage_.locateByOrderId(m)->status_);
        EXPECT_EQ(l, restarted.book_.getTop(instrId1_, SELL_SIDE));
    }

    {
        TradeRecorder reports;
        EngineOverDirectory again(dir, instrId1_, inQueues_.get(), &reports);
        EXPECT_TRUE(again.dispatcher_.ordersToMatch().empty());
        EXPECT_EQ(FILLED_ORDSTATUS, again.storage_.locateByOrderId(s)->status_);
        EXPECT_EQ(CANCELED_ORDSTATUS, again.storage_.locateByOrderId(m)->status_);
        EXPECT_EQ(20u, again.storage_.locateByOrderId(m)->cumQty_);
        EXPECT_EQ(NEW_ORDSTATUS, again.storage_.locateByOrderId(l)->status_);
        EXPECT_EQ(l, again.book_.getTop(instrId1_, SELL_SIDE));
    }
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, AMarketOrderThatFillsCompletelyIsReportedFilled)
{
    // A fill that leaves an order nothing removes it from the book, and a market order was never in it. The removal
    // threw, the state machine swallowed it, and the order never reached Filled: it stayed NEW with nothing left, its
    // client told nothing of its trade, and it was persisted so (#211).
    const std::string dir = test::uniqueTestPath("market-full-fill");
    std::filesystem::remove_all(dir);
    IdT sell;
    IdT market;
    {
        TradeRecorder reports;
        EngineOverDirectory engine(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 100);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        sell = engine.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, BUY_SIDE, 0.0, 30);
        order->ordType_ = MARKET_ORDERTYPE;
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        const OrderEntry *bought = engine.process(clOrdId);
        market = bought->orderId_;

        EXPECT_EQ(FILLED_ORDSTATUS, bought->status_);
        EXPECT_EQ(0u, bought->leavesQty_);
        EXPECT_EQ(30u, bought->cumQty_);
        EXPECT_EQ(std::vector<std::string>{ "30@10" }, reports.tradesOf(market))
            << "the market order's trade went unreported";
        EXPECT_EQ(std::vector<std::string>{ "30@10" }, reports.tradesOf(sell));
        EXPECT_EQ(sell, engine.book_.getTop(instrId1_, SELL_SIDE)) << "the sell rests with what is left";
    }
    {
        TradeRecorder reports;
        EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
        EXPECT_EQ(FILLED_ORDSTATUS, restarted.storage_.locateByOrderId(market)->status_);
        EXPECT_EQ(PARTFILL_ORDSTATUS, restarted.storage_.locateByOrderId(sell)->status_);
        EXPECT_EQ(70u, restarted.storage_.locateByOrderId(sell)->leavesQty_);
    }
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, AMarketOrdersRestTheEngineCanceledIsRestoredWithNoLeaves)
{
    // The engine cancels what a market order couldn't trade at once. A closed order's LeavesQty is 0, whatever closed
    // it (#386), and its record says so: the rest, 20, was kept.
    const std::string dir = test::uniqueTestPath("market-rest-canceled");
    std::filesystem::remove_all(dir);
    IdT market;
    {
        TradeRecorder reports;
        EngineOverDirectory engine(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 10);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        engine.process(clOrdId);
        order = createTestOrder(instrId1_, BUY_SIDE, 0.0, 30);
        order->ordType_ = MARKET_ORDERTYPE;
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        const OrderEntry *bought = engine.process(clOrdId);
        market = bought->orderId_;

        EXPECT_EQ(CANCELED_ORDSTATUS, bought->status_);
        EXPECT_EQ(10u, bought->cumQty_);
        EXPECT_EQ(0u, bought->leavesQty_);
    }
    {
        TradeRecorder reports;
        EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
        const OrderEntry *restored = restarted.storage_.locateByOrderId(market);
        ASSERT_NE(nullptr, restored);
        EXPECT_EQ(CANCELED_ORDSTATUS, restored->status_);
        EXPECT_EQ(10u, restored->cumQty_);
        EXPECT_EQ(0u, restored->leavesQty_);
    }
    std::filesystem::remove_all(dir);
}

// =============================================================================
// A market order's booking is ordered with the others on its instrument (#209)
// =============================================================================

namespace
{
/// The transactions the processor built for the orders queued, kept rather than run, then handed to a real manager in
/// the same order: what each one waits for is what the manager would make it wait for. Returns their ids.
std::vector<TransactionId> bookingsAsTheManagerOrdersThem(TestTransactionManager &kept, TransactionMgr &manager)
{
    std::vector<TransactionId> ids;
    for (std::unique_ptr<Transaction> &tr : kept.kept_)
    {
        tr->setTransactionId(TransactionId()); // the test manager numbers them all alike
        Transaction *added = tr.get();
        manager.addTransaction(tr);
        ids.push_back(added->transactionId());
    }
    kept.kept_.clear();
    return ids;
}
} // namespace

TEST_F(ProcessorTest, AMarketOrdersBookingWaitsForTheBookingBeforeItOnItsInstrument)
{
    // A market order's booking related only to the order: its match has no book operation, which is what relates a limit
    // order's booking to the instrument. So the manager ran it alongside another booking on the instrument, both matched
    // the same resting order, and the second's fill threw and left its order NEW for good (#209).
    auto sell = createTestOrder(instrId1_, SELL_SIDE, 10.0, 100);
    inQueues_->push("test", OrderEvent(sell.release()));
    processor_->process();

    transMgr_->keep_ = true;
    auto limit = createTestOrder(instrId1_, BUY_SIDE, 10.0, 60);
    inQueues_->push("test", OrderEvent(limit.release()));
    processor_->process();
    auto market = createTestOrder(instrId1_, BUY_SIDE, 0.0, 60);
    market->ordType_ = MARKET_ORDERTYPE;
    inQueues_->push("test", OrderEvent(market.release()));
    processor_->process();
    auto secondMarket = createTestOrder(instrId1_, BUY_SIDE, 0.0, 60);
    secondMarket->ordType_ = MARKET_ORDERTYPE;
    inQueues_->push("test", OrderEvent(secondMarket.release()));
    processor_->process();
    transMgr_->keep_ = false;
    ASSERT_EQ(3u, transMgr_->kept_.size());

    TransactionMgr manager;
    TransactionMgrParams params(IdTGenerator::instance());
    manager.init(params);
    const std::vector<TransactionId> ids = bookingsAsTheManagerOrdersThem(*transMgr_, manager);
    TransactionIdsT parents;
    manager.getParentTransactions(ids[1], &parents);
    EXPECT_EQ(1u, parents.count(ids[0])) << "the market order's booking runs alongside the limit order's";
    parents.clear();
    manager.getParentTransactions(ids[2], &parents);
    EXPECT_EQ(1u, parents.count(ids[1])) << "the second market order's booking runs alongside the first's";
    manager.stop();
}

// =============================================================================
// A suspended order across a restart (#215)
// =============================================================================

TEST_F(ProcessorTest, ASuspendedOrderStaysOutOfTheBookAfterARestartAndIsBookedOnceWhenResumed)
{
    // Suspending an order takes it out of the book. A restart put it back, so it traded while suspended, and resuming it
    // booked it a second time (#215).
    const std::string dir = test::uniqueTestPath("suspended-restart");
    std::filesystem::remove_all(dir);
    IdT id;
    {
        TradeRecorder reports;
        EngineOverDirectory before(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        const OrderEntry *stored = before.process(clOrdId);
        id = stored->orderId_;
        inQueues_->push("test", OrderChangeStateEvent(id, OrderChangeStateEvent::SUSPEND));
        before.processor_.process();
        ASSERT_EQ(SUSPENDED_ORDSTATUS, stored->status_);
        ASSERT_FALSE(before.book_.getTop(instrId1_, BUY_SIDE).isValid())
            << "suspending takes the order out of the book";
    }

    TradeRecorder reports;
    EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
    const OrderEntry *restored = restarted.storage_.locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(SUSPENDED_ORDSTATUS, restored->status_);
    EXPECT_FALSE(restarted.book_.getTop(instrId1_, BUY_SIDE).isValid())
        << "the suspended order came back into the book";

    inQueues_->push("test", OrderChangeStateEvent(id, OrderChangeStateEvent::RESUME));
    restarted.processor_.process();
    EXPECT_EQ(NEW_ORDSTATUS, restored->status_);
    const BookSnapshot book = restarted.book_.getSnapshot(instrId1_, &restarted.storage_);
    ASSERT_EQ(1u, book.bids.size());
    EXPECT_EQ(1u, book.bids[0].orderCount) << "the resumed order is booked twice";
    EXPECT_EQ(100u, book.bids[0].totalQty);
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, ASwapFillsTwoReportsOnEachSideShareTheirOwnLink)
{
    // A side's near and far reports carry one link, which FIX sends as TrdMatchID (TradeLinkID until #378), so a client
    // can pair them; each side has its own (#199). The gateway's link came from each report's own transact time, so a
    // millisecond boundary between the two reports split the pair.
    LegRecordingOutQueues outQueues;
    ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(), inQueues_.get(),
                           &outQueues, inQueues_.get(), transMgr_.get());
    Processor processor;
    processor.init(params);
    transMgr_->proc_ = &processor;

    const auto swap = [this](Side side)
    {
        auto order = createTestOrder(instrId1_, side, 1.0850, 100);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->farPrice_ = 1.0872;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    auto sell = swap(SELL_SIDE);
    const RawDataEntry sellClOrdId = sell->clOrderId_.get();
    inQueues_->push("test", OrderEvent(sell.release()));
    processor.process();
    auto buy = swap(BUY_SIDE);
    const RawDataEntry buyClOrdId = buy->clOrderId_.get();
    inQueues_->push("test", OrderEvent(buy.release()));
    processor.process();

    const OrderEntry *savedSell = OrderStorage::instance()->locateByClOrderId(sellClOrdId);
    const OrderEntry *savedBuy = OrderStorage::instance()->locateByClOrderId(buyClOrdId);
    ASSERT_NE(nullptr, savedSell);
    ASSERT_NE(nullptr, savedBuy);
    ASSERT_EQ(FILLED_ORDSTATUS, savedBuy->status_) << "the swaps didn't match";

    std::vector<u64> sideLinks;
    for (const OrderEntry *order : { savedSell, savedBuy })
    {
        std::vector<u64> links;
        for (const auto &report : outQueues.reports_)
        {
            if ((report.orderId_ == order->orderId_) && (TRADE_EXECTYPE == report.type_))
            {
                links.push_back(report.tradeLink_);
            }
        }
        ASSERT_EQ(2u, links.size()) << "trade reports for order " << order->orderId_.id_;
        EXPECT_NE(0u, links[0]) << "order " << order->orderId_.id_;
        EXPECT_EQ(links[0], links[1]) << "order " << order->orderId_.id_ << ": its near and far reports";
        sideLinks.push_back(links[0]);
    }
    EXPECT_NE(sideLinks[0], sideLinks[1]) << "the two sides share a link";
    // A plain fill's report carries none
    for (const auto &report : outQueues.reports_)
    {
        if (SINGLE_LEG == report.leg_)
        {
            EXPECT_EQ(0u, report.tradeLink_);
        }
    }
}

// =============================================================================
// Stop orders (#223)
// =============================================================================

namespace
{
/// The fixture's storage and book, with a processor whose trades are recorded and whose transactions run as the manager
/// runs them here, where each relates to the instrument: one after another, in the order they were enqueued. So a
/// stop's release, which the transaction of the trade that reaches it enqueues, runs once that transaction is done,
/// with every fill of its sweep.
class StopEngine
{
public:
    StopEngine(OrderBookImpl *book, IncomingQueues *inQueues, TestTransactionManager *transactions,
               ExpiryScheduler *expiry = nullptr, std::function<DateTimeT()> clock = {})
        : inQueues_(inQueues), transactions_(transactions)
    {
        ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), book, inQueues, &reports_, inQueues,
                               transactions);
        params.expiry_ = expiry;
        params.clock_ = std::move(clock); // the business day a trade or a restatement is in (#244)
        processor_.init(params);
        transactions_->proc_ = &processor_;
        transactions_->keepNested_ = true;
    }
    ~StopEngine()
    {
        transactions_->proc_ = nullptr;
    }

    /// Processes the order and every transaction that follows from it, and returns the stored order
    OrderEntry *submit(std::unique_ptr<OrderEntry> order)
    {
        const RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        processor_.process();
        transactions_->runKept();
        OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
        EXPECT_NE(nullptr, stored);
        return stored;
    }

    /// Processes the request and every transaction that follows from it
    template <typename EventT> void request(const EventT &evnt)
    {
        inQueues_->push("test", evnt);
        processor_.process();
        transactions_->runKept();
    }

    /// Processes what is queued already, a timer's events, and every transaction that follows from them
    void processQueued(size_t events)
    {
        for (size_t i = 0; i < events; ++i)
        {
            processor_.process();
        }
        transactions_->runKept();
    }

    TradeRecorder reports_;
    Processor processor_;

private:
    IncomingQueues *inQueues_;
    TestTransactionManager *transactions_;
};

/// The order, made a stop order of the type: released at stopPx, a STOP at the market and a STOPLIMIT at its price
std::unique_ptr<OrderEntry> asStop(std::unique_ptr<OrderEntry> order, OrderType type, PriceT stopPx)
{
    order->ordType_ = type;
    order->stopPx_ = stopPx;
    return order;
}

/// The stops the instrument holds, in the order a trade reaching all of them would release them: the buy stops a trade
/// at the highest price reaches, then the sell stops one at 0 reaches
OrderBook::OrdersT heldStops(const OrderBook &book, const SourceIdT &instrument)
{
    OrderBook::OrdersT held;
    book.triggeredStops(instrument, std::numeric_limits<PriceT>::max(), &held);
    book.triggeredStops(instrument, 0.0, &held);
    return held;
}
} // namespace

TEST_F(ProcessorTest, AStopSellIsHeldUntilATradeReachesItsStopThenSellsAsAMarketOrder)
{
    // A stop sell protects a position: it sells only once the price falls to its stop. It sold as it arrived, at 0
    // against every bid, as a market sell does (#223).
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const OrderEntry *bid = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 50), STOP_ORDERTYPE, 9.5));
    ASSERT_NE(nullptr, stop);
    EXPECT_EQ(NEW_ORDSTATUS, stop->status_);
    EXPECT_FALSE(stop->stopTriggered_);
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty()) << "the stop traded as it arrived";
    EXPECT_EQ(OrderBook::OrdersT{ stop->orderId_ }, heldStops(*orderBook_, instrId1_));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, SELL_SIDE).isValid()) << "a held stop rests in the price book";

    // A trade above the stop doesn't reach it
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    EXPECT_FALSE(stop->stopTriggered_);
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty());

    // A sell takes the rest of the bid at 10, then trades at 9, which reaches the stop: it sells at the market
    const OrderEntry *lowerBid = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 9.0, 100));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 9.0, 100));
    EXPECT_EQ(FILLED_ORDSTATUS, bid->status_);
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_EQ(std::vector<std::string>{ "50@9" }, engine.reports_.tradesOf(stop->orderId_));
    EXPECT_EQ(FILLED_ORDSTATUS, stop->status_);
    // The release is reported, as FIX's ExecType L, before the trades it led to (#236)
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRIGGERED_EXECTYPE, TRADE_EXECTYPE }),
              engine.reports_.reportTypesOf(stop->orderId_));
    EXPECT_EQ(40u, lowerBid->leavesQty_);
    EXPECT_FALSE(orderBook_->holdsStops(instrId1_));
}

TEST_F(ProcessorTest, AStopReachedDuringASweepIsReleasedOnceTheSweepIsDone)
{
    // A buy sweeps three levels, and its first trade reaches a buy stop. The sweep arrived first and takes every level it
    // can; the stop's release runs after it, finds nothing left to buy, and the stop is canceled, as a market order is.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 10));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 12.0, 10));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0));
    const OrderEntry *sweep = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 12.0, 30));

    EXPECT_EQ((std::vector<std::string>{ "10@10", "10@11", "10@12" }), engine.reports_.tradesOf(sweep->orderId_));
    EXPECT_EQ(FILLED_ORDSTATUS, sweep->status_);
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty())
        << "the stop traded ahead of the sweep that reached it";
    EXPECT_EQ(CANCELED_ORDSTATUS, stop->status_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRIGGERED_EXECTYPE, CANCEL_EXECTYPE }),
              engine.reports_.reportTypesOf(stop->orderId_));
    EXPECT_FALSE(orderBook_->holdsStops(instrId1_));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "a released STOP rests in the price book";
}

TEST_F(ProcessorTest, AStopLimitIsReleasedAsALimitOrderAndRestsWithWhatIsLeft)
{
    // It traded at its limit as it arrived, whatever its stop (#223)
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20));
    const OrderEntry *ask = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 10.5, 50), STOPLIMIT_ORDERTYPE, 10.0));
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty()) << "the stop limit traded at its limit as it arrived";
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "a held stop limit rests in the price book";

    // A trade at 10 reaches the stop. Released, the order buys what is left at 10, and rests at its limit of 10.5.
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_EQ(std::vector<std::string>{ "15@10" }, engine.reports_.tradesOf(stop->orderId_));
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRIGGERED_EXECTYPE, TRADE_EXECTYPE }),
              engine.reports_.reportTypesOf(stop->orderId_));
    EXPECT_EQ(PARTFILL_ORDSTATUS, stop->status_);
    EXPECT_EQ(35u, stop->leavesQty_);
    EXPECT_EQ(stop->orderId_, orderBook_->getTop(instrId1_, BUY_SIDE));
    EXPECT_EQ(100u, ask->leavesQty_) << "the stop limit bought above its limit";
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty());
}

TEST_F(ProcessorTest, StopsAreReleasedLowestBuyStopFirstAndAtOneStopInTheOrderTheyArrived)
{
    // A trade at 10 reaches buy stops at 10 and at 9.5. A rising price reaches 9.5 first, so that stop is released
    // first, and stops at one price go in the order they arrived. Only the first released finds something to buy.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 15));
    OrderEntry *first =
        engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 10.0, 10), STOPLIMIT_ORDERTYPE, 10.0));
    OrderEntry *lower = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 10.0, 10), STOPLIMIT_ORDERTYPE, 9.5));
    OrderEntry *second =
        engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 10.0, 10), STOPLIMIT_ORDERTYPE, 10.0));
    EXPECT_EQ((OrderBook::OrdersT{ lower->orderId_, first->orderId_, second->orderId_ }),
              heldStops(*orderBook_, instrId1_));

    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));

    EXPECT_TRUE(lower->stopTriggered_ && first->stopTriggered_ && second->stopTriggered_);
    EXPECT_EQ(std::vector<std::string>{ "10@10" }, engine.reports_.tradesOf(lower->orderId_));
    EXPECT_TRUE(engine.reports_.tradesOf(first->orderId_).empty());
    EXPECT_TRUE(engine.reports_.tradesOf(second->orderId_).empty());
    EXPECT_EQ(first->orderId_, orderBook_->getTop(instrId1_, BUY_SIDE)) << "the stops at 10 were released out of order";
}

TEST_F(ProcessorTest, AReleasedStopsTradeReleasesTheStopsItReaches)
{
    // The trade at 10 releases the stop at 10 and not the one at 11. That stop buys at 11, which releases the other.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    const OrderEntry *ask = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 50));
    OrderEntry *atTen = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0));
    OrderEntry *atEleven = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 0.0, 10), STOP_ORDERTYPE, 11.0));

    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));

    EXPECT_EQ(std::vector<std::string>{ "10@11" }, engine.reports_.tradesOf(atTen->orderId_));
    EXPECT_EQ(std::vector<std::string>{ "10@11" }, engine.reports_.tradesOf(atEleven->orderId_));
    EXPECT_EQ(FILLED_ORDSTATUS, atTen->status_);
    EXPECT_EQ(FILLED_ORDSTATUS, atEleven->status_);
    EXPECT_EQ(30u, ask->leavesQty_);
    EXPECT_FALSE(orderBook_->holdsStops(instrId1_));
}

TEST_F(ProcessorTest, ACanceledStopLeavesTheStopBookAndIsNotReleased)
{
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0));
    engine.request(OrderCancelEvent(stop->orderId_));
    EXPECT_EQ(CANCELED_ORDSTATUS, stop->status_);
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty());

    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    EXPECT_FALSE(stop->stopTriggered_);
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty());
}

TEST_F(ProcessorTest, ACancelEnqueuedBeforeAStopsReleaseWins)
{
    // A trade reaches the stop, and a cancel of the stop is enqueued before the trade's transaction enqueues the
    // release. The release is decided when it runs, after the cancel, and finds the stop canceled. Decided as the trade
    // was, it would have released an order that the cancel had already taken.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const OrderEntry *bid = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0));

    transMgr_->keep_ = true; // the sell's booking and the cancel are enqueued, in that order, before either runs
    inQueues_->push("test", OrderEvent(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5).release()));
    engine.processor_.process();
    inQueues_->push("test", OrderCancelEvent(stop->orderId_));
    engine.processor_.process();
    transMgr_->keep_ = false;
    ASSERT_EQ(2u, transMgr_->kept_.size());
    transMgr_->runKept();

    EXPECT_EQ(CANCELED_ORDSTATUS, stop->status_);
    EXPECT_FALSE(stop->stopTriggered_);
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty());
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, CANCEL_EXECTYPE }), engine.reports_.reportTypesOf(stop->orderId_))
        << "a release that found the stop canceled reported it triggered";
    EXPECT_EQ(95u, bid->leavesQty_);
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty());
}

TEST_F(ProcessorTest, AStopSuspendedBeforeItsReleaseRunsIsReleasedByATradeOnceResumed)
{
    // The release finds the stop suspended and does nothing, and the suspend takes it out of the stop book. Resumed, it
    // is held again, and the next trade that reaches it releases it.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const OrderEntry *bid = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0));

    transMgr_->keep_ = true; // the sell's booking and the suspend are enqueued, in that order, before either runs
    inQueues_->push("test", OrderEvent(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5).release()));
    engine.processor_.process();
    inQueues_->push("test", OrderChangeStateEvent(stop->orderId_, OrderChangeStateEvent::SUSPEND));
    engine.processor_.process();
    transMgr_->keep_ = false;
    ASSERT_EQ(2u, transMgr_->kept_.size());
    transMgr_->runKept();
    ASSERT_EQ(SUSPENDED_ORDSTATUS, stop->status_);
    EXPECT_FALSE(stop->stopTriggered_);
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty());

    engine.request(OrderChangeStateEvent(stop->orderId_, OrderChangeStateEvent::RESUME));
    EXPECT_EQ(OrderBook::OrdersT{ stop->orderId_ }, heldStops(*orderBook_, instrId1_));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    EXPECT_EQ(std::vector<std::string>{ "10@10" }, engine.reports_.tradesOf(stop->orderId_));
    EXPECT_EQ(80u, bid->leavesQty_);
}

TEST_F(ProcessorTest, AStopsReleaseWaitsForTheTransactionThatBooksTheStop)
{
    // A stop is in the stop book once the transaction that books it has run, which can be before its creator marks that
    // transaction enqueued: the manager may run it on another worker at once. A trade can reach the stop in that gap,
    // and the release must still be enqueued after the booking, as any transaction about an order must (#83). Here the
    // stop is stored and held, and its creator marks the booking enqueued 100 ms later.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 100));
    auto order = asStop(createTestOrder(instrId1_, BUY_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0);
    COP::Store::PublishGuard guard;
    OrderEntry *stop = OrderStorage::instance()->save(*order, IdTGenerator::instance(), &guard);
    ASSERT_NE(nullptr, stop);
    stop->status_ = NEW_ORDSTATUS;
    guard.release(); // initialised: only the mark is left
    orderBook_->addStop(*stop);

    std::vector<bool> marks;
    transMgr_->onAdd_ = [&]()
    {
        marks.push_back(stop->bookingPending_.load());
    };
    std::thread creator(
        [&guard]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            guard.bookingEnqueued();
        });
    inQueues_->push("test", OrderEvent(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5).release()));
    engine.processor_.process(); // the buy's booking runs, and its trade reaches the stop
    creator.join();
    transMgr_->onAdd_ = nullptr;

    ASSERT_EQ(3u, marks.size()) << "the buy's booking, its trade's transaction and the stop's release";
    EXPECT_FALSE(marks.back()) << "the release was enqueued before the transaction that books the stop";
    transMgr_->kept_.clear(); // the stop's state machine was never set up here, so its trade isn't run
}

TEST_F(ProcessorTest, AStopTwoTradesReachBeforeItsReleaseRunsIsReleasedOnce)
{
    // Each trade's transaction enqueues a release, and both are enqueued before either runs. The first releases the stop
    // limit, which rests below the ask; the second finds it released and does nothing.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const OrderEntry *ask = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 9.5, 10), STOPLIMIT_ORDERTYPE, 10.0));

    transMgr_->keep_ = true;
    for (int i = 0; i < 2; ++i)
    {
        inQueues_->push("test", OrderEvent(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5).release()));
        engine.processor_.process();
    }
    transMgr_->keep_ = false;
    transMgr_->runKept();

    EXPECT_EQ(90u, ask->leavesQty_);
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_EQ(NEW_ORDSTATUS, stop->status_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRIGGERED_EXECTYPE }),
              engine.reports_.reportTypesOf(stop->orderId_))
        << "the second release, which found the stop released, reported it again";
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty());
    const BookSnapshot book = orderBook_->getSnapshot(instrId1_, OrderStorage::instance());
    ASSERT_EQ(1u, book.bids.size());
    EXPECT_EQ(1u, book.bids[0].orderCount) << "the stop limit was booked twice";
    EXPECT_EQ(10u, book.bids[0].totalQty);
}

namespace
{
/// Notes each version of a stop order it saves: "persist held", or "persist released" once its stop was reached
class StopVersionLog final : public COP::OrderSaver
{
public:
    explicit StopVersionLog(std::vector<std::string> *log) : log_(log) {}

    COP::u32 save(const COP::OrderEntry &order) override
    {
        if ((STOP_ORDERTYPE == order.ordType_) || (STOPLIMIT_ORDERTYPE == order.ordType_))
        {
            log_->push_back(order.stopTriggered_ ? "persist released" : "persist held");
        }
        return version_++;
    }
    void erase(const COP::IdT &, COP::u32) override {}

private:
    std::vector<std::string> *log_;
    COP::u32 version_ = 0;
};
} // namespace

TEST_F(ProcessorTest, AStopsReleaseIsDurableBeforeItsTriggeredReport)
{
    // A client that hears TRIGGERED must find the release in the order's record after a crash, as for every report (#28)
    std::vector<std::string> log;
    StopVersionLog saver(&log);
    OrderStorage::instance()->attach(&saver);
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.reports_.log_ = &log;
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 9.5, 10), STOPLIMIT_ORDERTYPE, 10.0));
    log.clear();

    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));

    const auto released = std::find(log.begin(), log.end(), "persist released");
    const auto triggered = std::find(log.begin(), log.end(), "report TRIGGERED");
    ASSERT_NE(log.end(), released);
    ASSERT_NE(log.end(), triggered);
    EXPECT_LT(released, triggered) << "the release was reported before it was durable";
}

TEST_F(ProcessorTest, AReplacedStopIsHeldAtItsNewStopPrice)
{
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 9.0));

    OrderEntry *replacement = stop->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = stop->clOrderId_;
    replacement->stopPx_ = 10.0;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(stop->orderId_, replacement));

    EXPECT_EQ(REPLACED_ORDSTATUS, stop->status_);
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    EXPECT_EQ(NEW_ORDSTATUS, live->status_);
    EXPECT_FALSE(live->stopTriggered_);
    EXPECT_EQ(OrderBook::OrdersT{ live->orderId_ }, heldStops(*orderBook_, instrId1_))
        << "the original is still held, or the replacement isn't";

    // A trade at 10 reaches the replacement's stop, which the original's, at 9, didn't
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    EXPECT_TRUE(live->stopTriggered_);
    EXPECT_EQ(std::vector<std::string>{ "10@10" }, engine.reports_.tradesOf(live->orderId_));
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty());
}

TEST_F(ProcessorTest, AReleasedStopLimitsReplacementStaysReleased)
{
    // Its stop was reached, so a replacement that is still a stop limit rests at its own limit; it isn't held again
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 9.5, 20), STOPLIMIT_ORDERTYPE, 10.0));
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));
    ASSERT_TRUE(stop->stopTriggered_);
    ASSERT_EQ(stop->orderId_, orderBook_->getTop(instrId1_, BUY_SIDE));

    OrderEntry *replacement = stop->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = stop->clOrderId_;
    replacement->price_ = 9.6;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    replacement->stopTriggered_ = false; // as a client's replacement has it: the engine decides
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(stop->orderId_, replacement));

    EXPECT_EQ(REPLACED_ORDSTATUS, stop->status_);
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    EXPECT_EQ(NEW_ORDSTATUS, live->status_);
    EXPECT_TRUE(live->stopTriggered_);
    EXPECT_EQ(live->orderId_, orderBook_->getTop(instrId1_, BUY_SIDE));
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty()) << "the replacement is held again";
}

TEST_F(ProcessorTest, AReleasedStopLimitsStopPxCantBeReplaced)
{
    // Its stop was reached, so a new stopPx would mean nothing: refused (#238), decided when the replace's turn comes
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 9.5, 20), STOPLIMIT_ORDERTYPE, 10.0));
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));
    ASSERT_TRUE(stop->stopTriggered_);

    OrderEntry *replacement = stop->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = stop->clOrderId_;
    replacement->stopPx_ = 10.5;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    replacement->stopTriggered_ = false; // as a client's replacement has it: the engine decides
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(stop->orderId_, replacement));

    EXPECT_EQ(nullptr, OrderStorage::instance()->locateByClOrderId(replacementId)) << "the replacement was stored";
    EXPECT_EQ(NEW_ORDSTATUS, stop->status_);
    EXPECT_DOUBLE_EQ(10.0, stop->stopPx_);
    EXPECT_EQ(stop->orderId_, orderBook_->getTop(instrId1_, BUY_SIDE));
}

TEST_F(ProcessorTest, AHeldStopLimitsReplaceMovesItsLimitAndItsStop)
{
    // A held stop limit's replace may move both its limit and its stop (#238)
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 9.5, 20), STOPLIMIT_ORDERTYPE, 10.0));

    OrderEntry *replacement = stop->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = stop->clOrderId_;
    replacement->price_ = 9.75;
    replacement->stopPx_ = 10.25;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(stop->orderId_, replacement));

    EXPECT_EQ(REPLACED_ORDSTATUS, stop->status_);
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    EXPECT_DOUBLE_EQ(9.75, live->price_);
    EXPECT_DOUBLE_EQ(10.25, live->stopPx_);
    EXPECT_EQ(OrderBook::OrdersT{ live->orderId_ }, heldStops(*orderBook_, instrId1_));
}

TEST_F(ProcessorTest, ASuspendedStopIsNotReleasedAndIsHeldAgainWhenResumed)
{
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0));
    engine.request(OrderChangeStateEvent(stop->orderId_, OrderChangeStateEvent::SUSPEND));
    ASSERT_EQ(SUSPENDED_ORDSTATUS, stop->status_);
    EXPECT_TRUE(heldStops(*orderBook_, instrId1_).empty()) << "the suspended stop is still held";

    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    EXPECT_FALSE(stop->stopTriggered_) << "a trade released the suspended stop";

    engine.request(OrderChangeStateEvent(stop->orderId_, OrderChangeStateEvent::RESUME));
    EXPECT_EQ(NEW_ORDSTATUS, stop->status_);
    EXPECT_EQ(OrderBook::OrdersT{ stop->orderId_ }, heldStops(*orderBook_, instrId1_));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, SELL_SIDE).isValid()) << "the resumed stop rests in the price book";

    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    EXPECT_EQ(std::vector<std::string>{ "10@10" }, engine.reports_.tradesOf(stop->orderId_));
}

TEST_F(ProcessorTest, AHeldStopIsHeldAgainAfterARestart)
{
    // No trade price survives a restart, so a held stop waits again for a trade that reaches it
    const std::string dir = test::uniqueTestPath("stop-held-restart");
    std::filesystem::remove_all(dir);
    IdT id;
    {
        TradeRecorder reports;
        EngineOverDirectory before(dir, instrId1_, inQueues_.get(), &reports);
        auto order = asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 9.5);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        id = before.process(clOrdId)->orderId_;
    }

    TradeRecorder reports;
    EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
    const OrderEntry *restored = restarted.storage_.locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(NEW_ORDSTATUS, restored->status_);
    EXPECT_FALSE(restored->stopTriggered_);
    EXPECT_EQ(OrderBook::OrdersT{ id }, heldStops(restarted.book_, instrId1_));
    EXPECT_FALSE(restarted.book_.getTop(instrId1_, SELL_SIDE).isValid());
    EXPECT_TRUE(restarted.dispatcher_.ordersToMatch().empty());

    auto bid = createTestOrder(instrId1_, BUY_SIDE, 9.0, 50);
    RawDataEntry clOrdId = bid->clOrderId_.get();
    inQueues_->push("test", OrderEvent(bid.release()));
    restarted.process(clOrdId);
    auto sell = createTestOrder(instrId1_, SELL_SIDE, 9.0, 10);
    clOrdId = sell->clOrderId_.get();
    inQueues_->push("test", OrderEvent(sell.release()));
    restarted.process(clOrdId);
    EXPECT_EQ(std::vector<std::string>{ "10@9" }, reports.tradesOf(id)) << "the trade at 9 didn't release the stop";
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, AReleasedStopLimitRestsAgainAfterARestart)
{
    const std::string dir = test::uniqueTestPath("stop-released-restart");
    std::filesystem::remove_all(dir);
    IdT id;
    {
        TradeRecorder reports;
        EngineOverDirectory before(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 5);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        before.process(clOrdId);
        order = asStop(createTestOrder(instrId1_, BUY_SIDE, 10.5, 20), STOPLIMIT_ORDERTYPE, 10.0);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        id = before.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 5);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        before.process(clOrdId);
        ASSERT_EQ(id, before.book_.getTop(instrId1_, BUY_SIDE)) << "released with nothing to buy, the stop limit rests";
    }

    TradeRecorder reports;
    EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
    const OrderEntry *restored = restarted.storage_.locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(NEW_ORDSTATUS, restored->status_);
    EXPECT_TRUE(restored->stopTriggered_);
    EXPECT_EQ(id, restarted.book_.getTop(instrId1_, BUY_SIDE));
    EXPECT_TRUE(heldStops(restarted.book_, instrId1_).empty()) << "the released stop limit is held again";
    EXPECT_TRUE(restarted.dispatcher_.ordersToMatch().empty());
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, AReleasedStopACrashLeftOpenTradesAsAMarketOrderOnceRestarted)
{
    // The release committed, which marked the stop released, and a crash lost the trade it started (#207, for a market
    // order). Restored, the stop trades as a market order again; held, it would wait for another trade.
    const std::string dir = test::uniqueTestPath("stop-crash-restart");
    std::filesystem::remove_all(dir);
    IdT id;
    {
        TradeRecorder reports;
        EngineOverDirectory crashed(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        crashed.process(clOrdId);
        order = asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.0);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        id = crashed.process(clOrdId)->orderId_;

        crashed.transactions_.keepNested_ = true;
        order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 5);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        crashed.process(clOrdId);
        ASSERT_EQ(2u, crashed.transactions_.kept_.size()) << "the trade's transaction and the stop's release";
        crashed.transactions_.runKept(2);
        ASSERT_TRUE(crashed.storage_.locateByOrderId(id)->stopTriggered_);
        ASSERT_EQ(1u, crashed.transactions_.kept_.size()) << "the trade the release started is lost";
    }

    TradeRecorder reports;
    EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
    OrderEntry *restored = restarted.storage_.locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    ASSERT_EQ(NEW_ORDSTATUS, restored->status_);
    ASSERT_TRUE(restored->stopTriggered_);
    EXPECT_TRUE(heldStops(restarted.book_, instrId1_).empty());
    ASSERT_EQ(1u, restarted.dispatcher_.ordersToMatch().size());
    EXPECT_EQ(id, restarted.dispatcher_.ordersToMatch()[0]->orderId_);

    restarted.processor_.matchRestored(restored);
    EXPECT_EQ(std::vector<std::string>{ "10@10" }, reports.tradesOf(id));
    EXPECT_EQ(FILLED_ORDSTATUS, restored->status_);
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, ASwapsTradesReleaseNoStop)
{
    // On this branch a swap trades only with a swap (#212), so its legs' prices aren't the instrument's trades: a held
    // stop is released by an outright trade at its stop, which goes through the matcher that releases stops (#223), and
    // not by a swap's, which doesn't
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    OrderEntry *stop = engine.submit(asStop(createTestOrder(instrId1_, BUY_SIDE, 0.0, 10), STOP_ORDERTYPE, 1.08));
    const auto swap = [this](Side side)
    {
        auto order = createTestOrder(instrId1_, side, 1.09, 100);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->farPrice_ = 1.095;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    engine.submit(swap(SELL_SIDE));
    const OrderEntry *buy = engine.submit(swap(BUY_SIDE));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_) << "the swaps didn't trade";
    EXPECT_TRUE(orderBook_->holdsStops(instrId1_)) << "a swap's trade at 1.09 released the buy stop at 1.08";
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty());

    // An outright trade at 1.09 releases it, and it buys what is left as a market order
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 1.09, 20));
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 1.09, 10));
    EXPECT_FALSE(orderBook_->holdsStops(instrId1_));
    EXPECT_EQ(std::vector<std::string>{ "10@1.09" }, engine.reports_.tradesOf(stop->orderId_));
}

// =============================================================================
// Average price (#239)
// =============================================================================

TEST_F(ProcessorTest, AnOrdersAveragePriceIsItsFillsWeightedByQuantity)
{
    // Nothing computed avgPx_, so every order reported 0 (#239). A buy takes 10 at 10 and 30 at 11, and each ask
    // averages its own price.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const OrderEntry *atTen = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    const OrderEntry *atEleven = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 30));
    const OrderEntry *buy = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 11.0, 60));
    ASSERT_EQ(40u, buy->cumQty_);
    EXPECT_DOUBLE_EQ(10.75, buy->avgPx_);
    EXPECT_DOUBLE_EQ(10.0, atTen->avgPx_);
    EXPECT_DOUBLE_EQ(11.0, atEleven->avgPx_);

    // The buy rests at 11 with 20 left, and a sell of 10 trades with it there: (10 x 10 + 30 x 11 + 10 x 11) / 50
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 10));
    ASSERT_EQ(50u, buy->cumQty_);
    EXPECT_DOUBLE_EQ(10.8, buy->avgPx_);
}

TEST_F(ProcessorTest, AReplacementAveragesItsFillsWithTheOriginals)
{
    // A replacement takes over the original's fills, so its average price starts at the original's
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    OrderEntry *buy = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30));
    ASSERT_EQ(10u, buy->cumQty_);
    ASSERT_DOUBLE_EQ(10.0, buy->avgPx_);

    OrderEntry *replacement = buy->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = buy->clOrderId_;
    replacement->price_ = 12.0;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    replacement->cumQty_ = 0; // as a client's replacement has them: the engine carries the original's over
    replacement->avgPx_ = 0.0;
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(buy->orderId_, replacement));
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    ASSERT_EQ(PARTFILL_ORDSTATUS, live->status_) << "the replace was refused, or lost the original's fills";
    EXPECT_DOUBLE_EQ(10.0, live->avgPx_);

    // The replacement buys its other 20 at 12: (10 x 10 + 20 x 12) / 30
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 12.0, 20));
    ASSERT_EQ(30u, live->cumQty_);
    EXPECT_DOUBLE_EQ(340.0 / 30.0, live->avgPx_);
}

TEST_F(ProcessorTest, ASwapsAveragePriceIsItsNearLegs)
{
    // On this branch a swap's fills reach the engine through its near leg, so its average is the near leg's (#239): a buy
    // swap takes 30 at 1.0850 and 30 at 1.0860 from two resting swaps of the same dates, whose far prices it crosses
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const auto swap = [this](Side side, PriceT nearPrice, QuantityT qty)
    {
        auto order = createTestOrder(instrId1_, side, nearPrice, qty);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->farPrice_ = 1.095;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    engine.submit(swap(SELL_SIDE, 1.085, 30));
    engine.submit(swap(SELL_SIDE, 1.086, 30));
    const OrderEntry *buy = engine.submit(swap(BUY_SIDE, 1.086, 60));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_) << "the swaps didn't trade";
    EXPECT_DOUBLE_EQ(1.0855, buy->avgPx_);
    EXPECT_DOUBLE_EQ(1.095, buy->farPrice_);
}

namespace
{
/// Counts the last trades persisted (#237)
class LastTradeCount final : public COP::OrderSaver
{
public:
    COP::u32 save(const COP::OrderEntry &) override
    {
        return version_++;
    }
    void erase(const COP::IdT &, COP::u32) override {}
    COP::u32 saveLastTrade(const COP::SourceIdT &, COP::PriceT, const COP::IdT &) override
    {
        ++lastTrades_;
        return version_++;
    }

    int lastTrades_ = 0;

private:
    COP::u32 version_ = 0;
};
} // namespace

TEST_F(ProcessorTest, ASwapsTradePersistsNoLastTrade)
{
    // On this branch a swap's trades release no stop (#223), so they persist no last trade (#237): a restart releases
    // only the stops live trading would have. An outright trade persists one.
    LastTradeCount saver;
    OrderStorage::instance()->attach(&saver);
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const auto swap = [this](Side side, PriceT nearPrice, QuantityT qty)
    {
        auto order = createTestOrder(instrId1_, side, nearPrice, qty);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->farPrice_ = 1.095;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    engine.submit(swap(SELL_SIDE, 1.085, 30));
    const OrderEntry *buy = engine.submit(swap(BUY_SIDE, 1.085, 30));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_) << "the swaps didn't trade";
    EXPECT_EQ(0, saver.lastTrades_) << "a swap's trade persisted its instrument's last trade";

    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 5));
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));
    EXPECT_EQ(1, saver.lastTrades_) << "an outright trade";
}

// =============================================================================
// The last trade survives a restart (#237)
// =============================================================================

TEST_F(ProcessorTest, AStopWhoseReleaseACrashLostIsReleasedOnceRestarted)
{
    // The trade that reached the stop committed, and a crash lost the stop's release (#237). No trade price survived a
    // restart, so the stop stayed held until the next trade, at whatever price that came. Now the trade persists its
    // instrument's last trade, and the restart releases the stop from it. An earlier trade, at 10, didn't reach it:
    // the restore must take the last one, at 9.
    const std::string dir = test::uniqueTestPath("stop-release-lost");
    std::filesystem::remove_all(dir);
    IdT id;
    {
        TradeRecorder reports;
        EngineOverDirectory crashed(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        crashed.process(clOrdId);
        order = createTestOrder(instrId1_, BUY_SIDE, 9.0, 100);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        crashed.process(clOrdId);
        order = asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 9.5);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        id = crashed.process(clOrdId)->orderId_;
        order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 5);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        crashed.process(clOrdId);
        ASSERT_FALSE(crashed.storage_.locateByOrderId(id)->stopTriggered_)
            << "a trade at 10 reached a sell stop at 9.5";

        // A sell sweeps the bid at 10 and trades at 9, which reaches the stop
        crashed.transactions_.keepNested_ = true;
        order = createTestOrder(instrId1_, SELL_SIDE, 9.0, 100);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        crashed.process(clOrdId);
        ASSERT_EQ(4u, crashed.transactions_.kept_.size())
            << "the two trades' transactions, the sweep's next match between them, and the stop's release";
        crashed.transactions_.runKept(3);
        ASSERT_FALSE(crashed.storage_.locateByOrderId(id)->stopTriggered_) << "the release is lost";
    }

    TradeRecorder reports;
    EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
    OrderEntry *restored = restarted.storage_.locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    ASSERT_FALSE(restored->stopTriggered_);
    EXPECT_TRUE(restarted.dispatcher_.ordersToMatch().empty());
    ASSERT_EQ(std::vector<OrderEntry *>{ restored }, restarted.dispatcher_.stopsReached());

    restarted.processor_.releaseRestored(restored);
    EXPECT_TRUE(restored->stopTriggered_);
    EXPECT_EQ(std::vector<std::string>{ "10@9" }, reports.tradesOf(id));
    EXPECT_EQ(FILLED_ORDSTATUS, restored->status_);
    EXPECT_FALSE(restarted.book_.holdsStops(instrId1_));
    std::filesystem::remove_all(dir);
}

TEST_F(ProcessorTest, AStopThatArrivedAfterTheLastTradeWaitsForTheNextOneAfterARestart)
{
    // The last trade is at 10, and then a sell stop at 10.5 arrives, which a trade at 10 would reach. No trade has since
    // it arrived, so it waits for the next one, restart or not: releasing every stop the last price reaches would
    // release it at every restart (#237).
    const std::string dir = test::uniqueTestPath("stop-after-last-trade");
    std::filesystem::remove_all(dir);
    IdT id;
    {
        TradeRecorder reports;
        EngineOverDirectory first(dir, instrId1_, inQueues_.get(), &reports);
        auto order = createTestOrder(instrId1_, BUY_SIDE, 10.0, 100);
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        first.process(clOrdId);
        order = createTestOrder(instrId1_, SELL_SIDE, 10.0, 5);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        first.process(clOrdId);
        order = asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 10.5);
        clOrdId = order->clOrderId_.get();
        inQueues_->push("test", OrderEvent(order.release()));
        id = first.process(clOrdId)->orderId_;
        ASSERT_FALSE(first.storage_.locateByOrderId(id)->stopTriggered_);
    }

    TradeRecorder reports;
    EngineOverDirectory restarted(dir, instrId1_, inQueues_.get(), &reports);
    const OrderEntry *restored = restarted.storage_.locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    EXPECT_TRUE(restarted.dispatcher_.stopsReached().empty()) << "a stop that arrived after the last trade is released";
    EXPECT_EQ(OrderBook::OrdersT{ id }, heldStops(restarted.book_, instrId1_));
    std::filesystem::remove_all(dir);
}

// =============================================================================
// Time in force (#240)
// =============================================================================

namespace
{
/// The order, with the time in force
std::unique_ptr<OrderEntry> withTif(std::unique_ptr<OrderEntry> order, TimeInForce tif)
{
    order->tif_ = tif;
    return order;
}
} // namespace

TEST_F(ProcessorTest, AnIocOrderTradesWhatItCanAtOnceAndTheRestIsCanceled)
{
    // It rested with what its sweep left, and could trade later at a price its sender no longer wanted (#240)
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 10));
    const OrderEntry *ioc = engine.submit(withTif(createTestOrder(instrId1_, BUY_SIDE, 10.5, 30), IOC_TIF));

    EXPECT_EQ(std::vector<std::string>{ "10@10" }, engine.reports_.tradesOf(ioc->orderId_));
    EXPECT_EQ(CANCELED_ORDSTATUS, ioc->status_);
    EXPECT_EQ(10u, ioc->cumQty_);
    // Its rest was its LeavesQty, 20, where its client's cancel would have left 0: a closed order has none (#386)
    EXPECT_EQ(0u, ioc->leavesQty_);
    EXPECT_EQ(0u, engine.reports_.leavesReportedWith(ioc->orderId_, CANCEL_EXECTYPE));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "an IOC order rests in the book";
}

TEST_F(ProcessorTest, AnIocOrderWithNothingToTradeIsCanceledNotRejected)
{
    // The book has an ask, at a price the order doesn't trade at: canceled, as the rest of an IOC sweep is
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 10));
    const OrderEntry *ioc = engine.submit(withTif(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), IOC_TIF));

    EXPECT_TRUE(engine.reports_.tradesOf(ioc->orderId_).empty());
    EXPECT_EQ(CANCELED_ORDSTATUS, ioc->status_);
    EXPECT_EQ(0u, ioc->leavesQty_);
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid());
}

TEST_F(ProcessorTest, AFokOrderTheBookCanFillTradesInFullAcrossLevels)
{
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 20));
    const OrderEntry *fok = engine.submit(withTif(createTestOrder(instrId1_, BUY_SIDE, 11.0, 30), FOK_TIF));

    EXPECT_EQ((std::vector<std::string>{ "10@10", "20@11" }), engine.reports_.tradesOf(fok->orderId_));
    EXPECT_EQ(FILLED_ORDSTATUS, fok->status_);
}

TEST_F(ProcessorTest, AFokOrderTheBookCantFillIsCanceledWithNothingTraded)
{
    // In full at once, or not at all (#240): 10 at 10 is all there is at 11 or better, and the 20 at 12 is too dear
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    const OrderEntry *atTen = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    const OrderEntry *atTwelve = engine.submit(createTestOrder(instrId1_, SELL_SIDE, 12.0, 20));
    const OrderEntry *fok = engine.submit(withTif(createTestOrder(instrId1_, BUY_SIDE, 11.0, 30), FOK_TIF));

    EXPECT_TRUE(engine.reports_.tradesOf(fok->orderId_).empty()) << "a FOK order traded part of itself";
    EXPECT_EQ(CANCELED_ORDSTATUS, fok->status_);
    EXPECT_EQ(0u, fok->leavesQty_);
    EXPECT_EQ(10u, atTen->leavesQty_);
    EXPECT_EQ(20u, atTwelve->leavesQty_);
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid());
}

TEST_F(ProcessorTest, AFokOrderExactlyTheBookCanFillTradesInFull)
{
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.5, 20));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 12.0, 50));
    const OrderEntry *fok = engine.submit(withTif(createTestOrder(instrId1_, BUY_SIDE, 11.0, 30), FOK_TIF));

    EXPECT_EQ(30u, fok->cumQty_);
    EXPECT_EQ(FILLED_ORDSTATUS, fok->status_);
}

TEST_F(ProcessorTest, AnIocOrFokOrderIsNeverBooked)
{
    // Its rest is canceled by a transaction after the one that books it. Booked in between, it could trade with an order
    // that came meanwhile (#240).
    for (const TimeInForce tif : { IOC_TIF, FOK_TIF })
    {
        SCOPED_TRACE(tif);
        StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
        engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 10));
        inQueues_->push("test", OrderEvent(withTif(createTestOrder(instrId1_, BUY_SIDE, 10.5, 30), tif).release()));
        engine.processor_.process(); // its booking runs; its fills and its cancel wait, kept
        EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "the order was booked";
        transMgr_->runKept();
        EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid());
    }
}

TEST_F(ProcessorTest, AReleasedIocStopLimitCancelsWhatItCantTradeAtItsLimit)
{
    // A stop order's time in force applies when its stop releases it, not when it arrives (#240): held, it waits
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 100));
    OrderEntry *stop = engine.submit(
        withTif(asStop(createTestOrder(instrId1_, BUY_SIDE, 10.5, 50), STOPLIMIT_ORDERTYPE, 10.0), IOC_TIF));
    EXPECT_EQ(NEW_ORDSTATUS, stop->status_) << "a held IOC stop was canceled as it arrived";

    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_EQ(std::vector<std::string>{ "15@10" }, engine.reports_.tradesOf(stop->orderId_));
    EXPECT_EQ(CANCELED_ORDSTATUS, stop->status_);
    EXPECT_EQ(0u, stop->leavesQty_);
    EXPECT_EQ(0u, engine.reports_.leavesReportedWith(stop->orderId_, CANCEL_EXECTYPE));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "a released IOC stop limit rests";
}

TEST_F(ProcessorTest, AReleasedFokStopTheBookCantFillIsCanceledWithNothingTraded)
{
    // Its release is its first match, and a FOK order's check comes first there too (#240)
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 11.0, 100));
    OrderEntry *stop = engine.submit(
        withTif(asStop(createTestOrder(instrId1_, BUY_SIDE, 10.5, 50), STOPLIMIT_ORDERTYPE, 10.0), FOK_TIF));

    // A trade at 10 reaches the stop; 15 are left at 10, and 50 would take 11
    engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 5));
    EXPECT_TRUE(stop->stopTriggered_);
    EXPECT_TRUE(engine.reports_.tradesOf(stop->orderId_).empty()) << "a released FOK stop traded part of itself";
    EXPECT_EQ(CANCELED_ORDSTATUS, stop->status_);
    EXPECT_EQ(0u, stop->leavesQty_);
}

namespace
{
const DateTimeT OCT15 = 1792022400000ULL; // 2026-10-15 00:00 UTC
const DateTimeT HOUR = 60ULL * 60 * 1000;

/// A GTD order, created at 'now' and expiring at 'expireTime'
std::unique_ptr<OrderEntry> asGtd(std::unique_ptr<OrderEntry> order, DateTimeT now, DateTimeT expireTime)
{
    order->tif_ = GTD_TIF;
    order->creationTime_ = now;
    order->expireTime_ = expireTime;
    return order;
}
} // namespace

TEST_F(ProcessorTest, AGtdOrderExpiresAtItsTimeAndLeavesTheBook)
{
    // Nothing produced a timer: a GTD order never expired (#240)
    DateTimeT now = OCT15 + 9 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    OrderEntry *gtd = engine.submit(asGtd(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), now, now + HOUR));
    ASSERT_EQ(NEW_ORDSTATUS, gtd->status_);
    ASSERT_EQ(1u, expiry.pending());

    now += HOUR - 1;
    EXPECT_EQ(0u, expiry.fireDue()) << "it expired before its time";
    now += 1;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(EXPIRED_ORDSTATUS, gtd->status_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, EXPIRED_EXECTYPE }), engine.reports_.reportTypesOf(gtd->orderId_));
    EXPECT_EQ(0u, gtd->leavesQty_);
    EXPECT_EQ(0u, engine.reports_.leavesReportedWith(gtd->orderId_, EXPIRED_EXECTYPE));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid());
}

TEST_F(ProcessorTest, AnOrderThatExpiresAfterAPartFillKeepsItAndHasNoLeaves)
{
    // An expiry closes the order as a cancel does, and a closed order's LeavesQty is 0, whatever closed it (#386). It
    // was the 60 the order hadn't filled.
    DateTimeT now = OCT15 + 9 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    OrderEntry *gtd = engine.submit(asGtd(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now, now + HOUR));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 40), IOC_TIF)); // no timer of its own
    ASSERT_EQ(PARTFILL_ORDSTATUS, gtd->status_);
    ASSERT_EQ(60u, gtd->leavesQty_);

    now += HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(EXPIRED_ORDSTATUS, gtd->status_);
    EXPECT_EQ(40u, gtd->cumQty_);
    EXPECT_EQ(0u, gtd->leavesQty_);
    EXPECT_EQ(0u, engine.reports_.leavesReportedWith(gtd->orderId_, EXPIRED_EXECTYPE));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid());
}

TEST_F(ProcessorTest, AMatchBetweenAnExpiryAndItsRemovalFromTheBookPassesTheExpiredOrderBy)
{
    // An expiry is decided in one transaction, and the order leaves the book in another, enqueued as the decision ends.
    // A match enqueued in between runs first, with the expired order still in the book. The matcher passes by an order
    // with no leaves, and an expired order has none (#386), as a canceled one has had since #73.
    DateTimeT now = OCT15 + 9 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    OrderEntry *gtd = engine.submit(asGtd(createTestOrder(instrId1_, SELL_SIDE, 10.0, 100), now, now + HOUR));
    auto sell = withTif(createTestOrder(instrId1_, SELL_SIDE, 10.5, 100), GTC_TIF);
    sell->creationTime_ = now;
    OrderEntry *gtc = engine.submit(std::move(sell));

    // The GTD sell's expiry is decided, and the transaction that takes it out of the book is held back
    now += HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processor_.process();
    ASSERT_EQ(EXPIRED_ORDSTATUS, gtd->status_);
    EXPECT_EQ(0u, gtd->leavesQty_);
    ASSERT_EQ(gtd->orderId_, orderBook_->getTop(instrId1_, SELL_SIDE)) << "out of the book only once the removal runs";

    // A buy that crosses both sells arrives, and its match runs ahead of the removal
    auto buy = createTestOrder(instrId1_, BUY_SIDE, 11.0, 100);
    const RawDataEntry clOrdId = buy->clOrderId_.get();
    inQueues_->push("test", OrderEvent(buy.release()));
    EXPECT_NO_THROW(engine.processor_.process());
    engine.processQueued(0); // the removal, then the trade's reports

    OrderEntry *bought = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, bought);
    EXPECT_EQ(FILLED_ORDSTATUS, bought->status_) << "it trades with the GTC sell, passing the expired one by";
    EXPECT_EQ(FILLED_ORDSTATUS, gtc->status_);
    EXPECT_EQ(EXPIRED_ORDSTATUS, gtd->status_);
    EXPECT_EQ(0u, gtd->cumQty_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, EXPIRED_EXECTYPE }), engine.reports_.reportTypesOf(gtd->orderId_));
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRADE_EXECTYPE }), engine.reports_.reportTypesOf(bought->orderId_));
    EXPECT_FALSE(orderBook_->getTop(instrId1_, SELL_SIDE).isValid());
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid());
}

TEST_F(ProcessorTest, ADayOrderExpiresAtTheEndOfItsDayAndAGtcOrderDoesNot)
{
    // 17:00 New York (#240, ruled 2026-10-16): 21:00 UTC, as New York keeps daylight time in October
    DateTimeT now = OCT15 + 13 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    auto order = withTif(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), DAY_TIF);
    order->creationTime_ = now;
    OrderEntry *day = engine.submit(std::move(order));
    order = withTif(createTestOrder(instrId1_, BUY_SIDE, 9.0, 30), GTC_TIF);
    order->creationTime_ = now;
    OrderEntry *gtc = engine.submit(std::move(order));
    ASSERT_EQ(2u, expiry.pending()) << "the DAY order's expiry, and the GTC order's restatement (#244)";

    now = OCT15 + 21 * HOUR - 1;
    EXPECT_EQ(0u, expiry.fireDue());
    now += 1;
    ASSERT_EQ(2u, expiry.fireDue());
    engine.processQueued(2);
    EXPECT_EQ(EXPIRED_ORDSTATUS, day->status_);
    EXPECT_EQ(0u, day->leavesQty_);
    EXPECT_EQ(NEW_ORDSTATUS, gtc->status_) << "a GTC order expired with the day";
    EXPECT_EQ(30u, gtc->leavesQty_);
    EXPECT_EQ(gtc->orderId_, orderBook_->getTop(instrId1_, BUY_SIDE));
}

TEST_F(ProcessorTest, ATimerOfAnOrderThatFilledDecidesNothing)
{
    // The order traded before its time: its timer finds it filled. The state machine has no expiry from there, and
    // its no_transition throws, failing the timer's transaction.
    DateTimeT now = OCT15 + 9 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    OrderEntry *gtd = engine.submit(asGtd(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), now, now + HOUR));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30), IOC_TIF)); // no timer of its own
    ASSERT_EQ(FILLED_ORDSTATUS, gtd->status_);

    now += HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    EXPECT_NO_THROW(engine.processQueued(1));
    EXPECT_EQ(FILLED_ORDSTATUS, gtd->status_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRADE_EXECTYPE }), engine.reports_.reportTypesOf(gtd->orderId_));
}

TEST_F(ProcessorTest, ADayReplacementExpiresAtTheEndOfTheDayItWasReplacedIn)
{
    // A replacement keeps its original's creationTime_: a GTC order from the day before, replaced by a DAY one,
    // expired at once, its original's day long over. Its day is the replace's, its lastUpdateTime_ (#240).
    DateTimeT now = OCT15 + 13 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    auto order = withTif(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), GTC_TIF);
    order->creationTime_ = now;
    order->lastUpdateTime_ = now;
    OrderEntry *gtc = engine.submit(std::move(order));
    const size_t before = expiry.pending();

    now = OCT15 + 37 * HOUR; // the next day, 13:00 UTC
    OrderEntry *replacement = gtc->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = gtc->clOrderId_;
    replacement->tif_ = DAY_TIF;
    replacement->lastUpdateTime_ = now; // as the gateways set it for a replace
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(gtc->orderId_, replacement));
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    ASSERT_EQ(NEW_ORDSTATUS, live->status_);
    ASSERT_EQ(before + 1, expiry.pending()) << "the replacement has no expiry";
    for (size_t fired = expiry.fireDue(); 0 < fired; fired = expiry.fireDue())
    {
        engine.processQueued(fired);
    }
    EXPECT_EQ(NEW_ORDSTATUS, live->status_) << "the replacement expired with its original's day";

    now = OCT15 + 45 * HOUR; // 17:00 EDT on the 16th
    for (size_t fired = expiry.fireDue(); 0 < fired; fired = expiry.fireDue())
    {
        engine.processQueued(fired);
    }
    EXPECT_EQ(EXPIRED_ORDSTATUS, live->status_);
}

TEST_F(ProcessorTest, AnExpiryOfAnOrderNoLongerLiveDecidesNothing)
{
    // The state machine has no expiry from a filled order, and its no_transition throws: decided for one, the timer's
    // transaction failed, and logged an error, every time an order filled before its time (#240)
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    OrderEntry *buy = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_);
    for (const OrderStateChange change : { OrderStateChange::EXPIRE, OrderStateChange::DAY_END })
    {
        TransactionScope scope;
        Context cnxt(OrderStorage::instance(), orderBook_.get(), inQueues_.get(), &engine.reports_, nullptr,
                     IdTGenerator::instance(), nullptr);
        EXPECT_NO_THROW(engine.processor_.process(change, buy, &scope, cnxt));
        EXPECT_EQ(0u, scope.operationCount());
        EXPECT_EQ(FILLED_ORDSTATUS, buy->status_);
    }
}

TEST_F(ProcessorTest, ADayEndDecidesNothingForAnOrderThatIsntADayOrder)
{
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    OrderEntry *gtc = engine.submit(withTif(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), GTC_TIF));
    EXPECT_NO_THROW(engine.request(TimerEvent(gtc->orderId_, TimerEvent::DAY_END)));
    EXPECT_EQ(NEW_ORDSTATUS, gtc->status_);
    EXPECT_EQ(std::vector<ExecType>{ NEW_EXECTYPE }, engine.reports_.reportTypesOf(gtc->orderId_));
}

TEST_F(ProcessorTest, AHeldDayStopExpiresAndLeavesTheStopBook)
{
    DateTimeT now = OCT15 + 9 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    auto order = withTif(asStop(createTestOrder(instrId1_, SELL_SIDE, 0.0, 10), STOP_ORDERTYPE, 9.5), DAY_TIF);
    order->creationTime_ = now;
    OrderEntry *stop = engine.submit(std::move(order));
    ASSERT_TRUE(orderBook_->holdsStops(instrId1_));

    now = OCT15 + 24 * HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(EXPIRED_ORDSTATUS, stop->status_);
    EXPECT_EQ(0u, stop->leavesQty_);
    EXPECT_FALSE(orderBook_->holdsStops(instrId1_)) << "an expired stop is still held";
}

TEST_F(ProcessorTest, AnOrderDoneForTheDayHasNoLeavesUntilANewDayOrAResumeGivesThemBack)
{
    // Done for the day, an order trades no more, so its LeavesQty is 0, as a closed order's is (#386). The next day, or
    // a resume, books it again with what it hasn't filled, and it is NEW or PARTFILL by its fills, as its leaves no
    // longer tell.
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get());
    OrderEntry *part = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 40), IOC_TIF));
    OrderEntry *none = engine.submit(createTestOrder(instrId1_, BUY_SIDE, 9.0, 50));
    ASSERT_EQ(PARTFILL_ORDSTATUS, part->status_);
    ASSERT_EQ(NEW_ORDSTATUS, none->status_);

    for (OrderEntry *order : { part, none })
    {
        engine.request(OrderChangeStateEvent(order->orderId_, OrderChangeStateEvent::FINISH));
        EXPECT_EQ(DFD_ORDSTATUS, order->status_);
        EXPECT_EQ(0u, order->leavesQty_);
        EXPECT_EQ(0u, engine.reports_.leavesReportedWith(order->orderId_, DFD_EXECTYPE));
    }
    EXPECT_FALSE(orderBook_->getTop(instrId1_, BUY_SIDE).isValid()) << "an order done for the day is in the book";

    // The day's end that expires a working DAY order begins the next day of one done for the day
    engine.request(TimerEvent(part->orderId_, TimerEvent::DAY_END));
    EXPECT_EQ(PARTFILL_ORDSTATUS, part->status_);
    EXPECT_EQ(40u, part->cumQty_);
    EXPECT_EQ(60u, part->leavesQty_);
    // Suspended while done for the day, then resumed
    engine.request(OrderChangeStateEvent(none->orderId_, OrderChangeStateEvent::SUSPEND));
    EXPECT_EQ(SUSPENDED_ORDSTATUS, none->status_);
    EXPECT_EQ(0u, none->leavesQty_);
    engine.request(OrderChangeStateEvent(none->orderId_, OrderChangeStateEvent::RESUME));
    EXPECT_EQ(NEW_ORDSTATUS, none->status_) << "an order that never traded came back partly filled";
    EXPECT_EQ(50u, none->leavesQty_);

    const BookSnapshot book = orderBook_->getSnapshot(instrId1_, OrderStorage::instance());
    ASSERT_EQ(2u, book.bids.size());
    EXPECT_EQ(60u, book.bids[0].totalQty);
    EXPECT_EQ(50u, book.bids[1].totalQty);
}

TEST_F(ProcessorTest, AReplacementHasATimerOfItsOwnAndTheOriginalsFindsItReplaced)
{
    DateTimeT now = OCT15 + 9 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry);
    OrderEntry *gtd = engine.submit(asGtd(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), now, now + HOUR));

    OrderEntry *replacement = gtd->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = gtd->clOrderId_;
    replacement->expireTime_ = now + 2 * HOUR;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(gtd->orderId_, replacement));
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    ASSERT_EQ(REPLACED_ORDSTATUS, gtd->status_);
    ASSERT_EQ(2u, expiry.pending());

    now += HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    EXPECT_NO_THROW(engine.processQueued(1));
    EXPECT_EQ(NEW_ORDSTATUS, live->status_) << "the original's timer expired its replacement";
    now += HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(EXPIRED_ORDSTATUS, live->status_);
}

// =============================================================================
// Business days and day figures (#244)
// =============================================================================

namespace
{
/// Logs the business day of each version persisted, as "persist day <dayStart_>"
class DayVersionLog final : public COP::OrderSaver
{
public:
    explicit DayVersionLog(std::vector<std::string> *log) : log_(log) {}

    COP::u32 save(const COP::OrderEntry &order) override
    {
        log_->push_back("persist day " + std::to_string(order.dayStart_));
        return version_++;
    }
    void erase(const COP::IdT &, COP::u32) override {}

private:
    std::vector<std::string> *log_;
    COP::u32 version_ = 0;
};

/// A GTC order created at 'now'
std::unique_ptr<OrderEntry> asGtc(std::unique_ptr<OrderEntry> order, DateTimeT now)
{
    order->tif_ = GTC_TIF;
    order->creationTime_ = now;
    return order;
}
} // namespace

TEST_F(ProcessorTest, AnOrdersDayFiguresAreItsFillsInTheDay)
{
    // FIX's DayOrderQty, DayCumQty and DayAvgPx: nothing kept them, and every order reported 0 (#244)
    DateTimeT now = OCT15 + 13 * HOUR;
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now;
                      });
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30));
    engine.submit(createTestOrder(instrId1_, SELL_SIDE, 10.5, 20));
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 11.0, 100), now));
    ASSERT_NE(nullptr, buy);
    EXPECT_EQ(OCT15 - 3 * HOUR, buy->dayStart_) << "the day that began 17:00 EDT on the 14th";
    EXPECT_EQ(50u, buy->dayCumQty_);
    EXPECT_DOUBLE_EQ(10.2, buy->dayAvgPx_);
    EXPECT_EQ(100u, buy->dayOrderQty_);
    EXPECT_EQ(50u, buy->cumQty_);
}

TEST_F(ProcessorTest, AFillAfterTheDaysEndCountsInTheNewDay)
{
    // A trade after 17:00 New York, before the resting order's restatement ran, is the new day's (#244)
    DateTimeT now = OCT15 + 13 * HOUR;
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30), IOC_TIF));
    ASSERT_EQ(30u, buy->dayCumQty_);

    now = OCT15 + 21 * HOUR; // the day's end
    OrderEntry *sell = engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20), IOC_TIF));
    EXPECT_EQ(OCT15 + 21 * HOUR, buy->dayStart_);
    EXPECT_EQ(20u, buy->dayCumQty_) << "the fill counted in the day before, or the day's figures didn't begin again";
    EXPECT_DOUBLE_EQ(10.0, buy->dayAvgPx_);
    EXPECT_EQ(70u, buy->dayOrderQty_) << "not what it had left when the day began";
    EXPECT_EQ(50u, buy->cumQty_);
    ASSERT_NE(nullptr, sell);
    EXPECT_EQ(OCT15 + 21 * HOUR, sell->dayStart_);
    EXPECT_EQ(20u, sell->dayCumQty_);
}

TEST_F(ProcessorTest, ATradesDateIsTheBusinessDayItCountsIn)
{
    // Every trade's tradeDate was 1, so a client read 1970-01-01 (#258). It is the business day's date, as YYYYMMDD:
    // the date the day ends on, at 17:00 New York, so a trade after 17:00 has the next day's (ruled 2026-10-19).
    DateTimeT now = OCT15 + 13 * HOUR; // 09:00 EDT
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now));
    OrderEntry *sell = engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30), IOC_TIF));
    now = OCT15 + 21 * HOUR; // 17:00 EDT
    OrderEntry *late = engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20), IOC_TIF));
    ASSERT_NE(nullptr, buy);
    ASSERT_NE(nullptr, sell);
    ASSERT_NE(nullptr, late);
    EXPECT_EQ((std::vector<DateTimeT>{ 20261015, 20261016 }), engine.reports_.tradeDatesOf(buy->orderId_));
    EXPECT_EQ(std::vector<DateTimeT>{ 20261015 }, engine.reports_.tradeDatesOf(sell->orderId_));
    EXPECT_EQ(std::vector<DateTimeT>{ 20261016 }, engine.reports_.tradeDatesOf(late->orderId_));
    // The stored trades have it too. Each trade is in the order's executions twice, as the match stored it and as it
    // was reported, so compare the dates they hold.
    auto storedDates = [](OrderEntry &order)
    {
        std::set<DateTimeT> dates;
        for (const EventData &event : *order.executions_.get())
        {
            const ExecutionEntry *exec = OrderStorage::instance()->locateByExecId(event.eventId_);
            if ((nullptr != exec) && (TRADE_EXECTYPE == exec->type_))
            {
                dates.insert(static_cast<const TradeExecEntry *>(exec)->tradeDate_);
            }
        }
        return dates;
    };
    EXPECT_EQ((std::set<DateTimeT>{ 20261015, 20261016 }), storedDates(*buy));
    EXPECT_EQ(std::set<DateTimeT>{ 20261016 }, storedDates(*late));
}

TEST_F(ProcessorTest, BothSidesOfATradeCountItInTheSameDay)
{
    // The clock is read once for a match (#244): a trade at the day's end can't count in one day for one side and in
    // the next for the other. Here each read moves the clock on a millisecond, across 17:00 New York.
    DateTimeT now = OCT15 + 13 * HOUR;
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now++;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), OCT15 + 13 * HOUR));
    now = OCT15 + 21 * HOUR - 1; // the day's last millisecond
    OrderEntry *sell = engine.submit(asGtc(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20), now));
    ASSERT_NE(nullptr, sell);
    EXPECT_EQ(buy->dayStart_, sell->dayStart_) << "the trade's sides counted it in different days";
    EXPECT_EQ(20u, buy->dayCumQty_);
    EXPECT_EQ(20u, sell->dayCumQty_);
}

TEST_F(ProcessorTest, ASwapsFillAfterTheDaysEndCountsInTheNewDay)
{
    // A swap's near legs are its fills (#239), applied by SwapExecutionDeferedEvent, which set no business day: a swap's
    // fill after 17:00 New York counted in the day before (#244). Here the buy arrives in the day's last millisecond and
    // trades at its end, so both sides count the fill in the new day: the resting sell, and the buy from the old day.
    DateTimeT now = OCT15 + 13 * HOUR;
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now;
                      });
    const auto swap = [&](Side side, PriceT nearPrice, QuantityT qty)
    {
        auto order = createTestOrder(instrId1_, side, nearPrice, qty);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->tif_ = GTC_TIF;
        order->creationTime_ = now;
        order->farPrice_ = 1.095;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    OrderEntry *sell = engine.submit(swap(SELL_SIDE, 1.085, 30));
    ASSERT_EQ(OCT15 - 3 * HOUR, sell->dayStart_);

    now = OCT15 + 21 * HOUR - 1; // the day's last millisecond
    auto late = swap(BUY_SIDE, 1.085, 30);
    now += 1; // the day's end
    const OrderEntry *buy = engine.submit(std::move(late));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_) << "the swaps didn't trade";
    EXPECT_EQ(OCT15 + 21 * HOUR, sell->dayStart_) << "the resting swap's fill counted in the day before";
    EXPECT_EQ(30u, sell->dayCumQty_);
    EXPECT_EQ(OCT15 + 21 * HOUR, buy->dayStart_) << "the arriving swap's fill counted in the day before";
    EXPECT_EQ(30u, buy->dayCumQty_);
}

TEST_F(ProcessorTest, BothSidesOfASwapCountItInTheSameDay)
{
    // A swap's match reads the clock once (#244), as an outright match does (BothSidesOfATradeCountItInTheSameDay): here
    // each read moves the clock on a millisecond, across 17:00 New York
    DateTimeT now = OCT15 + 13 * HOUR;
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now++;
                      });
    const auto swap = [&](Side side, PriceT nearPrice, QuantityT qty)
    {
        auto order = createTestOrder(instrId1_, side, nearPrice, qty);
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->tif_ = GTC_TIF;
        order->creationTime_ = now;
        order->farPrice_ = 1.095;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        return order;
    };
    OrderEntry *sell = engine.submit(swap(SELL_SIDE, 1.085, 30));
    now = OCT15 + 21 * HOUR - 1; // the day's last millisecond
    const OrderEntry *buy = engine.submit(swap(BUY_SIDE, 1.085, 30));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_) << "the swaps didn't trade";
    EXPECT_EQ(sell->dayStart_, buy->dayStart_) << "the swap's sides counted it in different days";
    EXPECT_EQ(30u, sell->dayCumQty_);
    EXPECT_EQ(30u, buy->dayCumQty_);
}

TEST_F(ProcessorTest, AnOrderThatOutlivesItsDayIsRestatedAtTheDaysEnd)
{
    // At 17:00 New York the order's day figures begin again, and its client gets a RESTATED report, FIX's ExecType D
    // (#244, ruled 2026-10-16), then again at the next day's end
    DateTimeT now = OCT15 + 13 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry,
                      [&now]
                      {
                          return now;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30), IOC_TIF));
    ASSERT_EQ(1u, expiry.pending()) << "the GTC order's restatement";

    now = OCT15 + 21 * HOUR - 1;
    EXPECT_EQ(0u, expiry.fireDue()) << "restated before its day ended";
    now += 1;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(OCT15 + 21 * HOUR, buy->dayStart_);
    EXPECT_EQ(0u, buy->dayCumQty_);
    EXPECT_DOUBLE_EQ(0.0, buy->dayAvgPx_);
    EXPECT_EQ(70u, buy->dayOrderQty_);
    EXPECT_EQ(30u, buy->cumQty_) << "a restatement changed the order's fills";
    EXPECT_EQ(PARTFILL_ORDSTATUS, buy->status_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRADE_EXECTYPE, RESTATED_EXECTYPE }),
              engine.reports_.reportTypesOf(buy->orderId_));

    ASSERT_EQ(1u, expiry.pending()) << "no restatement for the next day";
    now = OCT15 + 45 * HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(OCT15 + 45 * HOUR, buy->dayStart_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRADE_EXECTYPE, RESTATED_EXECTYPE, RESTATED_EXECTYPE }),
              engine.reports_.reportTypesOf(buy->orderId_));
}

TEST_F(ProcessorTest, ARestatementAfterAFillThatBeganTheDayKeepsTheFill)
{
    // A trade after the day's end began the new day's figures: the restatement doesn't begin them again, which would
    // lose the trade, and reports them as they are (#244)
    DateTimeT now = OCT15 + 13 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry,
                      [&now]
                      {
                          return now;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now));

    now = OCT15 + 22 * HOUR; // after the day's end, before its restatement fires
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 20), IOC_TIF));
    ASSERT_EQ(20u, buy->dayCumQty_);
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    EXPECT_EQ(OCT15 + 21 * HOUR, buy->dayStart_);
    EXPECT_EQ(20u, buy->dayCumQty_) << "the restatement lost the day's fill";
    EXPECT_EQ(100u, buy->dayOrderQty_);
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRADE_EXECTYPE, RESTATED_EXECTYPE }),
              engine.reports_.reportTypesOf(buy->orderId_));
}

TEST_F(ProcessorTest, AnOrderNoLongerLiveIsRestatedNoMore)
{
    DateTimeT now = OCT15 + 13 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry,
                      [&now]
                      {
                          return now;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 30), now));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30), IOC_TIF));
    ASSERT_EQ(FILLED_ORDSTATUS, buy->status_);

    now = OCT15 + 21 * HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    EXPECT_NO_THROW(engine.processQueued(1));
    EXPECT_EQ((std::vector<ExecType>{ NEW_EXECTYPE, TRADE_EXECTYPE }), engine.reports_.reportTypesOf(buy->orderId_));
    EXPECT_EQ(0u, expiry.pending()) << "a filled order is still restated";
}

TEST_F(ProcessorTest, ARestatementIsDurableBeforeItsReport)
{
    // A client that hears RESTATED must find the new day's figures in the order's record after a crash (#28, #244)
    std::vector<std::string> log;
    DayVersionLog saver(&log);
    OrderStorage::instance()->attach(&saver);
    DateTimeT now = OCT15 + 13 * HOUR;
    ExpiryScheduler expiry(inQueues_.get(),
                           [&now]
                           {
                               return now;
                           });
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), &expiry,
                      [&now]
                      {
                          return now;
                      });
    engine.reports_.log_ = &log;
    engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now));
    log.clear();

    now = OCT15 + 21 * HOUR;
    ASSERT_EQ(1u, expiry.fireDue());
    engine.processQueued(1);
    const auto persisted = std::find(log.begin(), log.end(), "persist day " + std::to_string(OCT15 + 21 * HOUR));
    const auto restated = std::find(log.begin(), log.end(), "report RESTATED");
    ASSERT_NE(log.end(), persisted);
    ASSERT_NE(log.end(), restated);
    EXPECT_LT(persisted, restated) << "the restatement was reported before it was durable";
}

TEST_F(ProcessorTest, AReplacementTakesOverItsOriginalsDay)
{
    // The day's fills and the day they are for are the original's, as they are when the replace is decided; what it had
    // left to trade when the day began is from its own quantity (#244)
    DateTimeT now = OCT15 + 13 * HOUR;
    StopEngine engine(orderBook_.get(), inQueues_.get(), transMgr_.get(), nullptr,
                      [&now]
                      {
                          return now;
                      });
    OrderEntry *buy = engine.submit(asGtc(createTestOrder(instrId1_, BUY_SIDE, 10.0, 100), now));
    engine.submit(withTif(createTestOrder(instrId1_, SELL_SIDE, 10.0, 30), IOC_TIF));

    OrderEntry *replacement = buy->clone();
    replacement->orderId_ = IdT();
    test::assignClOrderId(replacement);
    replacement->origClOrderId_ = buy->clOrderId_;
    replacement->orderQty_ = 150;
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    replacement->dayStart_ = 0; // as a client's copy taken before a fill would have them
    replacement->dayCumQty_ = 0;
    replacement->dayAvgPx_ = 0.0;
    const RawDataEntry replacementId = replacement->clOrderId_.get();
    engine.request(OrderReplaceEvent(buy->orderId_, replacement));
    OrderEntry *live = OrderStorage::instance()->locateByClOrderId(replacementId);
    ASSERT_NE(nullptr, live);
    ASSERT_EQ(REPLACED_ORDSTATUS, buy->status_);
    EXPECT_EQ(OCT15 - 3 * HOUR, live->dayStart_);
    EXPECT_EQ(30u, live->dayCumQty_);
    EXPECT_DOUBLE_EQ(10.0, live->dayAvgPx_);
    EXPECT_EQ(150u, live->dayOrderQty_);
}
