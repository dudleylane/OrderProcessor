/**
 * Concurrent Order Processor library - Transaction Operations Tests
 *
 * Authors: dudleylane, Claude
 * Test Implementation: 2026
 *
 * Copyright (C) 2026 dudleylane
 *
 * Distributed under the GNU Affero General Public License (AGPL).
 *
 * Tests for individual Operation rollback implementations.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>

#include "TestFixtures.h"
#include "TestAux.h"
#include "TrOperations.h"
#include "TransactionScope.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "DeferedEvents.h"
#include "OrderMatcher.h"
#include "mocks/MockOrderBook.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Proc;
using namespace test;

namespace
{

// =============================================================================
// Test Deferred Event Container
// =============================================================================

class TestDeferedContainer : public DeferedEventContainer
{
public:
    void addDeferedEvent(DeferedEventBase *evnt) override
    {
        events_.push_back(evnt);
    }

    size_t deferedEventCount() const override
    {
        return events_.size();
    }

    void removeDeferedEventsFrom(size_t startIndex) override
    {
        if (startIndex >= events_.size())
        {
            return;
        }
        for (size_t i = startIndex; i < events_.size(); ++i)
        {
            delete events_[i];
        }
        events_.erase(events_.begin() + startIndex, events_.end());
    }

    void clear()
    {
        for (auto *e : events_)
        {
            delete e;
        }
        events_.clear();
    }

    ~TestDeferedContainer()
    {
        clear();
    }

    std::vector<DeferedEventBase *> events_;
};

/// Records the cancel rejects pushed to it, each with its target
class RecordingOutQueues : public Queues::OutQueues
{
public:
    void push(const Queues::ExecReportEvent &evnt, const std::string &target) override
    {
        reports_.emplace_back(evnt, target);
    }
    void push(const Queues::CancelRejectEvent &evnt, const std::string &target) override
    {
        rejects_.emplace_back(evnt, target);
    }
    void push(const Queues::BusinessRejectEvent &, const std::string &) override {}
    void push(const Queues::OrderRejectEvent &evnt, const std::string &target) override
    {
        refusals_.emplace_back(evnt, target);
    }

    std::vector<std::pair<Queues::ExecReportEvent, std::string>> reports_;
    std::vector<std::pair<Queues::CancelRejectEvent, std::string>> rejects_;
    std::vector<std::pair<Queues::OrderRejectEvent, std::string>> refusals_;
};

// =============================================================================
// Test Fixture for OrderBook Operations
// =============================================================================

class OrderBookOperationTest : public SingletonFixture
{
protected:
    void SetUp() override
    {
        SingletonFixture::SetUp();

        // Create instrument using the proper test helper
        instrumentId_ = test::addInstrument("TEST");

        // Create instrument set for order book init
        std::set<SourceIdT> instruments;
        instruments.insert(instrumentId_);

        // Initialize order book
        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments);

        // Create test order - don't save to OrderStorage, just create it
        testOrder_ = test::createCorrectOrder(instrumentId_);
        testOrder_->orderId_ = IdT(1, 1); // Set orderId manually
        testOrder_->side_ = BUY_SIDE;
        testOrder_->price_ = 100.0;
        testOrder_->orderQty_ = 100;
    }

    void TearDown() override
    {
        testOrder_.reset();
        orderBook_.reset();
        SingletonFixture::TearDown();
    }

    Context createContext()
    {
        return Context(nullptr, // orderStorage
                       orderBook_.get(),
                       nullptr, // inQueues
                       nullptr, // outQueues
                       nullptr, // orderMatcher
                       IdTGenerator::instance(),
                       nullptr // deferedEvents
        );
    }

protected:
    SourceIdT instrumentId_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<OrderEntry> testOrder_;
    DummyOrderSaver saver_;
};

// =============================================================================
// AddToOrderBookTrOperation Tests
// =============================================================================

TEST_F(OrderBookOperationTest, AddToOrderBook_Execute_AddsOrderToBook)
{
    AddToOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    op.execute(ctx);

    // Verify order is in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_TRUE(foundId.isValid());
    EXPECT_EQ(testOrder_->orderId_, foundId);
}

TEST_F(OrderBookOperationTest, AddToOrderBook_Rollback_RemovesOrderFromBook)
{
    AddToOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    // Execute then rollback
    op.execute(ctx);
    op.rollback(ctx);

    // Verify order is NOT in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_FALSE(foundId.isValid());
}

TEST_F(OrderBookOperationTest, AddToOrderBook_ExecuteThenRollback_BookUnchanged)
{
    // Verify book is empty initially
    IdT initialTop = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_FALSE(initialTop.isValid());

    AddToOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    op.execute(ctx);
    op.rollback(ctx);

    // Book should be back to initial state
    IdT finalTop = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_FALSE(finalTop.isValid());
}

// =============================================================================
// RemoveFromOrderBookTrOperation Tests
// =============================================================================

TEST_F(OrderBookOperationTest, RemoveFromOrderBook_Execute_RemovesOrderFromBook)
{
    // First add the order to the book
    orderBook_->add(*testOrder_);

    RemoveFromOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    op.execute(ctx);

    // Verify order is NOT in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_FALSE(foundId.isValid());
}

TEST_F(OrderBookOperationTest, RemoveFromOrderBook_Rollback_ReAddsOrderToBook)
{
    // First add the order to the book
    orderBook_->add(*testOrder_);

    RemoveFromOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    // Execute then rollback
    op.execute(ctx);
    op.rollback(ctx);

    // Verify order is back in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_TRUE(foundId.isValid());
    EXPECT_EQ(testOrder_->orderId_, foundId);
}

TEST_F(OrderBookOperationTest, RemoveFromOrderBook_ExecuteThenRollback_BookUnchanged)
{
    // Add order to book
    orderBook_->add(*testOrder_);

    // Verify order is present
    IdT initialTop = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_TRUE(initialTop.isValid());

    RemoveFromOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    op.execute(ctx);
    op.rollback(ctx);

    // Book should be back to initial state
    IdT finalTop = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_TRUE(finalTop.isValid());
    EXPECT_EQ(initialTop, finalTop);
}

// =============================================================================
// MatchOrderTrOperation Tests
// =============================================================================

class MatchOrderOperationTest : public OrderStorageFixture
{
protected:
    void SetUp() override
    {
        OrderStorageFixture::SetUp();

        // Create instrument using the proper test helper
        instrumentId_ = test::addInstrument("MATCH_TEST");

        // Create instrument set for order book init
        std::set<SourceIdT> instruments;
        instruments.insert(instrumentId_);

        // Initialize order book
        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments);

        // Initialize order matcher
        matcher_ = std::make_unique<OrderMatcher>();
        matcher_->init(&deferedContainer_);

        // Create test orders with unique ClOrderIds and save to OrderStorage
        auto buyOrderTemp = test::createCorrectOrder(instrumentId_);
        test::assignClOrderId(buyOrderTemp.get()); // Assign unique ClOrderId
        buyOrderTemp->side_ = BUY_SIDE;
        buyOrderTemp->price_ = 100.0;
        buyOrderTemp->orderQty_ = 100;
        buyOrderTemp->leavesQty_ = 100;
        buyOrder_ = OrderStorage::instance()->save(*buyOrderTemp, IdTGenerator::instance());

        auto sellOrderTemp = test::createCorrectOrder(instrumentId_);
        test::assignClOrderId(sellOrderTemp.get()); // Assign unique ClOrderId
        sellOrderTemp->side_ = SELL_SIDE;
        sellOrderTemp->price_ = 100.0;
        sellOrderTemp->orderQty_ = 100;
        sellOrderTemp->leavesQty_ = 100;
        sellOrder_ = OrderStorage::instance()->save(*sellOrderTemp, IdTGenerator::instance());
    }

    void TearDown() override
    {
        // Orders are owned by OrderStorage, don't delete them
        buyOrder_ = nullptr;
        sellOrder_ = nullptr;
        matcher_.reset();
        orderBook_.reset();
        OrderStorageFixture::TearDown();
    }

    Context createContext()
    {
        return Context(OrderStorage::instance(), orderBook_.get(),
                       nullptr, // inQueues
                       nullptr, // outQueues
                       matcher_.get(), IdTGenerator::instance(), &deferedContainer_);
    }

protected:
    SourceIdT instrumentId_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<OrderMatcher> matcher_;
    OrderEntry *buyOrder_;  // Owned by OrderStorage
    OrderEntry *sellOrder_; // Owned by OrderStorage
    TestDeferedContainer deferedContainer_;
    DummyOrderSaver saver_;
};

TEST_F(MatchOrderOperationTest, MatchOrder_Execute_NoMatchingOrder_NoDeferredEvents)
{
    // When there's no matching order for a limit order, no events are generated
    buyOrder_->ordType_ = LIMIT_ORDERTYPE;

    size_t initialEventCount = deferedContainer_.deferedEventCount();

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);

    // No matching order for limit order means no events
    EXPECT_EQ(initialEventCount, deferedContainer_.deferedEventCount());
}

TEST_F(MatchOrderOperationTest, MatchOrder_Execute_WithMatchingOrder_CreatesDeferredEvents)
{
    // Add sell order to book so buy order can match
    orderBook_->add(*sellOrder_);

    size_t initialEventCount = deferedContainer_.deferedEventCount();

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);

    // Should have created deferred events for the match
    EXPECT_GT(deferedContainer_.deferedEventCount(), initialEventCount);
}

TEST_F(MatchOrderOperationTest, MatchOrder_Rollback_RemovesDeferredEvents)
{
    // Add sell order to book so buy order can match
    orderBook_->add(*sellOrder_);

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    // Execute creates deferred events
    op.execute(ctx);
    size_t afterExecute = deferedContainer_.deferedEventCount();
    EXPECT_GT(afterExecute, 0u);

    // Rollback should remove them
    op.rollback(ctx);

    EXPECT_EQ(0u, deferedContainer_.deferedEventCount());
}

TEST_F(MatchOrderOperationTest, MatchOrder_ExecuteThenRollback_NoDeferredEventsRemain)
{
    // Add sell order to book
    orderBook_->add(*sellOrder_);

    EXPECT_EQ(0u, deferedContainer_.deferedEventCount());

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);
    op.rollback(ctx);

    // Should be back to zero
    EXPECT_EQ(0u, deferedContainer_.deferedEventCount());
}

TEST_F(MatchOrderOperationTest, MatchOrder_Rollback_OnlyRemovesOwnEvents)
{
    // Add a pre-existing event
    deferedContainer_.addDeferedEvent(new ExecutionDeferedEvent(buyOrder_));
    EXPECT_EQ(1u, deferedContainer_.deferedEventCount());

    // Add sell order to book
    orderBook_->add(*sellOrder_);

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);
    size_t afterExecute = deferedContainer_.deferedEventCount();
    EXPECT_GT(afterExecute, 1u);

    // Rollback should only remove events added by this operation
    op.rollback(ctx);

    // Pre-existing event should still be there
    EXPECT_EQ(1u, deferedContainer_.deferedEventCount());
}

// =============================================================================
// Exec Report Operations - Verify Intentional No-Op Rollback
// =============================================================================

class ExecReportOperationTest : public SingletonFixture
{
protected:
    void SetUp() override
    {
        SingletonFixture::SetUp();

        // Create instrument using the proper test helper
        instrumentId_ = test::addInstrument("EXEC_TEST");

        testOrder_ = test::createCorrectOrder(instrumentId_);
        testOrder_->orderId_ = IdT(1, 1);
        testOrder_->side_ = BUY_SIDE;
        testOrder_->price_ = 100.0;
        testOrder_->orderQty_ = 100;
    }

    void TearDown() override
    {
        testOrder_.reset();
        SingletonFixture::TearDown();
    }

protected:
    SourceIdT instrumentId_;
    std::unique_ptr<OrderEntry> testOrder_;
};

TEST_F(ExecReportOperationTest, CreateExecReport_Rollback_IsNoOp)
{
    // This test documents that exec report rollback is intentionally empty
    // because you cannot unsend a message
    CreateExecReportTrOperation op(NEW_ORDSTATUS, NEW_EXECTYPE, *testOrder_);
    Context ctx;

    // Rollback should not throw and is intentionally a no-op
    EXPECT_NO_THROW(op.rollback(ctx));
}

TEST_F(ExecReportOperationTest, CreateRejectExecReport_Rollback_IsNoOp)
{
    CreateRejectExecReportTrOperation op("test reason", REJECTED_ORDSTATUS, *testOrder_);
    Context ctx;

    EXPECT_NO_THROW(op.rollback(ctx));
}

TEST_F(ExecReportOperationTest, CreateReplaceExecReport_Rollback_IsNoOp)
{
    CreateReplaceExecReportTrOperation op(testOrder_->orderId_, NEW_ORDSTATUS, *testOrder_);
    Context ctx;

    EXPECT_NO_THROW(op.rollback(ctx));
}

} // namespace

// =============================================================================
// PersistOrderTrOperation Tests (#20)
// =============================================================================

// =============================================================================
// Cancel Operations (#73)
// =============================================================================

TEST_F(MatchOrderOperationTest, CancelOrder_RelatesToTheOrderAndItsInstrument)
{
    // Related to the instrument as well as the order, so it runs after every earlier match on the instrument
    CancelOrderTrOperation op(buyOrder_, "client");
    EXPECT_EQ(buyOrder_->orderId_, op.getObjectId());
    EXPECT_EQ(buyOrder_->instrument_.getId(), op.getRelatedId());
}

TEST_F(MatchOrderOperationTest, CancelOrder_Execute_QueuesTheRequest)
{
    CancelOrderTrOperation op(buyOrder_, "client");
    Context ctx = createContext();

    op.execute(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    auto *request = dynamic_cast<CancelRequestDeferedEvent *>(deferedContainer_.events_[0]);
    ASSERT_NE(nullptr, request);
    EXPECT_EQ(buyOrder_, request->order_);
    EXPECT_EQ("client", request->requester_);
}

TEST_F(MatchOrderOperationTest, CancelOrder_Rollback_RemovesOnlyItsRequest)
{
    deferedContainer_.addDeferedEvent(new CancelRequestDeferedEvent(sellOrder_, "earlier"));
    CancelOrderTrOperation op(buyOrder_, "client");
    Context ctx = createContext();

    op.execute(ctx);
    EXPECT_EQ(2u, deferedContainer_.deferedEventCount());
    op.rollback(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    auto *remaining = dynamic_cast<CancelRequestDeferedEvent *>(deferedContainer_.events_[0]);
    ASSERT_NE(nullptr, remaining);
    EXPECT_EQ(sellOrder_, remaining->order_);
}

TEST_F(MatchOrderOperationTest, CancelOrder_ExecuteWithoutAContainerThrows)
{
    CancelOrderTrOperation op(buyOrder_, "client");
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, nullptr, matcher_.get(), IdTGenerator::instance());
    EXPECT_THROW(op.execute(ctx), std::runtime_error);
}

// =============================================================================
// State change operations (#96)
// =============================================================================

TEST_F(MatchOrderOperationTest, ChangeOrderState_RelatesToTheOrderAndItsInstrument)
{
    // As a cancel does: it runs after every earlier match on the instrument
    ChangeOrderStateTrOperation op(buyOrder_, OrderStateChange::SUSPEND);
    EXPECT_EQ(buyOrder_->orderId_, op.getObjectId());
    EXPECT_EQ(buyOrder_->instrument_.getId(), op.getRelatedId());
}

TEST_F(MatchOrderOperationTest, ChangeOrderState_Execute_QueuesTheChange)
{
    ChangeOrderStateTrOperation op(buyOrder_, OrderStateChange::EXPIRE);
    Context ctx = createContext();

    op.execute(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    auto *change = dynamic_cast<StateChangeDeferedEvent *>(deferedContainer_.events_[0]);
    ASSERT_NE(nullptr, change);
    EXPECT_EQ(buyOrder_, change->order_);
    EXPECT_EQ(OrderStateChange::EXPIRE, change->change_);
}

TEST_F(MatchOrderOperationTest, ChangeOrderState_Rollback_RemovesOnlyItsChange)
{
    deferedContainer_.addDeferedEvent(new StateChangeDeferedEvent(sellOrder_, OrderStateChange::FINISH));
    ChangeOrderStateTrOperation op(buyOrder_, OrderStateChange::SUSPEND);
    Context ctx = createContext();

    op.execute(ctx);
    EXPECT_EQ(2u, deferedContainer_.deferedEventCount());
    op.rollback(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    auto *remaining = dynamic_cast<StateChangeDeferedEvent *>(deferedContainer_.events_[0]);
    ASSERT_NE(nullptr, remaining);
    EXPECT_EQ(sellOrder_, remaining->order_);
}

TEST_F(MatchOrderOperationTest, ChangeOrderState_ExecuteWithoutAContainerThrows)
{
    ChangeOrderStateTrOperation op(buyOrder_, OrderStateChange::RESUME);
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, nullptr, matcher_.get(), IdTGenerator::instance());
    EXPECT_THROW(op.execute(ctx), std::runtime_error);
}

TEST_F(MatchOrderOperationTest, CancelReject_GoesToTheRequesterWithTheReasonAndStatus)
{
    RecordingOutQueues out;
    buyOrder_->status_ = FILLED_ORDSTATUS;
    CancelRejectTrOperation op(*buyOrder_, Queues::CancelRejectEvent::TOO_LATE, "requester");
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);

    op.execute(ctx);

    ASSERT_EQ(1u, out.rejects_.size());
    EXPECT_EQ(buyOrder_->orderId_, out.rejects_[0].first.id_);
    EXPECT_EQ(Queues::CancelRejectEvent::TOO_LATE, out.rejects_[0].first.reason_);
    EXPECT_EQ(FILLED_ORDSTATUS, out.rejects_[0].first.ordStatus_);
    EXPECT_EQ("requester", out.rejects_[0].second);
}

// =============================================================================
// The cancel request's own ClOrdID, handed back with its answer (#98)
// =============================================================================

TEST_F(MatchOrderOperationTest, CancelReject_CarriesTheRequestsClOrdId)
{
    RecordingOutQueues out;
    buyOrder_->status_ = FILLED_ORDSTATUS;
    CancelRejectTrOperation op(*buyOrder_, Queues::CancelRejectEvent::TOO_LATE, "requester", "CXL-1");
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);

    op.execute(ctx);

    ASSERT_EQ(1u, out.rejects_.size());
    EXPECT_EQ("CXL-1", out.rejects_[0].first.requestClOrdId_);
}

TEST_F(MatchOrderOperationTest, CancelOrder_Execute_QueuesTheRequestWithItsClOrdId)
{
    CancelOrderTrOperation op(buyOrder_, "client", "CXL-2");
    Context ctx = createContext();

    op.execute(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    auto *request = dynamic_cast<CancelRequestDeferedEvent *>(deferedContainer_.events_[0]);
    ASSERT_NE(nullptr, request);
    EXPECT_EQ("CXL-2", request->requestClOrdId_);
}

TEST_F(MatchOrderOperationTest, CancelExecReport_CarriesTheRequestsClOrdId)
{
    RecordingOutQueues out;
    CreateCancelExecReportTrOperation op(*buyOrder_, "CXL-3");
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);

    op.execute(ctx);

    ASSERT_EQ(1u, out.reports_.size());
    ASSERT_NE(nullptr, out.reports_[0].first.exec_);
    EXPECT_EQ(CANCEL_EXECTYPE, out.reports_[0].first.exec_->type_);
    EXPECT_EQ(CANCELED_ORDSTATUS, out.reports_[0].first.exec_->orderStatus_);
    EXPECT_EQ("CXL-3", out.reports_[0].first.requestClOrdId_);
}

TEST_F(MatchOrderOperationTest, CancelReject_WithoutARequesterGoesToTheOrdersSource)
{
    // The bug this covers: the reject named no order, and went to the made-up target "tgt" (#73)
    RecordingOutQueues out;
    CancelRejectTrOperation op(*buyOrder_, Queues::CancelRejectEvent::OTHER);
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);

    op.execute(ctx);

    ASSERT_EQ(1u, out.rejects_.size());
    EXPECT_EQ(buyOrder_->orderId_, out.rejects_[0].first.id_);
    EXPECT_EQ("CLNT", out.rejects_[0].second);
}

// =============================================================================
// Replace Operations (#74)
// =============================================================================

TEST_F(MatchOrderOperationTest, ReplaceOrder_RelatesToTheOriginalAndItsInstrument)
{
    // Related to the instrument as well as the original, so it runs after every earlier match on the instrument
    ReplaceOrderTrOperation op(buyOrder_, std::unique_ptr<OrderEntry>(buyOrder_->clone()), "client");
    EXPECT_EQ(buyOrder_->orderId_, op.getObjectId());
    EXPECT_EQ(buyOrder_->instrument_.getId(), op.getRelatedId());
}

TEST_F(MatchOrderOperationTest, ReplaceOrder_Execute_QueuesTheRequestWithTheReplacement)
{
    std::unique_ptr<OrderEntry> replacement(buyOrder_->clone());
    OrderEntry *replacementPtr = replacement.get();
    ReplaceOrderTrOperation op(buyOrder_, std::move(replacement), "client");
    Context ctx = createContext();

    op.execute(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    auto *request = dynamic_cast<ReplaceRequestDeferedEvent *>(deferedContainer_.events_[0]);
    ASSERT_NE(nullptr, request);
    EXPECT_EQ(buyOrder_, request->original_);
    EXPECT_EQ(replacementPtr, request->replacement_.get());
    EXPECT_EQ("client", request->requester_);
}

TEST_F(MatchOrderOperationTest, ReplaceOrder_Rollback_RemovesOnlyItsRequest)
{
    deferedContainer_.addDeferedEvent(new CancelRequestDeferedEvent(sellOrder_, "earlier"));
    ReplaceOrderTrOperation op(buyOrder_, std::unique_ptr<OrderEntry>(buyOrder_->clone()), "client");
    Context ctx = createContext();

    op.execute(ctx);
    EXPECT_EQ(2u, deferedContainer_.deferedEventCount());
    op.rollback(ctx);

    ASSERT_EQ(1u, deferedContainer_.deferedEventCount());
    EXPECT_NE(nullptr, dynamic_cast<CancelRequestDeferedEvent *>(deferedContainer_.events_[0]));
}

TEST_F(MatchOrderOperationTest, RefuseReplace_GoesToTheRequester)
{
    RecordingOutQueues out;
    Queues::OrderRejectEvent refusal;
    refusal.replacement_ = true;
    refusal.refusal_ = Queues::CancelRejectEvent::TOO_LATE;
    refusal.reason_ = "Replace refused: too late";
    RefuseReplaceTrOperation op(*buyOrder_, refusal, "requester");
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);

    op.execute(ctx);

    ASSERT_EQ(1u, out.refusals_.size());
    EXPECT_EQ("requester", out.refusals_[0].second);
    EXPECT_EQ(Queues::CancelRejectEvent::TOO_LATE, out.refusals_[0].first.refusal_);
    EXPECT_EQ("Replace refused: too late", out.refusals_[0].first.reason_);
    EXPECT_EQ(buyOrder_->orderId_, op.getObjectId());
}

TEST_F(MatchOrderOperationTest, PersistOrderWritesAVersion)
{
    // The order reaches storage when the transaction commits, not while the state machine action
    // that changed it is still running (#20).
    MockOrderSaver saver;
    OrderStorage::instance()->attach(&saver);
    Context ctx = createContext();
    PersistOrderTrOperation op(*buyOrder_);

    EXPECT_CALL(saver, save(::testing::Ref(*buyOrder_))).WillOnce(::testing::Return(7u));
    op.execute(ctx);
}

TEST_F(MatchOrderOperationTest, PersistOrderRollbackErasesTheVersionItWrote)
{
    // Rollback is exact: it erases the version this operation appended and leaves earlier ones.
    MockOrderSaver saver;
    OrderStorage::instance()->attach(&saver);
    Context ctx = createContext();
    PersistOrderTrOperation op(*buyOrder_);

    EXPECT_CALL(saver, save(::testing::_)).WillOnce(::testing::Return(7u));
    op.execute(ctx);

    EXPECT_CALL(saver, erase(buyOrder_->orderId_, 7u)).Times(1);
    op.rollback(ctx);
}

TEST_F(MatchOrderOperationTest, PersistOrderRollbackWritesNothingWithoutASaver)
{
    // With no saver attached (tests, and the load passes) nothing is written, so nothing is erased.
    Context ctx = createContext();
    PersistOrderTrOperation op(*buyOrder_);

    op.execute(ctx);
    op.rollback(ctx);
}

// =============================================================================
// Stop release (#223)
// =============================================================================

namespace
{
/// The order made a stop limit held in the book's stop book: released, it buys at 100
void holdAsStopLimit(OrderEntry *order, OrderBookImpl *book)
{
    order->ordType_ = STOPLIMIT_ORDERTYPE;
    order->stopPx_ = 99.0;
    order->status_ = NEW_ORDSTATUS;
    book->addStop(*order);
}
} // namespace

TEST_F(MatchOrderOperationTest, ReleaseStop_RelatesToTheOrderAndItsInstrument)
{
    // Related to the instrument, as a match is, so it runs after the transaction whose trade reached the stop; and to
    // the order, so after every earlier transaction on it, a cancel's among them
    ReleaseStopTrOperation op(buyOrder_);
    EXPECT_EQ(buyOrder_->orderId_, op.getObjectId());
    EXPECT_EQ(buyOrder_->instrument_.getId(), op.getRelatedId());
}

TEST_F(MatchOrderOperationTest, ReleaseStop_ReleasesAHeldStopAndRollbackHoldsItAgain)
{
    orderBook_->add(*sellOrder_);
    holdAsStopLimit(buyOrder_, orderBook_.get());
    ReleaseStopTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);
    EXPECT_TRUE(buyOrder_->stopTriggered_);
    EXPECT_FALSE(orderBook_->holdsStops(instrumentId_));
    EXPECT_EQ(buyOrder_->orderId_, orderBook_->getTop(instrumentId_, BUY_SIDE)) << "a released stop limit rests";
    ASSERT_EQ(1u, deferedContainer_.deferedEventCount()) << "its trade with the sell";
    EXPECT_NE(nullptr, dynamic_cast<ExecutionDeferedEvent *>(deferedContainer_.events_[0]));

    op.rollback(ctx);
    EXPECT_FALSE(buyOrder_->stopTriggered_);
    EXPECT_TRUE(orderBook_->holdsStops(instrumentId_));
    EXPECT_FALSE(orderBook_->getTop(instrumentId_, BUY_SIDE).isValid());
    EXPECT_EQ(0u, deferedContainer_.deferedEventCount());
}

TEST_F(MatchOrderOperationTest, ReleaseStop_DoesNothingToAStopNoLongerHeldOrWorking)
{
    // A transaction on the order before it decided first: a cancel, whose own operations take the stop out of the stop
    // book, or another trade's release
    orderBook_->add(*sellOrder_);
    holdAsStopLimit(buyOrder_, orderBook_.get());
    buyOrder_->status_ = CANCELED_ORDSTATUS;
    Context ctx = createContext();
    {
        ReleaseStopTrOperation op(buyOrder_);
        op.execute(ctx);
        EXPECT_FALSE(buyOrder_->stopTriggered_);
        EXPECT_TRUE(orderBook_->holdsStops(instrumentId_));
        EXPECT_EQ(0u, deferedContainer_.deferedEventCount());
        op.rollback(ctx);
        EXPECT_TRUE(orderBook_->holdsStops(instrumentId_));
    }

    buyOrder_->status_ = NEW_ORDSTATUS;
    orderBook_->removeStop(*buyOrder_);
    buyOrder_->stopTriggered_ = true;
    {
        ReleaseStopTrOperation op(buyOrder_);
        op.execute(ctx);
        EXPECT_FALSE(orderBook_->getTop(instrumentId_, BUY_SIDE).isValid());
        EXPECT_EQ(0u, deferedContainer_.deferedEventCount());
        op.rollback(ctx);
        EXPECT_FALSE(orderBook_->holdsStops(instrumentId_)) << "undoing a release that did nothing held the stop";
        EXPECT_TRUE(buyOrder_->stopTriggered_);
    }
}

TEST_F(MatchOrderOperationTest, ReportRelease_ReportsTheReleaseToTheOrdersSource)
{
    // The TRIGGERED report, FIX's ExecType L (#236): the order is NEW, and its trades are reported after
    orderBook_->add(*sellOrder_);
    holdAsStopLimit(buyOrder_, orderBook_.get());
    RecordingOutQueues out;
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);
    ReleaseStopTrOperation release(buyOrder_);
    ReportReleaseTrOperation report(release, *buyOrder_);
    EXPECT_EQ(buyOrder_->orderId_, report.getObjectId());

    release.execute(ctx);
    report.execute(ctx);

    ASSERT_EQ(1u, out.reports_.size());
    const ExecutionEntry *exec = out.reports_[0].first.exec_;
    ASSERT_NE(nullptr, exec);
    EXPECT_EQ(TRIGGERED_EXECTYPE, exec->type_);
    EXPECT_EQ(NEW_ORDSTATUS, exec->orderStatus_);
    EXPECT_EQ(buyOrder_->orderId_, exec->orderId_);
    EXPECT_EQ(std::string(buyOrder_->source_.get()), out.reports_[0].second);
}

TEST_F(MatchOrderOperationTest, ReportRelease_ReportsNothingForAReleaseThatReleasedNothing)
{
    // The release found the stop canceled first, as a cancel enqueued before it would leave it
    orderBook_->add(*sellOrder_);
    holdAsStopLimit(buyOrder_, orderBook_.get());
    buyOrder_->status_ = CANCELED_ORDSTATUS;
    RecordingOutQueues out;
    Context ctx(OrderStorage::instance(), orderBook_.get(), nullptr, &out, matcher_.get(), IdTGenerator::instance(),
                &deferedContainer_);
    ReleaseStopTrOperation release(buyOrder_);
    ReportReleaseTrOperation report(release, *buyOrder_);

    release.execute(ctx);
    report.execute(ctx);

    EXPECT_TRUE(out.reports_.empty());
}

// =============================================================================
// A transaction's persists, written as one (#163)
// =============================================================================

namespace
{
/// An operation that publishes, as an execution report's does. It notes that it ran, and whether the persists before it
/// had been committed by then.
class PublishingOperation final : public Operation
{
public:
    PublishingOperation(const IdT &id, const bool *committed, bool *ran, bool *ranAfterCommit)
        : Operation(CREATE_EXECREPORT_TROPERATION, id), committed_(committed), ran_(ran),
          ranAfterCommit_(ranAfterCommit)
    {
    }
    void execute(const Context &) override
    {
        *ran_ = true;
        *ranAfterCommit_ = *committed_;
    }
    void rollback(const Context &) override {}

private:
    const bool *committed_;
    bool *ran_;
    bool *ranAfterCommit_;
};

/// A trade's transaction as Processor builds it: each order's persist first, then what publishes
void buildTrade(TransactionScope *scope, const OrderEntry &buy, const OrderEntry &sell, Operation *publishing)
{
    if (nullptr != publishing)
    {
        std::unique_ptr<Operation> op(publishing);
        scope->addOperation(op);
    }
    std::unique_ptr<Operation> persistSell = std::make_unique<PersistOrderTrOperation>(sell);
    scope->addOperationFirst(persistSell);
    std::unique_ptr<Operation> persistBuy = std::make_unique<PersistOrderTrOperation>(buy);
    scope->addOperationFirst(persistBuy);
}

/// LMDB, wired to the order storage as the server wires it, without restoring what the directory holds
class LmdbOrders
{
public:
    explicit LmdbOrders(const std::string &dir)
    {
        storage_.load(dir, &ignore_);
        dispatcher_.init(WideDataStorage::instance(), nullptr, &storage_, OrderStorage::instance());
    }
    LMDBStorage &storage()
    {
        return storage_;
    }
    StorageRecordDispatcher &dispatcher()
    {
        return dispatcher_;
    }

private:
    class Ignore final : public FileStorageObserver
    {
    public:
        void startLoad() override {}
        void onRecordLoaded(const IdT &, u32, const char *, size_t) override {}
        void finishLoad() override {}
    };
    Ignore ignore_;
    LMDBStorage storage_;
    StorageRecordDispatcher dispatcher_;
};

/// The dispatcher, except that the process ends while it saves one order: a crash in the middle of a transaction's
/// persists
class CrashingSaver final : public OrderSaver
{
public:
    CrashingSaver(OrderSaver *inner, const OrderEntry *crashOn) : inner_(inner), crashOn_(crashOn) {}
    u32 save(const OrderEntry &order) override
    {
        if (&order == crashOn_)
        {
            std::_Exit(0);
        }
        return inner_->save(order);
    }
    void erase(const IdT &orderId, u32 version) override
    {
        inner_->erase(orderId, version);
    }
    void beginGroup() override
    {
        inner_->beginGroup();
    }
    void commitGroup() override
    {
        inner_->commitGroup();
    }
    void abortGroup() noexcept override
    {
        inner_->abortGroup();
    }

private:
    OrderSaver *inner_;
    const OrderEntry *crashOn_;
};
} // namespace

TEST_F(MatchOrderOperationTest, ATradesPersistsAreCommittedTogetherBeforeItPublishes)
{
    // Both orders reach storage in one write, and before the report goes out (#28)
    ::testing::StrictMock<MockOrderSaver> saver;
    OrderStorage::instance()->attach(&saver);
    bool committed = false;
    bool ran = false;
    bool ranAfterCommit = false;
    {
        ::testing::InSequence order;
        EXPECT_CALL(saver, beginGroup());
        EXPECT_CALL(saver, save(::testing::Ref(*buyOrder_))).WillOnce(::testing::Return(3u));
        EXPECT_CALL(saver, save(::testing::Ref(*sellOrder_))).WillOnce(::testing::Return(5u));
        EXPECT_CALL(saver, commitGroup())
            .WillOnce(
                [&committed]
                {
                    committed = true;
                });
    }
    TransactionScope scope;
    buildTrade(&scope, *buyOrder_, *sellOrder_,
               new PublishingOperation(buyOrder_->orderId_, &committed, &ran, &ranAfterCommit));

    EXPECT_TRUE(scope.executeTransaction(createContext()));
    EXPECT_TRUE(ran);
    EXPECT_TRUE(ranAfterCommit) << "the report went out before the orders were durable";
}

TEST_F(MatchOrderOperationTest, ATradesLastTradeIsCommittedWithItsOrders)
{
    // The instrument's last trade joins the orders' write (#237): its price is durable with the fills it comes from
    ::testing::StrictMock<MockOrderSaver> saver;
    OrderStorage::instance()->attach(&saver);
    const IdT tradeId(4242, 1792000000);
    {
        ::testing::InSequence order;
        EXPECT_CALL(saver, beginGroup());
        EXPECT_CALL(saver, saveLastTrade(instrumentId_, 100.0, tradeId)).WillOnce(::testing::Return(0u));
        EXPECT_CALL(saver, save(::testing::Ref(*buyOrder_))).WillOnce(::testing::Return(3u));
        EXPECT_CALL(saver, save(::testing::Ref(*sellOrder_))).WillOnce(::testing::Return(5u));
        EXPECT_CALL(saver, commitGroup());
    }
    TransactionScope scope;
    buildTrade(&scope, *buyOrder_, *sellOrder_, nullptr);
    std::unique_ptr<Operation> lastTrade = std::make_unique<PersistLastTradeTrOperation>(instrumentId_, 100.0, tradeId);
    scope.addOperationFirst(lastTrade);

    EXPECT_TRUE(scope.executeTransaction(createContext()));
}

TEST_F(MatchOrderOperationTest, PersistLastTrade_RelatesToTheInstrumentsLastTradeAndRollbackErasesItsVersion)
{
    // Its object is the instrument's last trade, so the transactions that write it run one after another, as #148's
    // pruning needs, and not the instrument, whose bookings, cancels and releases a trade's fills don't wait for
    MockOrderSaver saver;
    OrderStorage::instance()->attach(&saver);
    Context ctx = createContext();
    const IdT tradeId(4242, 1792000000);
    PersistLastTradeTrOperation op(instrumentId_, 101.25, tradeId);
    EXPECT_EQ(IdT(instrumentId_.id_, 2), op.getObjectId());
    EXPECT_NE(instrumentId_, op.getObjectId());
    EXPECT_FALSE(op.getRelatedId().isValid());

    EXPECT_CALL(saver, saveLastTrade(instrumentId_, 101.25, tradeId)).WillOnce(::testing::Return(7u));
    op.execute(ctx);
    EXPECT_CALL(saver, eraseLastTrade(instrumentId_, 7u)).Times(1);
    op.rollback(ctx);
}

TEST_F(MatchOrderOperationTest, ATradeWhosePersistsFailToCommitPublishesNothing)
{
    ::testing::NiceMock<MockOrderSaver> saver;
    OrderStorage::instance()->attach(&saver);
    ON_CALL(saver, save(::testing::_)).WillByDefault(::testing::Return(1u));
    EXPECT_CALL(saver, commitGroup()).WillOnce(::testing::Throw(std::runtime_error("injected: the commit failed")));
    bool committed = false;
    bool ran = false;
    bool ranAfterCommit = false;
    TransactionScope scope;
    buildTrade(&scope, *buyOrder_, *sellOrder_,
               new PublishingOperation(buyOrder_->orderId_, &committed, &ran, &ranAfterCommit));

    EXPECT_FALSE(scope.executeTransaction(createContext()));
    EXPECT_FALSE(ran) << "a transaction whose orders aren't durable published";
}

TEST_F(MatchOrderOperationTest, OnePersistIsWrittenAsBefore)
{
    // Nothing to group: a transaction that persists one order writes it at once
    ::testing::StrictMock<MockOrderSaver> saver;
    OrderStorage::instance()->attach(&saver);
    EXPECT_CALL(saver, save(::testing::Ref(*buyOrder_))).WillOnce(::testing::Return(2u));
    TransactionScope scope;
    std::unique_ptr<Operation> persist = std::make_unique<PersistOrderTrOperation>(*buyOrder_);
    scope.addOperationFirst(persist);

    EXPECT_TRUE(scope.executeTransaction(createContext()));
}

TEST_F(MatchOrderOperationTest, ATradesOrdersReachLmdbInOneCommit)
{
    const std::string dir = test::uniqueTestPath("trade-one-commit");
    std::filesystem::remove_all(dir);
    {
        LmdbOrders lmdb(dir);
        OrderStorage::instance()->attach(&lmdb.dispatcher());
        TransactionScope scope;
        buildTrade(&scope, *buyOrder_, *sellOrder_, nullptr);
        const u64 before = lmdb.storage().commits();

        EXPECT_TRUE(scope.executeTransaction(createContext()));

        EXPECT_EQ(before + 1, lmdb.storage().commits()) << "the trade's orders took more than one LMDB commit";
        EXPECT_TRUE(lmdb.storage().isExists(buyOrder_->orderId_, 0));
        EXPECT_TRUE(lmdb.storage().isExists(sellOrder_->orderId_, 0));
    }
    std::filesystem::remove_all(dir);
}

TEST_F(MatchOrderOperationTest, ACrashWhileATradesOrdersArePersistedLeavesBothOrNeither)
{
    // The process ends while the second of the two orders is being saved, as a host crash would end it. A restart must
    // find both orders' new versions, or neither: one without the other is half a trade (#163).
    const std::string dir = test::uniqueTestPath("trade-crash");
    std::filesystem::remove_all(dir);
    {
        // The versions the orders' earlier transactions wrote
        LmdbOrders lmdb(dir);
        lmdb.dispatcher().save(*buyOrder_);
        lmdb.dispatcher().save(*sellOrder_);
    }

    EXPECT_EXIT(
        {
            LmdbOrders lmdb(dir);
            CrashingSaver crashing(&lmdb.dispatcher(), sellOrder_);
            OrderStorage::instance()->attach(&crashing);
            TransactionScope scope;
            buildTrade(&scope, *buyOrder_, *sellOrder_, nullptr);
            scope.executeTransaction(createContext());
            std::_Exit(1); // the crash didn't happen
        },
        ::testing::ExitedWithCode(0), "");

    LmdbOrders restarted(dir);
    const bool buyNew = restarted.storage().isExists(buyOrder_->orderId_, 1);
    const bool sellNew = restarted.storage().isExists(sellOrder_->orderId_, 1);
    EXPECT_EQ(buyNew, sellNew) << "half the trade is durable: the buy's new version " << (buyNew ? "is" : "isn't")
                               << " on disk, the sell's " << (sellNew ? "is" : "isn't");
    EXPECT_TRUE(restarted.storage().isExists(buyOrder_->orderId_, 0));
    EXPECT_TRUE(restarted.storage().isExists(sellOrder_->orderId_, 0));
    std::filesystem::remove_all(dir);
}
