/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 Migrated from testOrderBook.cpp
*/

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
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

    // A stored order with leaves to trade, as its last report left it: restore() gives an order its own figures as its
    // last report's, as for one restored from its record (#404). The book's depth reads them (#525).
    OrderEntry *storedOrder(SourceIdT instr, IdT orderId, Side side, PriceT price, QuantityT leaves)
    {
        auto order = createOrderWithId(instr, orderId, side, price);
        assignClOrderId(order.get());
        order->status_ = NEW_ORDSTATUS;
        order->orderQty_ = leaves;
        order->leavesQty_ = leaves;
        OrderEntry *stored = order.release();
        OrderStorage::instance()->restore(stored);
        return stored;
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

TEST_F(OrderBookTest, AStopsReleaseIsPendingFromItsMarkUntilTheStopLeavesTheStopBook)
{
    // The instrument's last trade names the releases pending (#518). A trade's claim marks one, a stop is marked once
    // however many trades reach it, the marks keep their order, and each instrument has its own. The mark ends when the
    // stop leaves the stop book, and a stop held again isn't pending.
    auto first = createOrderWithId(instrumentId1_, IdT(1, 1), SELL_SIDE, 1.0);
    first->ordType_ = STOP_ORDERTYPE;
    first->stopPx_ = 5.0;
    auto second = createOrderWithId(instrumentId1_, IdT(2, 1), BUY_SIDE, 1.0);
    second->ordType_ = STOP_ORDERTYPE;
    second->stopPx_ = 7.0;
    auto other = createOrderWithId(instrumentId2_, IdT(3, 1), BUY_SIDE, 1.0);
    other->ordType_ = STOP_ORDERTYPE;
    other->stopPx_ = 7.0;
    const auto pending = [this](const SourceIdT &instrument)
    {
        std::vector<IdT> ids;
        orderBook_->pendingReleases(instrument, &ids);
        return ids;
    };
    EXPECT_FALSE(orderBook_->markReleasePending(*first)) << "a stop the book doesn't hold";
    orderBook_->addStop(*first);
    orderBook_->addStop(*second);
    orderBook_->addStop(*other);
    EXPECT_TRUE(pending(instrumentId1_).empty());

    EXPECT_TRUE(orderBook_->markReleasePending(*second));
    EXPECT_TRUE(orderBook_->markReleasePending(*first));
    EXPECT_TRUE(orderBook_->markReleasePending(*second)) << "another order's trade reached it";
    EXPECT_TRUE(orderBook_->markReleasePending(*other));
    EXPECT_EQ((std::vector<IdT>{ IdT(2, 1), IdT(1, 1) }), pending(instrumentId1_));
    EXPECT_EQ(std::vector<IdT>{ IdT(3, 1) }, pending(instrumentId2_));

    orderBook_->removeStop(*second);
    orderBook_->addStop(*second);
    EXPECT_EQ(std::vector<IdT>{ IdT(1, 1) }, pending(instrumentId1_)) << "a stop held again is pending";
    orderBook_->removeStop(*first);
    EXPECT_TRUE(pending(instrumentId1_).empty());
    EXPECT_FALSE(orderBook_->markReleasePending(*first)) << "a stop no longer held";
    EXPECT_TRUE(pending(instrumentId1_).empty());
    EXPECT_EQ(std::vector<IdT>{ IdT(3, 1) }, pending(instrumentId2_));
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

// =============================================================================
// Depth (#525)
// =============================================================================

// The depth is what the resting orders' last reports gave, not what a decision has left them since: a fill is decided
// under the order's lock, and its report goes out when its transaction runs (#525)
TEST_F(OrderBookTest, SnapshotGivesTheLeavesOfEachOrdersLastReport)
{
    OrderEntry *order = storedOrder(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0, 100);
    orderBook_->add(*order);
    order->leavesQty_ = 60; // a fill of 40 decided, and its report not out

    const BookSnapshot book = orderBook_->getSnapshot(instrumentId1_, OrderStorage::instance());
    ASSERT_EQ(1u, book.bids.size());
    EXPECT_EQ(100u, book.bids[0].totalQty) << "the depth shows a fill before its report";
    EXPECT_EQ(1u, book.bids[0].orderCount);
    EXPECT_TRUE(book.asks.empty());
}

// An order whose first report isn't out is left out, as the order snapshot leaves it out, and so is one whose last
// report left it nothing to trade: a cancel zeroes the leaves at its decision and takes the order out of the book in a
// later operation (#525)
TEST_F(OrderBookTest, SnapshotLeavesOutAnUnreportedOrderAndAClosedOne)
{
    OrderEntry *unreported = storedOrder(instrumentId1_, IdT(1, 1), SELL_SIDE, 5.0, 50);
    unreported->published_.reset();
    OrderEntry *canceled = storedOrder(instrumentId1_, IdT(1, 2), SELL_SIDE, 6.0, 70);
    canceled->leavesQty_ = 0;
    canceled->published_.emplace(*canceled);
    OrderEntry *resting = storedOrder(instrumentId1_, IdT(1, 3), SELL_SIDE, 6.0, 30);
    orderBook_->add(*unreported);
    orderBook_->add(*canceled);
    orderBook_->add(*resting);

    const BookSnapshot book = orderBook_->getSnapshot(instrumentId1_, OrderStorage::instance());
    ASSERT_EQ(1u, book.asks.size()) << "the order with no report has a level";
    EXPECT_EQ(6.0, book.asks[0].price);
    EXPECT_EQ(30u, book.asks[0].totalQty);
    EXPECT_EQ(1u, book.asks[0].orderCount) << "the canceled order is counted";
}

// A level's total is a sum of the orders' leaves, each a QuantityT, and it wrapped past 4,294,967,295 (#525)
TEST_F(OrderBookTest, SnapshotLevelTotalsPastThirtyTwoBits)
{
    orderBook_->add(*storedOrder(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0, 3000000000u));
    orderBook_->add(*storedOrder(instrumentId1_, IdT(1, 2), BUY_SIDE, 5.0, 3000000000u));

    const BookSnapshot book = orderBook_->getSnapshot(instrumentId1_, OrderStorage::instance());
    ASSERT_EQ(1u, book.bids.size());
    EXPECT_EQ(6000000000u, book.bids[0].totalQty);
    EXPECT_EQ(2u, book.bids[0].orderCount);
}

// A report sets its order's figures under the order's write lock on its transaction's worker, while a report on another
// order of the instrument builds the depth on another worker. The depth reads each order's figures under its read
// lock: without it, the two are a data race, which the TSan build reports (#525).
TEST_F(OrderBookTest, SnapshotReadsEachOrdersReportedLeavesUnderItsLock)
{
    OrderEntry *order = storedOrder(instrumentId1_, IdT(1, 1), BUY_SIDE, 5.0, 100);
    orderBook_->add(*order);

    std::atomic<bool> started{ false };
    std::atomic<bool> stop{ false };
    std::thread reporter(
        [order, &started, &stop]
        {
            for (QuantityT leaves = 1; !stop.load(); leaves = (leaves % 100) + 1)
            {
                {
                    oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, true);
                    order->leavesQty_ = leaves;
                    order->published_.emplace(*order);
                }
                started = true;
                std::this_thread::yield();
            }
        });
    while (!started.load())
    {
        std::this_thread::yield();
    }
    const int SNAPSHOTS = 1000;
    int reported = 0;
    for (int i = 0; i < SNAPSHOTS; ++i)
    {
        const BookSnapshot book = orderBook_->getSnapshot(instrumentId1_, OrderStorage::instance());
        if ((1u == book.bids.size()) && (1u <= book.bids[0].totalQty) && (book.bids[0].totalQty <= 100u))
        {
            ++reported;
        }
    }
    stop = true;
    reporter.join();
    EXPECT_EQ(SNAPSHOTS, reported) << "a snapshot gave the order leaves that no report gave it";
}

} // namespace
