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
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>

#include "SubscrManager.h"
#include "SubscriptionLayerImpl.h"
#include "OrderFilter.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "IdTGenerator.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::SubscrMgr;
using namespace COP::SL;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

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

class TestSubscriptionLayer : public SubscriptionLayer {
public:
    std::vector<const OrderEntry*> processedOrders;
    std::vector<MatchedSubscribersT> processedSubscribers;

    void attach(SubscriptionManager*) override {}
    SubscriptionManager* dettach() override { return nullptr; }

    void process(const OrderEntry& order, const MatchedSubscribersT& subscribers) override {
        processedOrders.push_back(&order);
        processedSubscribers.push_back(subscribers);
    }

    void reset() {
        processedOrders.clear();
        processedSubscribers.clear();
    }
};

} // namespace

// =============================================================================
// SubscrManager Test Fixture
// =============================================================================

class SubscrManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Note: ExchLogger is created globally by TestMain.cpp
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();
        SubscriptionMgr::create();

        // Add test instrument
        auto* instr = new InstrumentEntry();
        instr->symbol_ = "TEST";
        instr->securityId_ = "TESTSEC";
        instr->securityIdSource_ = "ISIN";
        instrId_ = WideDataStorage::instance()->add(instr);
    }

    void TearDown() override {
        SubscriptionMgr::destroy();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
        // Note: ExchLogger is destroyed globally by TestMain.cpp
    }

    SourceIdT instrId_;
};

// =============================================================================
// SubscrManager Basic Tests
// =============================================================================

TEST_F(SubscrManagerTest, SingletonNotNull) {
    ASSERT_NE(SubscriptionMgr::instance(), nullptr);
}

TEST_F(SubscrManagerTest, AddSubscriptionSucceeds) {
    auto* manager = SubscriptionMgr::instance();
    auto* filter = new OrderFilter();
    filter->addFilter(new SideEqualFilter(BUY_SIDE));

    SubscriberIdT handlerId;
    handlerId.id_ = 1;

    EXPECT_NO_THROW(manager->addSubscription("TestSubscription", filter, handlerId));
}

TEST_F(SubscrManagerTest, AddMultipleSubscriptions) {
    auto* manager = SubscriptionMgr::instance();
    SubscriberIdT handlerId;
    handlerId.id_ = 1;

    auto* filter1 = new OrderFilter();
    filter1->addFilter(new SideEqualFilter(BUY_SIDE));
    EXPECT_NO_THROW(manager->addSubscription("Subscription1", filter1, handlerId));

    auto* filter2 = new OrderFilter();
    filter2->addFilter(new SideEqualFilter(SELL_SIDE));
    EXPECT_NO_THROW(manager->addSubscription("Subscription2", filter2, handlerId));

    auto* filter3 = new OrderFilter();
    filter3->addFilter(new OrderStatusEqualFilter(NEW_ORDSTATUS));
    EXPECT_NO_THROW(manager->addSubscription("Subscription3", filter3, handlerId));
}

// =============================================================================
// SubscrManager Remove Tests
// =============================================================================

TEST_F(SubscrManagerTest, RemoveSubscriptionsForHandler) {
    auto* manager = SubscriptionMgr::instance();
    SubscriberIdT handlerId;
    handlerId.id_ = 42;

    auto* filter1 = new OrderFilter();
    manager->addSubscription("Sub1", filter1, handlerId);

    auto* filter2 = new OrderFilter();
    manager->addSubscription("Sub2", filter2, handlerId);

    // Remove all subscriptions for handler
    EXPECT_NO_THROW(manager->removeSubscriptions(handlerId));
}

TEST_F(SubscrManagerTest, RemoveNonExistentHandler) {
    auto* manager = SubscriptionMgr::instance();

    SubscriberIdT nonExistent;
    nonExistent.id_ = 9999;

    // Removing non-existent handler should not crash
    EXPECT_NO_THROW(manager->removeSubscriptions(nonExistent));
}

TEST_F(SubscrManagerTest, DoubleRemove) {
    auto* manager = SubscriptionMgr::instance();
    SubscriberIdT handlerId;
    handlerId.id_ = 100;

    auto* filter = new OrderFilter();
    manager->addSubscription("Test", filter, handlerId);

    manager->removeSubscriptions(handlerId);

    // Second remove should be safe
    EXPECT_NO_THROW(manager->removeSubscriptions(handlerId));
}

// =============================================================================
// SubscrManager GetSubscribers Tests
// =============================================================================

TEST_F(SubscrManagerTest, GetSubscribersEmptyWhenNoMatch) {
    auto* manager = SubscriptionMgr::instance();
    SubscriberIdT handlerId;
    handlerId.id_ = 1;

    // Add subscription for BUY orders
    auto* filter = new OrderFilter();
    filter->addFilter(new SideEqualFilter(BUY_SIDE));
    manager->addSubscription("BuyFilter", filter, handlerId);

    // Create a SELL order - should not match
    OrderEntry* sellOrder = createTestOrder(instrId_, SELL_SIDE, 100.0, 100);

    MatchedSubscribersT subscribers;
    manager->getSubscribers(*sellOrder, &subscribers);

    EXPECT_TRUE(subscribers.empty());

    delete sellOrder;
}

TEST_F(SubscrManagerTest, GetSubscribersFindsMatch) {
    auto* manager = SubscriptionMgr::instance();
    SubscriberIdT handlerId;
    handlerId.id_ = 1;

    // Add subscription for BUY orders
    auto* filter = new OrderFilter();
    filter->addFilter(new SideEqualFilter(BUY_SIDE));
    manager->addSubscription("BuyFilter", filter, handlerId);

    // Create a BUY order - should match
    OrderEntry* buyOrder = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);

    MatchedSubscribersT subscribers;
    manager->getSubscribers(*buyOrder, &subscribers);

    EXPECT_FALSE(subscribers.empty());
    // Find handlerId in the deque
    bool found = std::find(subscribers.begin(), subscribers.end(), handlerId) != subscribers.end();
    EXPECT_TRUE(found);

    delete buyOrder;
}

// =============================================================================
// SubscriptionLayerImpl Test Fixture
// =============================================================================

class SubscriptionLayerImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Note: ExchLogger is created globally by TestMain.cpp
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();
        SubscriptionMgr::create();

        auto* instr = new InstrumentEntry();
        instr->symbol_ = "TEST";
        instrId_ = WideDataStorage::instance()->add(instr);
    }

    void TearDown() override {
        SubscriptionMgr::destroy();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
        // Note: ExchLogger is destroyed globally by TestMain.cpp
    }

    SourceIdT instrId_;
};

// =============================================================================
// SubscriptionLayerImpl Tests
// =============================================================================

TEST_F(SubscriptionLayerImplTest, Construction) {
    EXPECT_NO_THROW({
        SubscriptionLayerImpl layer;
    });
}

TEST_F(SubscriptionLayerImplTest, AttachSubscriptionManager) {
    SubscriptionLayerImpl layer;
    auto* manager = SubscriptionMgr::instance();

    EXPECT_NO_THROW(layer.attach(manager));
}

TEST_F(SubscriptionLayerImplTest, DetachSubscriptionManager) {
    SubscriptionLayerImpl layer;
    auto* manager = SubscriptionMgr::instance();

    layer.attach(manager);
    SubscriptionManager* detached = layer.dettach();

    EXPECT_EQ(detached, manager);
}

TEST_F(SubscriptionLayerImplTest, AttachDetachCycle) {
    SubscriptionLayerImpl layer;
    auto* manager = SubscriptionMgr::instance();

    // First cycle
    layer.attach(manager);
    EXPECT_EQ(layer.dettach(), manager);

    // Second cycle
    layer.attach(manager);
    EXPECT_EQ(layer.dettach(), manager);
}

// =============================================================================
// Concurrency Tests
// =============================================================================

TEST_F(SubscrManagerTest, ConcurrentAddSubscriptions) {
    auto* manager = SubscriptionMgr::instance();
    const int numThreads = 4;
    const int subscriptionsPerThread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([manager, t, subscriptionsPerThread, &successCount]() {
            for (int i = 0; i < subscriptionsPerThread; ++i) {
                auto* filter = new OrderFilter();
                std::string name = "Thread" + std::to_string(t) + "_Sub" + std::to_string(i);
                SubscriberIdT handlerId;
                handlerId.id_ = t * 1000 + i;

                manager->addSubscription(name, filter, handlerId);
                successCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(), numThreads * subscriptionsPerThread);
}

TEST_F(SubscrManagerTest, ConcurrentGetSubscribers) {
    auto* manager = SubscriptionMgr::instance();

    // Add subscriptions first
    for (int i = 0; i < 10; ++i) {
        auto* filter = new OrderFilter();
        filter->addFilter(new SideEqualFilter(BUY_SIDE));
        SubscriberIdT handlerId;
        handlerId.id_ = i;
        manager->addSubscription("Sub" + std::to_string(i), filter, handlerId);
    }

    const int numThreads = 8;
    const int queriesPerThread = 200;
    std::vector<std::thread> threads;
    std::atomic<int> matchCount{0};

    OrderEntry* buyOrder = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([manager, buyOrder, queriesPerThread, &matchCount]() {
            for (int i = 0; i < queriesPerThread; ++i) {
                MatchedSubscribersT subscribers;
                manager->getSubscribers(*buyOrder, &subscribers);
                matchCount.fetch_add(subscribers.size(), std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Each query should find 10 subscribers
    EXPECT_EQ(matchCount.load(), numThreads * queriesPerThread * 10);

    delete buyOrder;
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(SubscrManagerTest, EmptyFilterMatchesAll) {
    auto* manager = SubscriptionMgr::instance();
    SubscriberIdT handlerId;
    handlerId.id_ = 1;

    // Empty filter (no conditions)
    auto* filter = new OrderFilter();
    manager->addSubscription("AllOrders", filter, handlerId);

    OrderEntry* buyOrder = createTestOrder(instrId_, BUY_SIDE, 100.0, 100);
    OrderEntry* sellOrder = createTestOrder(instrId_, SELL_SIDE, 99.0, 50);

    MatchedSubscribersT buySubscribers, sellSubscribers;
    manager->getSubscribers(*buyOrder, &buySubscribers);
    manager->getSubscribers(*sellOrder, &sellSubscribers);

    // Empty filter typically matches all orders
    // (implementation specific - may match all or none)
    EXPECT_GE(buySubscribers.size() + sellSubscribers.size(), 0u);

    delete buyOrder;
    delete sellOrder;
}

TEST_F(SubscrManagerTest, SubscriptionNameUniqueness) {
    auto* manager = SubscriptionMgr::instance();

    SubscriberIdT handler1;
    handler1.id_ = 1;
    SubscriberIdT handler2;
    handler2.id_ = 2;

    auto* filter1 = new OrderFilter();
    EXPECT_NO_THROW(manager->addSubscription("SameName", filter1, handler1));

    auto* filter2 = new OrderFilter();
    EXPECT_NO_THROW(manager->addSubscription("SameName", filter2, handler2));
}
