/**
 * Concurrent Order Processor library - Google Test
 *
 * Author: Sergey Mikhailik
 * Test Implementation: 2026
 *
 * Copyright (C) 2009-2026 Sergey Mikhailik
 *
 * Distributed under the GNU General Public License (GPL).
 */

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "DeferedEvents.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "IdTGenerator.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::Proc;
using namespace COP::ACID;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

class TestDeferedEventFunctor : public DeferedEventFunctor {
public:
    std::vector<OrderEntry*> tradeExecutionOrders;
    std::vector<OrderEntry*> internalCancelOrders;

    void process(OrdState::onTradeExecution& /*ev*/, OrderEntry* order, const Context& /*cnxt*/) override {
        tradeExecutionOrders.push_back(order);
    }

    void process(OrdState::onInternalCancel& /*ev*/, OrderEntry* order, const Context& /*cnxt*/) override {
        internalCancelOrders.push_back(order);
    }

    void reset() {
        tradeExecutionOrders.clear();
        internalCancelOrders.clear();
    }
};

OrderEntry* createTestOrder(SourceIdT instrId, Side side, PriceT price, QuantityT qty) {
    SourceIdT srcId, destId, clOrdId, origClOrdId, accountId, clearingId, execList;
    auto* order = new OrderEntry(srcId, destId, clOrdId, origClOrdId, instrId, accountId, clearingId, execList);
    order->side_ = side;
    order->price_ = price;
    order->orderQty_ = qty;
    order->leavesQty_ = qty;
    order->cumQty_ = 0;
    order->ordType_ = LIMIT_ORDERTYPE;
    order->status_ = NEW_ORDSTATUS;
    return order;
}

} // namespace

// =============================================================================
// DeferedEvents Test Fixture
// =============================================================================

class DeferedEventsTest : public ::testing::Test {
protected:
    void SetUp() override {
        aux::ExchLogger::create();
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();

        // Add test instrument
        auto* instr = new InstrumentEntry();
        instr->symbol_ = "TEST";
        instr->securityId_ = "TESTSEC";
        instr->securityIdSource_ = "ISIN";
        instrId_ = WideDataStorage::instance()->add(instr);
    }

    void TearDown() override {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
        aux::ExchLogger::destroy();
    }

    SourceIdT instrId_;
};

// =============================================================================
// ExecutionDeferedEvent Tests
// =============================================================================

TEST_F(DeferedEventsTest, ExecutionDeferedEventConstruction) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);

    EXPECT_NO_THROW({
        ExecutionDeferedEvent event(order);
    });

    delete order;
}

TEST_F(DeferedEventsTest, ExecutionDeferedEventDefaultConstruction) {
    EXPECT_NO_THROW({
        ExecutionDeferedEvent event;
    });
}

TEST_F(DeferedEventsTest, ExecutionDeferedEventStoresBaseOrder) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    ExecutionDeferedEvent event(order);

    EXPECT_EQ(event.baseOrder_, order);

    delete order;
}

TEST_F(DeferedEventsTest, ExecutionDeferedEventEmptyTrades) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    ExecutionDeferedEvent event(order);

    EXPECT_TRUE(event.trades_.empty());

    delete order;
}

TEST_F(DeferedEventsTest, ExecutionDeferedEventAddTrade) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    ExecutionDeferedEvent event(order);

    TradeParams params;
    params.lastQty_ = 50;
    params.lastPx_ = 99.50;
    params.order_ = order;

    event.trades_.push_back(params);

    EXPECT_EQ(event.trades_.size(), 1u);
    EXPECT_EQ(event.trades_[0].lastQty_, 50);
    EXPECT_DOUBLE_EQ(event.trades_[0].lastPx_, 99.50);

    delete order;
}

TEST_F(DeferedEventsTest, ExecutionDeferedEventMultipleTrades) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    ExecutionDeferedEvent event(order);

    TradeParams params1;
    params1.lastQty_ = 30;
    params1.lastPx_ = 99.50;
    params1.order_ = order;

    TradeParams params2;
    params2.lastQty_ = 40;
    params2.lastPx_ = 99.75;
    params2.order_ = order;

    TradeParams params3;
    params3.lastQty_ = 30;
    params3.lastPx_ = 100.00;
    params3.order_ = order;

    event.trades_.push_back(params1);
    event.trades_.push_back(params2);
    event.trades_.push_back(params3);

    EXPECT_EQ(event.trades_.size(), 3u);

    delete order;
}

// =============================================================================
// MatchOrderDeferedEvent Tests
// =============================================================================

TEST_F(DeferedEventsTest, MatchOrderDeferedEventConstruction) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);

    EXPECT_NO_THROW({
        MatchOrderDeferedEvent event(order);
    });

    delete order;
}

TEST_F(DeferedEventsTest, MatchOrderDeferedEventDefaultConstruction) {
    EXPECT_NO_THROW({
        MatchOrderDeferedEvent event;
    });
}

TEST_F(DeferedEventsTest, MatchOrderDeferedEventStoresOrder) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    MatchOrderDeferedEvent event(order);

    EXPECT_EQ(event.order_, order);

    delete order;
}

// =============================================================================
// CancelOrderDeferedEvent Tests
// =============================================================================

TEST_F(DeferedEventsTest, CancelOrderDeferedEventConstruction) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);

    EXPECT_NO_THROW({
        CancelOrderDeferedEvent event(order);
    });

    delete order;
}

TEST_F(DeferedEventsTest, CancelOrderDeferedEventDefaultConstruction) {
    EXPECT_NO_THROW({
        CancelOrderDeferedEvent event;
    });
}

TEST_F(DeferedEventsTest, CancelOrderDeferedEventStoresOrder) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    CancelOrderDeferedEvent event(order);

    EXPECT_EQ(event.order_, order);

    delete order;
}

TEST_F(DeferedEventsTest, CancelOrderDeferedEventCancelReason) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    CancelOrderDeferedEvent event(order);

    event.cancelReason_ = "Market closed";
    EXPECT_EQ(event.cancelReason_, "Market closed");

    delete order;
}

TEST_F(DeferedEventsTest, CancelOrderDeferedEventEmptyReason) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    CancelOrderDeferedEvent event(order);

    // Default reason should be empty
    EXPECT_TRUE(event.cancelReason_.empty());

    delete order;
}

TEST_F(DeferedEventsTest, CancelReasonWithSpecialCharacters) {
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    CancelOrderDeferedEvent event(order);

    event.cancelReason_ = "Cancel: [User Request] @12:30 - \"immediate\"";
    EXPECT_EQ(event.cancelReason_, "Cancel: [User Request] @12:30 - \"immediate\"");

    delete order;
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(DeferedEventsTest, MultipleOrdersWithSameEventType) {
    OrderEntry* order1 = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    OrderEntry* order2 = createTestOrder(instrId_, BUY_SIDE, 101.0, 200);

    MatchOrderDeferedEvent event1(order1);
    MatchOrderDeferedEvent event2(order2);

    EXPECT_NE(event1.order_, event2.order_);
    EXPECT_EQ(event1.order_, order1);
    EXPECT_EQ(event2.order_, order2);

    delete order1;
    delete order2;
}

TEST_F(DeferedEventsTest, ExecutionDeferedEventWithNullBaseOrder) {
    ExecutionDeferedEvent event(nullptr);
    EXPECT_EQ(event.baseOrder_, nullptr);
}

TEST_F(DeferedEventsTest, MatchOrderDeferedEventWithNullOrder) {
    MatchOrderDeferedEvent event(nullptr);
    EXPECT_EQ(event.order_, nullptr);
}

TEST_F(DeferedEventsTest, CancelOrderDeferedEventWithNullOrder) {
    CancelOrderDeferedEvent event(nullptr);
    EXPECT_EQ(event.order_, nullptr);
}

// =============================================================================
// Functor Integration Tests
// =============================================================================

TEST_F(DeferedEventsTest, FunctorReset) {
    TestDeferedEventFunctor functor;
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);

    OrdState::onInternalCancel cancelEvent;
    Context context;
    functor.process(cancelEvent, order, context);

    EXPECT_EQ(functor.internalCancelOrders.size(), 1u);

    functor.reset();
    EXPECT_EQ(functor.internalCancelOrders.size(), 0u);

    delete order;
}

TEST_F(DeferedEventsTest, FunctorReceivesMultipleOrders) {
    TestDeferedEventFunctor functor;
    OrderEntry* order1 = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    OrderEntry* order2 = createTestOrder(instrId_, SELL_SIDE, 99.0, 50);

    OrdState::onInternalCancel cancelEvent1;
    OrdState::onInternalCancel cancelEvent2;
    Context context;

    functor.process(cancelEvent1, order1, context);
    functor.process(cancelEvent2, order2, context);

    EXPECT_EQ(functor.internalCancelOrders.size(), 2u);
    EXPECT_EQ(functor.internalCancelOrders[0], order1);
    EXPECT_EQ(functor.internalCancelOrders[1], order2);

    delete order1;
    delete order2;
}
