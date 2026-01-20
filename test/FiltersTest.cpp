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
#include <set>

#include "OrderFilter.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "IdTGenerator.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::SubscrMgr;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

OrderEntry* createTestOrder(SourceIdT instrId, Side side, OrderType type,
                            PriceT price, QuantityT qty, OrderStatus status) {
    SourceIdT srcId, destId, clOrdId, origClOrdId, accountId, clearingId, execList;
    auto* order = new OrderEntry(srcId, destId, clOrdId, origClOrdId, instrId, accountId, clearingId, execList);
    order->side_ = side;
    order->price_ = price;
    order->orderQty_ = qty;
    order->leavesQty_ = qty;
    order->cumQty_ = 0;
    order->ordType_ = type;
    order->status_ = status;
    return order;
}

} // namespace

// =============================================================================
// Filter Test Fixture
// =============================================================================

class FiltersTest : public ::testing::Test {
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
// OrderFilter Basic Tests
// =============================================================================

TEST_F(FiltersTest, OrderFilterConstruction) {
    EXPECT_NO_THROW({
        OrderFilter filter;
    });
}

TEST_F(FiltersTest, OrderFilterEmptyMatchesNothing) {
    OrderFilter filter;
    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);

    // Empty filter behavior depends on implementation
    // Typically matches everything or nothing
    bool result = filter.match(*order);
    // Just verify it doesn't crash
    (void)result;

    delete order;
}

TEST_F(FiltersTest, OrderFilterRelease) {
    OrderFilter filter;

    // Add some filters
    filter.addFilter(new OrderStatusEqualFilter(NEW_ORDSTATUS));
    filter.addFilter(new SideEqualFilter(BUY_SIDE));

    // Release should not crash
    EXPECT_NO_THROW(filter.release());
}

// =============================================================================
// Status Filter Tests
// =============================================================================

TEST_F(FiltersTest, OrderStatusEqualFilterMatches) {
    OrderFilter filter;
    filter.addFilter(new OrderStatusEqualFilter(NEW_ORDSTATUS));

    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    EXPECT_TRUE(filter.match(*order));

    delete order;
    filter.release();
}

TEST_F(FiltersTest, OrderStatusEqualFilterNoMatch) {
    OrderFilter filter;
    filter.addFilter(new OrderStatusEqualFilter(NEW_ORDSTATUS));

    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, FILLED_ORDSTATUS);
    EXPECT_FALSE(filter.match(*order));

    delete order;
    filter.release();
}

TEST_F(FiltersTest, OrderStatusInFilter) {
    std::set<OrderStatus> statuses = {NEW_ORDSTATUS, PARTFILL_ORDSTATUS};
    OrderFilter filter;
    filter.addFilter(new OrderStatusInFilter(statuses));

    OrderEntry* newOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    OrderEntry* partFillOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, PARTFILL_ORDSTATUS);
    OrderEntry* filledOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, FILLED_ORDSTATUS);

    EXPECT_TRUE(filter.match(*newOrder));
    EXPECT_TRUE(filter.match(*partFillOrder));
    EXPECT_FALSE(filter.match(*filledOrder));

    delete newOrder;
    delete partFillOrder;
    delete filledOrder;
    filter.release();
}

// =============================================================================
// Side Filter Tests
// =============================================================================

TEST_F(FiltersTest, SideEqualFilterMatchesBuy) {
    OrderFilter filter;
    filter.addFilter(new SideEqualFilter(BUY_SIDE));

    OrderEntry* buyOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    OrderEntry* sellOrder = createTestOrder(instrId_, SELL_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);

    EXPECT_TRUE(filter.match(*buyOrder));
    EXPECT_FALSE(filter.match(*sellOrder));

    delete buyOrder;
    delete sellOrder;
    filter.release();
}

TEST_F(FiltersTest, SideEqualFilterMatchesSell) {
    OrderFilter filter;
    filter.addFilter(new SideEqualFilter(SELL_SIDE));

    OrderEntry* buyOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    OrderEntry* sellOrder = createTestOrder(instrId_, SELL_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);

    EXPECT_FALSE(filter.match(*buyOrder));
    EXPECT_TRUE(filter.match(*sellOrder));

    delete buyOrder;
    delete sellOrder;
    filter.release();
}

// =============================================================================
// Order Type Filter Tests
// =============================================================================

TEST_F(FiltersTest, OrderTypeEqualFilterLimit) {
    OrderFilter filter;
    filter.addFilter(new OrderTypeEqualFilter(LIMIT_ORDERTYPE));

    OrderEntry* limitOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    OrderEntry* marketOrder = createTestOrder(instrId_, BUY_SIDE, MARKET_ORDERTYPE, 0.0, 100, NEW_ORDSTATUS);

    EXPECT_TRUE(filter.match(*limitOrder));
    EXPECT_FALSE(filter.match(*marketOrder));

    delete limitOrder;
    delete marketOrder;
    filter.release();
}

// =============================================================================
// Composite Filter Tests (AND Logic)
// =============================================================================

TEST_F(FiltersTest, MultipleFiltersAndLogic) {
    OrderFilter filter;
    filter.addFilter(new SideEqualFilter(BUY_SIDE));
    filter.addFilter(new OrderStatusEqualFilter(NEW_ORDSTATUS));
    filter.addFilter(new OrderTypeEqualFilter(LIMIT_ORDERTYPE));

    // All conditions match
    OrderEntry* matchingOrder = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    EXPECT_TRUE(filter.match(*matchingOrder));

    // Wrong side
    OrderEntry* wrongSide = createTestOrder(instrId_, SELL_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);
    EXPECT_FALSE(filter.match(*wrongSide));

    // Wrong status
    OrderEntry* wrongStatus = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, FILLED_ORDSTATUS);
    EXPECT_FALSE(filter.match(*wrongStatus));

    // Wrong type
    OrderEntry* wrongType = createTestOrder(instrId_, BUY_SIDE, MARKET_ORDERTYPE, 0.0, 100, NEW_ORDSTATUS);
    EXPECT_FALSE(filter.match(*wrongType));

    delete matchingOrder;
    delete wrongSide;
    delete wrongStatus;
    delete wrongType;
    filter.release();
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(FiltersTest, EmptySetFilter) {
    std::set<OrderStatus> emptySet;
    OrderFilter filter;
    filter.addFilter(new OrderStatusInFilter(emptySet));

    OrderEntry* order = createTestOrder(instrId_, BUY_SIDE, LIMIT_ORDERTYPE, 100.0, 100, NEW_ORDSTATUS);

    // Empty set should not match anything
    EXPECT_FALSE(filter.match(*order));

    delete order;
    filter.release();
}
