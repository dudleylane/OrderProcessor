/**
 Concurrent Order Processor library - New Test File

 Author: Sergey Mikhailik
 Test Implementation: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "TestFixtures.h"
#include "TestAux.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::Store;
using namespace test;

namespace {

// =============================================================================
// Test Fixture
// =============================================================================

class OrderStorageTest : public OrderStorageFixture {
protected:
    void SetUp() override {
        OrderStorageFixture::SetUp();
    }

    void TearDown() override {
        OrderStorageFixture::TearDown();
    }

    OrderDataStorage* storage() {
        return OrderStorage::instance();
    }
};

// =============================================================================
// Basic Singleton Tests
// =============================================================================

TEST_F(OrderStorageTest, InstanceNotNull) {
    ASSERT_NE(nullptr, storage());
}

TEST_F(OrderStorageTest, InstanceReturnsSamePointer) {
    auto* ptr1 = storage();
    auto* ptr2 = storage();
    EXPECT_EQ(ptr1, ptr2);
}

// =============================================================================
// Order Save Tests
// =============================================================================

TEST_F(OrderStorageTest, SaveOrderAssignsId) {
    auto order = createCorrectOrder();
    ASSERT_FALSE(order->orderId_.isValid());

    OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());

    ASSERT_NE(nullptr, saved);
    EXPECT_TRUE(saved->orderId_.isValid());
}

TEST_F(OrderStorageTest, SaveMultipleOrdersAssignsUniqueIds) {
    std::vector<IdT> orderIds;

    for (int i = 0; i < 10; ++i) {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());

        OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
        ASSERT_NE(nullptr, saved);

        // Check uniqueness
        for (const auto& existingId : orderIds) {
            EXPECT_NE(existingId, saved->orderId_);
        }
        orderIds.push_back(saved->orderId_);
    }
}

// =============================================================================
// Order Lookup by OrderId Tests
// =============================================================================

TEST_F(OrderStorageTest, LocateByOrderIdFindsOrder) {
    auto order = createCorrectOrder();
    OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);

    OrderEntry* found = storage()->locateByOrderId(saved->orderId_);

    ASSERT_NE(nullptr, found);
    EXPECT_EQ(saved->orderId_, found->orderId_);
}

TEST_F(OrderStorageTest, LocateByOrderIdReturnsNullForMissing) {
    IdT nonExistentId(9999, 9999);

    OrderEntry* found = storage()->locateByOrderId(nonExistentId);

    EXPECT_EQ(nullptr, found);
}

// =============================================================================
// Order Lookup by ClOrderId Tests
// =============================================================================

TEST_F(OrderStorageTest, LocateByClOrderIdFindsOrder) {
    auto order = createCorrectOrder();
    assignClOrderId(order.get());
    RawDataEntry clOrdId = order->clOrderId_.get();

    OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);

    OrderEntry* found = storage()->locateByClOrderId(clOrdId);

    ASSERT_NE(nullptr, found);
    EXPECT_EQ(saved->orderId_, found->orderId_);
}

TEST_F(OrderStorageTest, LocateByClOrderIdReturnsNullForMissing) {
    RawDataEntry nonExistentClId(STRING_RAWDATATYPE, "NonExistent", 11);

    OrderEntry* found = storage()->locateByClOrderId(nonExistentClId);

    EXPECT_EQ(nullptr, found);
}

// =============================================================================
// Order Restore Tests
// =============================================================================

TEST_F(OrderStorageTest, RestoreLoadsOrder) {
    // restore() is for loading orders from persistence (creates new entry)
    // Create an order with a pre-assigned OrderId (as if from a different storage)
    auto order = test::createCorrectOrder();
    order->orderId_ = IdTGenerator::instance()->getId();
    order->status_ = NEW_ORDSTATUS;
    order->price_ = 99.99;

    IdT savedOrderId = order->orderId_;

    // Restore the order (simulating a load from persistence)
    storage()->restore(order.release());

    // Verify by locating - the order should now be in storage
    OrderEntry* found = storage()->locateByOrderId(savedOrderId);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(NEW_ORDSTATUS, found->status_);
    EXPECT_DOUBLE_EQ(99.99, found->price_);
}

// =============================================================================
// Execution Save Tests
// =============================================================================

TEST_F(OrderStorageTest, SaveExecutionAssignsId) {
    auto order = createCorrectOrder();
    OrderEntry* savedOrder = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedOrder);

    ExecutionEntry exec;
    exec.orderId_ = savedOrder->orderId_;
    exec.type_ = NEW_EXECTYPE;

    ExecutionEntry* savedExec = storage()->save(exec, IdTGenerator::instance());

    ASSERT_NE(nullptr, savedExec);
    EXPECT_TRUE(savedExec->execId_.isValid());
}

TEST_F(OrderStorageTest, SaveAndLocateExecution) {
    auto order = createCorrectOrder();
    OrderEntry* savedOrder = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedOrder);

    // Use the reference version which assigns the ID
    ExecutionEntry exec;
    exec.orderId_ = savedOrder->orderId_;
    exec.type_ = NEW_EXECTYPE;

    ExecutionEntry* savedExec = storage()->save(exec, IdTGenerator::instance());

    ASSERT_NE(nullptr, savedExec);
    EXPECT_TRUE(savedExec->execId_.isValid());

    // Verify by locating
    ExecutionEntry* found = storage()->locateByExecId(savedExec->execId_);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(savedExec->execId_, found->execId_);
}

// =============================================================================
// Execution Lookup Tests
// =============================================================================

TEST_F(OrderStorageTest, LocateByExecIdFindsExecution) {
    auto order = createCorrectOrder();
    OrderEntry* savedOrder = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedOrder);

    ExecutionEntry exec;
    exec.orderId_ = savedOrder->orderId_;
    exec.type_ = NEW_EXECTYPE;

    ExecutionEntry* savedExec = storage()->save(exec, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedExec);

    ExecutionEntry* found = storage()->locateByExecId(savedExec->execId_);

    ASSERT_NE(nullptr, found);
    EXPECT_EQ(savedExec->execId_, found->execId_);
}

TEST_F(OrderStorageTest, LocateByExecIdReturnsNullForMissing) {
    IdT nonExistentId(8888, 8888);

    ExecutionEntry* found = storage()->locateByExecId(nonExistentId);

    EXPECT_EQ(nullptr, found);
}

// =============================================================================
// Concurrent Order Save Tests
// =============================================================================

TEST_F(OrderStorageTest, ConcurrentOrderSaves) {
    const int numThreads = 4;
    const int numOrdersPerThread = 50;
    std::atomic<int> totalSaved{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &totalSaved, numOrdersPerThread]() {
            for (int i = 0; i < numOrdersPerThread; ++i) {
                auto order = createCorrectOrder();
                assignClOrderId(order.get());

                OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
                EXPECT_NE(nullptr, saved);
                ++totalSaved;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numOrdersPerThread, totalSaved.load());
}

// =============================================================================
// Concurrent Order Lookup Tests
// =============================================================================

TEST_F(OrderStorageTest, ConcurrentOrderLookups) {
    // Save some orders first
    std::vector<IdT> orderIds;
    for (int i = 0; i < 20; ++i) {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());
        OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
        ASSERT_NE(nullptr, saved);
        orderIds.push_back(saved->orderId_);
    }

    // Concurrent lookups
    const int numThreads = 8;
    const int numLookupsPerThread = 100;
    std::atomic<int> totalLookups{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &orderIds, &totalLookups, numLookupsPerThread]() {
            for (int i = 0; i < numLookupsPerThread; ++i) {
                IdT id = orderIds[i % orderIds.size()];
                OrderEntry* found = storage()->locateByOrderId(id);
                EXPECT_NE(nullptr, found);
                ++totalLookups;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numLookupsPerThread, totalLookups.load());
}

// =============================================================================
// Concurrent Execution Save/Lookup Tests
// =============================================================================

TEST_F(OrderStorageTest, ConcurrentExecutionOperations) {
    // Create orders first
    std::vector<IdT> orderIds;
    for (int i = 0; i < 10; ++i) {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());
        OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
        ASSERT_NE(nullptr, saved);
        orderIds.push_back(saved->orderId_);
    }

    const int numThreads = 4;
    const int numExecsPerThread = 25;
    std::atomic<int> totalSaved{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &orderIds, &totalSaved, numExecsPerThread]() {
            for (int i = 0; i < numExecsPerThread; ++i) {
                ExecutionEntry exec;
                exec.orderId_ = orderIds[i % orderIds.size()];
                exec.type_ = TRADE_EXECTYPE;

                ExecutionEntry* saved = storage()->save(exec, IdTGenerator::instance());
                EXPECT_NE(nullptr, saved);
                ++totalSaved;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numExecsPerThread, totalSaved.load());
}

// =============================================================================
// Mixed Concurrent Operations Tests
// =============================================================================

TEST_F(OrderStorageTest, MixedConcurrentWritesAndReads) {
    // Initial orders
    std::vector<IdT> orderIds;
    for (int i = 0; i < 10; ++i) {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());
        OrderEntry* saved = storage()->save(*order, IdTGenerator::instance());
        orderIds.push_back(saved->orderId_);
    }

    const int numWriters = 2;
    const int numReaders = 4;
    const int numOps = 50;

    std::atomic<int> totalWrites{0};
    std::atomic<int> totalReads{0};

    std::vector<std::thread> threads;

    // Writers
    for (int t = 0; t < numWriters; ++t) {
        threads.emplace_back([this, &totalWrites, numOps]() {
            for (int i = 0; i < numOps; ++i) {
                auto order = createCorrectOrder();
                assignClOrderId(order.get());
                storage()->save(*order, IdTGenerator::instance());
                ++totalWrites;
            }
        });
    }

    // Readers
    for (int t = 0; t < numReaders; ++t) {
        threads.emplace_back([this, &orderIds, &totalReads, numOps]() {
            for (int i = 0; i < numOps; ++i) {
                IdT id = orderIds[i % orderIds.size()];
                storage()->locateByOrderId(id);
                ++totalReads;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numWriters * numOps, totalWrites.load());
    EXPECT_EQ(numReaders * numOps, totalReads.load());
}

} // namespace
