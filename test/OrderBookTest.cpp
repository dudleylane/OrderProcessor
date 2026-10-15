/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 Migrated from testOrderBook.cpp
*/

#include <gtest/gtest.h>
#include "TestFixtures.h"
#include "TestAux.h"
#include "MockOrderBook.h"
#include "OrderBookImpl.h"

using namespace COP;
using namespace COP::Store;
using namespace test;

namespace
{

// =============================================================================
// OrderBook Test Fixture
// =============================================================================

class OrderBookTest : public OrderBookFixture
{
protected:
    void SetUp() override
    {
        OrderBookFixture::SetUp();
    }

    void TearDown() override
    {
        OrderBookFixture::TearDown();
    }

    // Helper to create an order with specific properties
    std::unique_ptr<OrderEntry> createOrderWithId(SourceIdT instr, IdT orderId, Side side, PriceT price)
    {
        auto order = createCorrectOrder(instr);
        order->orderId_ = orderId;
        order->side_ = side;
        order->price_ = price;
        return order;
    }
};

// =============================================================================
// Basic Initialization Tests
// =============================================================================

TEST_F(OrderBookTest, InitWithEmptyInstruments)
{
    DummyOrderSaver saver;
    OrderBookImpl books;
    OrderBookImpl::InstrumentsT emptyInstr;
    books.init(emptyInstr);
    // Should not throw
    SUCCEED();
}

TEST_F(OrderBookTest, InitWithInstruments)
{
    // Already initialized in fixture - just verify instruments were added
    ASSERT_TRUE(instrumentId1_.isValid());
    ASSERT_TRUE(instrumentId2_.isValid());
}

// =============================================================================
// Find with Invalid Parameters Tests
// =============================================================================

TEST_F(OrderBookTest, FindWithInvalidInstrumentThrows)
{
    TestOrderFunctor functor(SourceIdT(), BUY_SIDE, true);
    EXPECT_THROW(orderBook_->find(functor), std::exception);
}

TEST_F(OrderBookTest, FindAllWithInvalidInstrumentThrows)
{
    TestOrderFunctor functor(SourceIdT(), BUY_SIDE, true);
    OrderBookImpl::OrdersT orders;
    EXPECT_THROW(orderBook_->findAll(functor, &orders), std::exception);
}

TEST_F(OrderBookTest, FindWithInvalidSideThrows)
{
    TestOrderFunctor functor(instrumentId1_, INVALID_SIDE, true);
    EXPECT_THROW(orderBook_->find(functor), std::exception);
}

TEST_F(OrderBookTest, FindAllWithInvalidSideThrows)
{
    TestOrderFunctor functor(instrumentId1_, INVALID_SIDE, true);
    OrderBookImpl::OrdersT orders;
    EXPECT_THROW(orderBook_->findAll(functor, &orders), std::exception);
}

// =============================================================================
// GetTop with Invalid Parameters Tests
// =============================================================================

TEST_F(OrderBookTest, GetTopWithInvalidInstrumentThrows)
{
    EXPECT_THROW(orderBook_->getTop(SourceIdT(), BUY_SIDE), std::exception);
}

TEST_F(OrderBookTest, GetTopWithInvalidSideThrows)
{
    EXPECT_THROW(orderBook_->getTop(instrumentId1_, INVALID_SIDE), std::exception);
}

// =============================================================================
// Empty Order Book Tests
// =============================================================================

TEST_F(OrderBookTest, FindOnEmptyBookReturnsInvalidId)
{
    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    EXPECT_FALSE(orderBook_->find(functor).isValid());
}

TEST_F(OrderBookTest, FindAllOnEmptyBookReturnsEmptyList)
{
    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    OrderBookImpl::OrdersT orders;
    orderBook_->findAll(functor, &orders);
    EXPECT_TRUE(orders.empty());
}

TEST_F(OrderBookTest, GetTopOnEmptyBookReturnsInvalidId)
{
    EXPECT_FALSE(orderBook_->getTop(instrumentId1_, BUY_SIDE).isValid());
}

// =============================================================================
// Add Order to Unknown Instrument Tests
// =============================================================================

TEST_F(OrderBookTest, AddOrderWithUnknownInstrumentThrows)
{
    SourceIdT unknownInstr = addInstrument("ccc");
    auto order = createCorrectOrder(unknownInstr);
    order->orderId_ = IdT(2, 1);

    EXPECT_THROW(orderBook_->add(*order), std::exception);
}

// =============================================================================
// Buy Side Order Tests
// =============================================================================

TEST_F(OrderBookTest, AddSingleBuyOrder)
{
    auto order = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    orderBook_->add(*order);

    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    EXPECT_TRUE(orderBook_->find(functor).isValid());
    EXPECT_EQ(IdT(1, 1), orderBook_->getTop(instrumentId1_, BUY_SIDE));
}

TEST_F(OrderBookTest, AddMultipleBuyOrdersPriceOrdering)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 2), BUY_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 3), BUY_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    OrderBookImpl::OrdersT orders;
    orderBook_->findAll(functor, &orders);

    ASSERT_EQ(3u, orders.size());
    // Buy side: highest price first
    EXPECT_EQ(IdT(1, 3), orders.at(0));
    EXPECT_EQ(IdT(1, 2), orders.at(1));
    EXPECT_EQ(IdT(1, 1), orders.at(2));
}

TEST_F(OrderBookTest, BuySideTopIsHighestPrice)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 2), BUY_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 3), BUY_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    EXPECT_EQ(IdT(1, 3), orderBook_->getTop(instrumentId1_, BUY_SIDE));
}

TEST_F(OrderBookTest, FindOnBuySideReturnsResult)
{
    auto order = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    orderBook_->add(*order);

    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    EXPECT_TRUE(orderBook_->find(functor).isValid());
}

TEST_F(OrderBookTest, FindOnSellSideWithBuyOrdersReturnsInvalid)
{
    auto order = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    orderBook_->add(*order);

    TestOrderFunctor functor(instrumentId1_, SELL_SIDE, true);
    EXPECT_FALSE(orderBook_->find(functor).isValid());
}

// =============================================================================
// Remove Buy Orders Tests
// =============================================================================

TEST_F(OrderBookTest, RemoveBuyOrderFromMiddle)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 2), BUY_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 3), BUY_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    // Remove middle order
    orderBook_->remove(*order2);

    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    OrderBookImpl::OrdersT orders;
    orderBook_->findAll(functor, &orders);

    ASSERT_EQ(2u, orders.size());
    EXPECT_EQ(IdT(1, 3), orders.at(0));
    EXPECT_EQ(IdT(1, 1), orders.at(1));
}

TEST_F(OrderBookTest, RemoveBuyOrderFromTop)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 2), BUY_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 3), BUY_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    // Remove top order
    orderBook_->remove(*order3);
    EXPECT_EQ(IdT(1, 2), orderBook_->getTop(instrumentId1_, BUY_SIDE));
}

TEST_F(OrderBookTest, RemoveAllBuyOrders)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    orderBook_->add(*order1);

    orderBook_->remove(*order1);

    EXPECT_FALSE(orderBook_->getTop(instrumentId1_, BUY_SIDE).isValid());

    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    OrderBookImpl::OrdersT orders;
    orderBook_->findAll(functor, &orders);
    EXPECT_EQ(0u, orders.size());
}

TEST_F(OrderBookTest, RemoveNonExistentBuyOrderThrows)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    orderBook_->add(*order1);

    auto nonExistent = createOrderWithId(instrumentId1_, IdT(1, 99), BUY_SIDE, 5.0);
    EXPECT_THROW(orderBook_->remove(*nonExistent), std::exception);
}

// =============================================================================
// Sell Side Order Tests
// =============================================================================

TEST_F(OrderBookTest, AddMultipleSellOrdersPriceOrdering)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 4), SELL_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 5), SELL_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 6), SELL_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    TestOrderFunctor functor(instrumentId1_, SELL_SIDE, true);
    OrderBookImpl::OrdersT orders;
    orderBook_->findAll(functor, &orders);

    ASSERT_EQ(3u, orders.size());
    // Sell side: lowest price first
    EXPECT_EQ(IdT(1, 4), orders.at(0));
    EXPECT_EQ(IdT(1, 5), orders.at(1));
    EXPECT_EQ(IdT(1, 6), orders.at(2));
}

TEST_F(OrderBookTest, SellSideTopIsLowestPrice)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 4), SELL_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 5), SELL_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 6), SELL_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    EXPECT_EQ(IdT(1, 4), orderBook_->getTop(instrumentId1_, SELL_SIDE));
}

TEST_F(OrderBookTest, RemoveSellOrderFromMiddle)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 4), SELL_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 5), SELL_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 6), SELL_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    orderBook_->remove(*order2);

    TestOrderFunctor functor(instrumentId1_, SELL_SIDE, true);
    OrderBookImpl::OrdersT orders;
    orderBook_->findAll(functor, &orders);

    ASSERT_EQ(2u, orders.size());
    EXPECT_EQ(IdT(1, 4), orders.at(0));
    EXPECT_EQ(IdT(1, 6), orders.at(1));
}

TEST_F(OrderBookTest, RemoveSellOrderFromTop)
{
    auto order1 = createOrderWithId(instrumentId1_, IdT(1, 4), SELL_SIDE, 5.0);
    auto order2 = createOrderWithId(instrumentId1_, IdT(1, 5), SELL_SIDE, 5.0001);
    auto order3 = createOrderWithId(instrumentId1_, IdT(1, 6), SELL_SIDE, 5.00011);

    orderBook_->add(*order1);
    orderBook_->add(*order2);
    orderBook_->add(*order3);

    // Remove bottom (top for sell side is lowest price)
    orderBook_->remove(*order3);

    EXPECT_EQ(IdT(1, 4), orderBook_->getTop(instrumentId1_, SELL_SIDE));
}

TEST_F(OrderBookTest, RemoveAllSellOrders)
{
    auto order = createOrderWithId(instrumentId1_, IdT(1, 4), SELL_SIDE, 5.0);
    orderBook_->add(*order);

    orderBook_->remove(*order);

    EXPECT_FALSE(orderBook_->getTop(instrumentId1_, SELL_SIDE).isValid());
}

TEST_F(OrderBookTest, RemoveNonExistentSellOrderThrows)
{
    auto order = createOrderWithId(instrumentId1_, IdT(1, 4), SELL_SIDE, 5.0);
    orderBook_->add(*order);

    auto nonExistent = createOrderWithId(instrumentId1_, IdT(1, 99), SELL_SIDE, 5.0);
    EXPECT_THROW(orderBook_->remove(*nonExistent), std::exception);
}

// =============================================================================
// Stop Book Tests (#223)
// =============================================================================

TEST_F(OrderBookTest, AHeldStopIsInNeitherSideOfThePriceBook)
{
    auto stop = createOrderWithId(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0);
    stop->ordType_ = STOPLIMIT_ORDERTYPE;
    stop->stopPx_ = 6.0;
    EXPECT_FALSE(orderBook_->holdsStops(instrumentId1_));

    orderBook_->addStop(*stop);
    EXPECT_TRUE(orderBook_->holdsStops(instrumentId1_));
    EXPECT_FALSE(orderBook_->holdsStops(instrumentId2_));
    EXPECT_FALSE(orderBook_->getTop(instrumentId1_, BUY_SIDE).isValid());
    TestOrderFunctor functor(instrumentId1_, BUY_SIDE, true);
    EXPECT_FALSE(orderBook_->find(functor).isValid());

    orderBook_->removeStop(*stop);
    EXPECT_FALSE(orderBook_->holdsStops(instrumentId1_));
}

TEST_F(OrderBookTest, ATradeReachesBuyStopsAtOrBelowItAndSellStopsAtOrAboveIt)
{
    // In the order a moving price would reach them: buy stops from the lowest, sell stops from the highest, and stops at
    // one price in the order they were added
    const struct
    {
        u64 id_;
        Side side_;
        PriceT stopPx_;
    } stops[] = { { 1, BUY_SIDE, 6.0 },  { 2, BUY_SIDE, 5.0 },  { 3, BUY_SIDE, 7.0 }, { 4, SELL_SIDE, 6.0 },
                  { 5, SELL_SIDE, 7.0 }, { 6, SELL_SIDE, 5.0 }, { 7, BUY_SIDE, 6.0 } };
    for (const auto &s : stops)
    {
        auto stop = createOrderWithId(instrumentId1_, IdT(s.id_, 1), s.side_, 1.0);
        stop->ordType_ = STOP_ORDERTYPE;
        stop->stopPx_ = s.stopPx_;
        orderBook_->addStop(*stop);
    }

    OrderBookImpl::OrdersT reached;
    orderBook_->triggeredStops(instrumentId1_, 6.0, &reached);
    EXPECT_EQ((OrderBookImpl::OrdersT{ IdT(2, 1), IdT(1, 1), IdT(7, 1), IdT(5, 1), IdT(4, 1) }), reached);

    // They stay held: the release takes each out
    reached.clear();
    orderBook_->triggeredStops(instrumentId1_, 6.0, &reached);
    EXPECT_EQ(5u, reached.size());
    reached.clear();
    orderBook_->triggeredStops(instrumentId2_, 6.0, &reached);
    EXPECT_TRUE(reached.empty());
}

TEST_F(OrderBookTest, RemovingAStopTakesOnlyThatOne)
{
    auto first = createOrderWithId(instrumentId1_, IdT(1, 1), SELL_SIDE, 1.0);
    first->ordType_ = STOP_ORDERTYPE;
    first->stopPx_ = 5.0;
    auto second = createOrderWithId(instrumentId1_, IdT(2, 1), SELL_SIDE, 1.0);
    second->ordType_ = STOP_ORDERTYPE;
    second->stopPx_ = 5.0;
    orderBook_->addStop(*first);
    orderBook_->addStop(*second);

    orderBook_->removeStop(*first);
    EXPECT_TRUE(orderBook_->holdsStops(instrumentId1_));
    OrderBookImpl::OrdersT reached;
    orderBook_->triggeredStops(instrumentId1_, 5.0, &reached);
    EXPECT_EQ(OrderBookImpl::OrdersT{ IdT(2, 1) }, reached);

    // An order the book doesn't hold is refused, and changes nothing
    EXPECT_THROW(orderBook_->removeStop(*first), std::exception);
    EXPECT_TRUE(orderBook_->holdsStops(instrumentId1_));
    orderBook_->removeStop(*second);
    EXPECT_FALSE(orderBook_->holdsStops(instrumentId1_));
}

TEST_F(OrderBookTest, AStopOfAnInstrumentTheBookDoesNotTradeIsRefused)
{
    const SourceIdT other = test::addInstrument("ccc");
    auto stop = createOrderWithId(other, IdT(1, 1), BUY_SIDE, 1.0);
    stop->ordType_ = STOP_ORDERTYPE;
    stop->stopPx_ = 5.0;
    EXPECT_THROW(orderBook_->addStop(*stop), std::exception);
    EXPECT_THROW(orderBook_->removeStop(*stop), std::exception);
    EXPECT_FALSE(orderBook_->holdsStops(other));
    OrderBookImpl::OrdersT reached;
    orderBook_->triggeredStops(other, 5.0, &reached);
    EXPECT_TRUE(reached.empty());
}

} // namespace
