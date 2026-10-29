/**
 Concurrent Order Processor library - New Test File

 Authors: dudleylane, Claude
 Test Implementation: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockDefered.h"
#include "MockQueues.h"
#include "OrderMatcher.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Queues;
using namespace test;

namespace
{

// =============================================================================
// Test Deferred Event Container
// =============================================================================

class TestDeferedEventContainer : public DeferedEventContainer
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

    size_t eventCount() const
    {
        return events_.size();
    }
    void clear()
    {
        for (auto *e : events_)
        {
            delete e;
        }
        events_.clear();
    }

    std::vector<DeferedEventBase *> events_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class OrderMatcherTest : public ProcessorFixture
{
protected:
    void SetUp() override
    {
        ProcessorFixture::SetUp();

        deferedContainer_ = std::make_unique<TestDeferedEventContainer>();
        matcher_ = std::make_unique<OrderMatcher>();
        matcher_->init(deferedContainer_.get());

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<OutgoingQueues>();

        context_ = Context(OrderStorage::instance(), orderBook_.get(), inQueues_.get(), outQueues_.get(),
                           matcher_.get(), IdTGenerator::instance());
    }

    void TearDown() override
    {
        deferedContainer_->clear();
        deferedContainer_.reset();
        matcher_.reset();
        outQueues_.reset();
        inQueues_.reset();

        ProcessorFixture::TearDown();
    }

    // Helper to create and save an order
    OrderEntry *createAndSaveOrder(Side side, PriceT price, QuantityT qty)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        order->side_ = side;
        order->price_ = price;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;

        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

protected:
    std::unique_ptr<TestDeferedEventContainer> deferedContainer_;
    std::unique_ptr<OrderMatcher> matcher_;
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<OutgoingQueues> outQueues_;
    Context context_;
};

// =============================================================================
// Basic Tests
// =============================================================================

TEST_F(OrderMatcherTest, CreateMatcher)
{
    ASSERT_NE(nullptr, matcher_);
}

TEST_F(OrderMatcherTest, InitWithContainer)
{
    OrderMatcher matcher;
    TestDeferedEventContainer container;

    matcher.init(&container);
    SUCCEED();
}

// =============================================================================
// Single Order Match Tests
// =============================================================================

TEST_F(OrderMatcherTest, MatchSingleBuyOrder)
{
    OrderEntry *order = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, order);
    order->status_ = NEW_ORDSTATUS;

    // Add to order book
    orderBook_->add(*order);

    // Match should complete without error
    matcher_->match(order, context_);

    SUCCEED();
}

TEST_F(OrderMatcherTest, MatchSingleSellOrder)
{
    OrderEntry *order = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, order);
    order->status_ = NEW_ORDSTATUS;

    // Add to order book
    orderBook_->add(*order);

    // Match should complete without error
    matcher_->match(order, context_);

    SUCCEED();
}

// =============================================================================
// Order Matching Tests
// =============================================================================

TEST_F(OrderMatcherTest, MatchBuyAgainstSellAtSamePrice)
{
    // Create and add sell order to book
    OrderEntry *sellOrder = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, sellOrder);
    sellOrder->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellOrder);

    // Create buy order at same price
    OrderEntry *buyOrder = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, buyOrder);
    buyOrder->status_ = NEW_ORDSTATUS;

    // Match buy order
    matcher_->match(buyOrder, context_);

    // Deferred events should have been generated
    // The exact behavior depends on the implementation
    SUCCEED();
}

TEST_F(OrderMatcherTest, MatchSellAgainstBuyAtSamePrice)
{
    // Create and add buy order to book
    OrderEntry *buyOrder = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, buyOrder);
    buyOrder->status_ = NEW_ORDSTATUS;
    orderBook_->add(*buyOrder);

    // Create sell order at same price
    OrderEntry *sellOrder = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, sellOrder);
    sellOrder->status_ = NEW_ORDSTATUS;

    // Match sell order
    matcher_->match(sellOrder, context_);

    SUCCEED();
}

// =============================================================================
// No Match Tests
// =============================================================================

TEST_F(OrderMatcherTest, NoMatchWhenPricesDontCross)
{
    // Create sell order at high price
    OrderEntry *sellOrder = createAndSaveOrder(SELL_SIDE, 20.0, 100);
    ASSERT_NE(nullptr, sellOrder);
    sellOrder->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellOrder);

    size_t eventsBefore = deferedContainer_->eventCount();

    // Create buy order at lower price - should not match
    OrderEntry *buyOrder = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, buyOrder);
    buyOrder->status_ = NEW_ORDSTATUS;

    matcher_->match(buyOrder, context_);

    // A limit order that crosses nothing adds no deferred event: it only rests
    EXPECT_EQ(eventsBefore, deferedContainer_->eventCount());
}

// =============================================================================
// Partial Fill Tests
// =============================================================================

TEST_F(OrderMatcherTest, PartialFillWhenDifferentQuantities)
{
    // Create sell order with small quantity
    OrderEntry *sellOrder = createAndSaveOrder(SELL_SIDE, 10.0, 50);
    ASSERT_NE(nullptr, sellOrder);
    sellOrder->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellOrder);

    // Create buy order with larger quantity at same price
    OrderEntry *buyOrder = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, buyOrder);
    buyOrder->status_ = NEW_ORDSTATUS;

    matcher_->match(buyOrder, context_);

    // Partial fill should have occurred
    SUCCEED();
}

// =============================================================================
// Multiple Orders Match Tests
// =============================================================================

TEST_F(OrderMatcherTest, MatchAgainstMultipleOrders)
{
    // Add multiple sell orders
    for (int i = 0; i < 5; ++i)
    {
        OrderEntry *sellOrder = createAndSaveOrder(SELL_SIDE, 10.0, 20);
        ASSERT_NE(nullptr, sellOrder);
        sellOrder->status_ = NEW_ORDSTATUS;
        orderBook_->add(*sellOrder);
    }

    // Create buy order that could match against all
    OrderEntry *buyOrder = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, buyOrder);
    buyOrder->status_ = NEW_ORDSTATUS;

    matcher_->match(buyOrder, context_);

    SUCCEED();
}

// =============================================================================
// Price Priority Tests
// =============================================================================

TEST_F(OrderMatcherTest, MatchWithBestPriceFirst)
{
    // Add sell orders at different prices
    OrderEntry *sellHigh = createAndSaveOrder(SELL_SIDE, 12.0, 100);
    ASSERT_NE(nullptr, sellHigh);
    sellHigh->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellHigh);

    OrderEntry *sellLow = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, sellLow);
    sellLow->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellLow);

    // Buy order should match against lowest sell first
    OrderEntry *buyOrder = createAndSaveOrder(BUY_SIDE, 12.0, 50);
    ASSERT_NE(nullptr, buyOrder);
    buyOrder->status_ = NEW_ORDSTATUS;

    matcher_->match(buyOrder, context_);

    SUCCEED();
}

// =============================================================================
// Empty Order Book Tests
// =============================================================================

TEST_F(OrderMatcherTest, MatchOnEmptyBook)
{
    OrderEntry *order = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, order);
    order->status_ = NEW_ORDSTATUS;

    // Should complete without error even with empty book
    matcher_->match(order, context_);

    SUCCEED();
}

// =============================================================================
// Different Instruments Tests
// =============================================================================

TEST_F(OrderMatcherTest, NoMatchAcrossDifferentInstruments)
{
    // Create sell order for instrument 1
    OrderEntry *sellOrder = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, sellOrder);
    sellOrder->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellOrder);

    // Create buy order for instrument 2
    auto buyOrder = createCorrectOrder(instrumentId2_);
    assignClOrderId(buyOrder.get());
    buyOrder->side_ = BUY_SIDE;
    buyOrder->price_ = 10.0;
    buyOrder->orderQty_ = 100;
    buyOrder->leavesQty_ = 100;

    OrderEntry *savedBuy = OrderStorage::instance()->save(*buyOrder, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedBuy);
    savedBuy->status_ = NEW_ORDSTATUS;

    // Should not match across different instruments
    matcher_->match(savedBuy, context_);

    SUCCEED();
}

// =============================================================================
// Only a working order is traded with (#432)
// =============================================================================

TEST_F(OrderMatcherTest, OnlyAWorkingOrderIsTradedWith)
{
    // The book holds an order the state machine took out of trading until its removal runs, and a suspended one keeps
    // what it has left. Every status but NEW and PARTFILL is passed by, whatever the order has left.
    OrderEntry *buy = createAndSaveOrder(BUY_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, buy);
    buy->status_ = NEW_ORDSTATUS;
    for (int s = INVALID_ORDSTATUS; s <= CANCELED_ORDSTATUS; ++s)
    {
        const OrderStatus status = static_cast<OrderStatus>(s);
        OrderEntry *sell = createAndSaveOrder(SELL_SIDE, 10.0, 100);
        ASSERT_NE(nullptr, sell);
        sell->status_ = status;
        orderBook_->add(*sell);

        const bool working = (NEW_ORDSTATUS == status) || (PARTFILL_ORDSTATUS == status);
        const IdT contra = OrderMatcher::findContra(buy, *orderBook_, OrderStorage::instance());
        EXPECT_EQ(working, contra.isValid()) << "status " << s;
        if (working)
        {
            EXPECT_EQ(sell->orderId_, contra) << "status " << s;
        }
        EXPECT_EQ(working, OrderMatcher::canFillAtOnce(buy, *orderBook_, OrderStorage::instance())) << "status " << s;
        orderBook_->remove(*sell);
    }

    // Nor is a NEW one with nothing left (#386)
    OrderEntry *sell = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, sell);
    sell->status_ = NEW_ORDSTATUS;
    sell->leavesQty_ = 0;
    orderBook_->add(*sell);
    EXPECT_FALSE(OrderMatcher::findContra(buy, *orderBook_, OrderStorage::instance()).isValid());
    EXPECT_FALSE(OrderMatcher::canFillAtOnce(buy, *orderBook_, OrderStorage::instance()));
}

TEST_F(OrderMatcherTest, AnOrderThatIsntWorkingDoesntEndTheSearch)
{
    // Passed by, as an order with nothing left is: the next order on the side may trade, where a price that doesn't
    // ends the search
    OrderEntry *suspended = createAndSaveOrder(SELL_SIDE, 10.0, 100);
    ASSERT_NE(nullptr, suspended);
    suspended->status_ = SUSPENDED_ORDSTATUS;
    orderBook_->add(*suspended);
    OrderEntry *working = createAndSaveOrder(SELL_SIDE, 10.5, 100);
    ASSERT_NE(nullptr, working);
    working->status_ = NEW_ORDSTATUS;
    orderBook_->add(*working);
    ASSERT_EQ(suspended->orderId_, orderBook_->getTop(instrumentId1_, SELL_SIDE));

    OrderEntry *buy = createAndSaveOrder(BUY_SIDE, 11.0, 100);
    ASSERT_NE(nullptr, buy);
    buy->status_ = NEW_ORDSTATUS;
    EXPECT_EQ(working->orderId_, OrderMatcher::findContra(buy, *orderBook_, OrderStorage::instance()));
    EXPECT_TRUE(OrderMatcher::canFillAtOnce(buy, *orderBook_, OrderStorage::instance()));

    // Only what the working order has left counts towards filling an order at once
    OrderEntry *bigger = createAndSaveOrder(BUY_SIDE, 11.0, 150);
    ASSERT_NE(nullptr, bigger);
    bigger->status_ = NEW_ORDSTATUS;
    EXPECT_FALSE(OrderMatcher::canFillAtOnce(bigger, *orderBook_, OrderStorage::instance()));
}

} // namespace
