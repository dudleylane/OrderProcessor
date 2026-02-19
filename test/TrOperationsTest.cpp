/**
 * Concurrent Order Processor library - Transaction Operations Tests
 *
 * Authors: dudleylane, Claude
 * Test Implementation: 2026
 *
 * Copyright (C) 2026 dudleylane
 *
 * Distributed under the GNU General Public License (GPL).
 *
 * Tests for individual Operation rollback implementations.
 */

#include <gtest/gtest.h>
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

using namespace COP;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Proc;
using namespace test;

namespace {

// =============================================================================
// Test Deferred Event Container
// =============================================================================

class TestDeferedContainer : public DeferedEventContainer {
public:
    void addDeferedEvent(DeferedEventBase* evnt) override {
        events_.push_back(evnt);
    }

    size_t deferedEventCount() const override {
        return events_.size();
    }

    void removeDeferedEventsFrom(size_t startIndex) override {
        if (startIndex >= events_.size()) {
            return;
        }
        for (size_t i = startIndex; i < events_.size(); ++i) {
            delete events_[i];
        }
        events_.erase(events_.begin() + startIndex, events_.end());
    }

    void clear() {
        for (auto* e : events_) {
            delete e;
        }
        events_.clear();
    }

    ~TestDeferedContainer() {
        clear();
    }

    std::vector<DeferedEventBase*> events_;
};

// =============================================================================
// Test Fixture for OrderBook Operations
// =============================================================================

class OrderBookOperationTest : public SingletonFixture {
protected:
    void SetUp() override {
        SingletonFixture::SetUp();

        // Create instrument using the proper test helper
        instrumentId_ = test::addInstrument("TEST");

        // Create instrument set for order book init
        std::set<SourceIdT> instruments;
        instruments.insert(instrumentId_);

        // Initialize order book
        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments, &saver_);

        // Create test order - don't save to OrderStorage, just create it
        testOrder_ = test::createCorrectOrder(instrumentId_);
        testOrder_->orderId_ = IdT(1, 1);  // Set orderId manually
        testOrder_->side_ = BUY_SIDE;
        testOrder_->price_ = 100.0;
        testOrder_->orderQty_ = 100;
    }

    void TearDown() override {
        testOrder_.reset();
        orderBook_.reset();
        SingletonFixture::TearDown();
    }

    Context createContext() {
        return Context(
            nullptr,  // orderStorage
            orderBook_.get(),
            nullptr,  // inQueues
            nullptr,  // outQueues
            nullptr,  // orderMatcher
            IdTGenerator::instance(),
            nullptr   // deferedEvents
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

TEST_F(OrderBookOperationTest, AddToOrderBook_Execute_AddsOrderToBook) {
    AddToOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    op.execute(ctx);

    // Verify order is in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_TRUE(foundId.isValid());
    EXPECT_EQ(testOrder_->orderId_, foundId);
}

TEST_F(OrderBookOperationTest, AddToOrderBook_Rollback_RemovesOrderFromBook) {
    AddToOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    // Execute then rollback
    op.execute(ctx);
    op.rollback(ctx);

    // Verify order is NOT in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_FALSE(foundId.isValid());
}

TEST_F(OrderBookOperationTest, AddToOrderBook_ExecuteThenRollback_BookUnchanged) {
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

TEST_F(OrderBookOperationTest, RemoveFromOrderBook_Execute_RemovesOrderFromBook) {
    // First add the order to the book
    orderBook_->add(*testOrder_);

    RemoveFromOrderBookTrOperation op(*testOrder_, instrumentId_);
    Context ctx = createContext();

    op.execute(ctx);

    // Verify order is NOT in book
    IdT foundId = orderBook_->getTop(instrumentId_, BUY_SIDE);
    EXPECT_FALSE(foundId.isValid());
}

TEST_F(OrderBookOperationTest, RemoveFromOrderBook_Rollback_ReAddsOrderToBook) {
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

TEST_F(OrderBookOperationTest, RemoveFromOrderBook_ExecuteThenRollback_BookUnchanged) {
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

class MatchOrderOperationTest : public OrderStorageFixture {
protected:
    void SetUp() override {
        OrderStorageFixture::SetUp();

        // Create instrument using the proper test helper
        instrumentId_ = test::addInstrument("MATCH_TEST");

        // Create instrument set for order book init
        std::set<SourceIdT> instruments;
        instruments.insert(instrumentId_);

        // Initialize order book
        orderBook_ = std::make_unique<OrderBookImpl>();
        orderBook_->init(instruments, &saver_);

        // Initialize order matcher
        matcher_ = std::make_unique<OrderMatcher>();
        matcher_->init(&deferedContainer_);

        // Create test orders with unique ClOrderIds and save to OrderStorage
        auto buyOrderTemp = test::createCorrectOrder(instrumentId_);
        test::assignClOrderId(buyOrderTemp.get());  // Assign unique ClOrderId
        buyOrderTemp->side_ = BUY_SIDE;
        buyOrderTemp->price_ = 100.0;
        buyOrderTemp->orderQty_ = 100;
        buyOrderTemp->leavesQty_ = 100;
        buyOrder_ = OrderStorage::instance()->save(*buyOrderTemp, IdTGenerator::instance());

        auto sellOrderTemp = test::createCorrectOrder(instrumentId_);
        test::assignClOrderId(sellOrderTemp.get());  // Assign unique ClOrderId
        sellOrderTemp->side_ = SELL_SIDE;
        sellOrderTemp->price_ = 100.0;
        sellOrderTemp->orderQty_ = 100;
        sellOrderTemp->leavesQty_ = 100;
        sellOrder_ = OrderStorage::instance()->save(*sellOrderTemp, IdTGenerator::instance());
    }

    void TearDown() override {
        // Orders are owned by OrderStorage, don't delete them
        buyOrder_ = nullptr;
        sellOrder_ = nullptr;
        matcher_.reset();
        orderBook_.reset();
        OrderStorageFixture::TearDown();
    }

    Context createContext() {
        return Context(
            OrderStorage::instance(),
            orderBook_.get(),
            nullptr,  // inQueues
            nullptr,  // outQueues
            matcher_.get(),
            IdTGenerator::instance(),
            &deferedContainer_
        );
    }

protected:
    SourceIdT instrumentId_;
    std::unique_ptr<OrderBookImpl> orderBook_;
    std::unique_ptr<OrderMatcher> matcher_;
    OrderEntry* buyOrder_;   // Owned by OrderStorage
    OrderEntry* sellOrder_;  // Owned by OrderStorage
    TestDeferedContainer deferedContainer_;
    DummyOrderSaver saver_;
};

TEST_F(MatchOrderOperationTest, MatchOrder_Execute_NoMatchingOrder_NoDeferredEvents) {
    // When there's no matching order for a limit order, no events are generated
    buyOrder_->ordType_ = LIMIT_ORDERTYPE;

    size_t initialEventCount = deferedContainer_.deferedEventCount();

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);

    // No matching order for limit order means no events
    EXPECT_EQ(initialEventCount, deferedContainer_.deferedEventCount());
}

TEST_F(MatchOrderOperationTest, MatchOrder_Execute_WithMatchingOrder_CreatesDeferredEvents) {
    // Add sell order to book so buy order can match
    orderBook_->add(*sellOrder_);

    size_t initialEventCount = deferedContainer_.deferedEventCount();

    MatchOrderTrOperation op(buyOrder_);
    Context ctx = createContext();

    op.execute(ctx);

    // Should have created deferred events for the match
    EXPECT_GT(deferedContainer_.deferedEventCount(), initialEventCount);
}

TEST_F(MatchOrderOperationTest, MatchOrder_Rollback_RemovesDeferredEvents) {
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

TEST_F(MatchOrderOperationTest, MatchOrder_ExecuteThenRollback_NoDeferredEventsRemain) {
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

TEST_F(MatchOrderOperationTest, MatchOrder_Rollback_OnlyRemovesOwnEvents) {
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

class ExecReportOperationTest : public SingletonFixture {
protected:
    void SetUp() override {
        SingletonFixture::SetUp();

        // Create instrument using the proper test helper
        instrumentId_ = test::addInstrument("EXEC_TEST");

        testOrder_ = test::createCorrectOrder(instrumentId_);
        testOrder_->orderId_ = IdT(1, 1);
        testOrder_->side_ = BUY_SIDE;
        testOrder_->price_ = 100.0;
        testOrder_->orderQty_ = 100;
    }

    void TearDown() override {
        testOrder_.reset();
        SingletonFixture::TearDown();
    }

protected:
    SourceIdT instrumentId_;
    std::unique_ptr<OrderEntry> testOrder_;
};

TEST_F(ExecReportOperationTest, CreateExecReport_Rollback_IsNoOp) {
    // This test documents that exec report rollback is intentionally empty
    // because you cannot unsend a message
    CreateExecReportTrOperation op(NEW_ORDSTATUS, NEW_EXECTYPE, *testOrder_);
    Context ctx;

    // Rollback should not throw and is intentionally a no-op
    EXPECT_NO_THROW(op.rollback(ctx));
}

TEST_F(ExecReportOperationTest, CreateRejectExecReport_Rollback_IsNoOp) {
    CreateRejectExecReportTrOperation op("test reason", REJECTED_ORDSTATUS, *testOrder_);
    Context ctx;

    EXPECT_NO_THROW(op.rollback(ctx));
}

TEST_F(ExecReportOperationTest, CreateReplaceExecReport_Rollback_IsNoOp) {
    CreateReplaceExecReportTrOperation op(testOrder_->orderId_, NEW_ORDSTATUS, *testOrder_);
    Context ctx;

    EXPECT_NO_THROW(op.rollback(ctx));
}

} // namespace
