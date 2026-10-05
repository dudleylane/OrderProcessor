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
#include <atomic>

#include "TestFixtures.h"
#include "TestAux.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::Store;
using namespace test;

namespace
{

// =============================================================================
// Test Fixture
// =============================================================================

class OrderStorageTest : public OrderStorageFixture
{
protected:
    void SetUp() override
    {
        OrderStorageFixture::SetUp();
    }

    void TearDown() override
    {
        OrderStorageFixture::TearDown();
    }

    OrderDataStorage *storage()
    {
        return OrderStorage::instance();
    }
};

// =============================================================================
// Basic Singleton Tests
// =============================================================================

TEST_F(OrderStorageTest, InstanceNotNull)
{
    ASSERT_NE(nullptr, storage());
}

TEST_F(OrderStorageTest, InstanceReturnsSamePointer)
{
    auto *ptr1 = storage();
    auto *ptr2 = storage();
    EXPECT_EQ(ptr1, ptr2);
}

// =============================================================================
// Order Save Tests
// =============================================================================

TEST_F(OrderStorageTest, SaveOrderAssignsId)
{
    auto order = createCorrectOrder();
    ASSERT_FALSE(order->orderId_.isValid());

    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());

    ASSERT_NE(nullptr, saved);
    EXPECT_TRUE(saved->orderId_.isValid());
}

TEST_F(OrderStorageTest, SaveMultipleOrdersAssignsUniqueIds)
{
    std::vector<IdT> orderIds;

    for (int i = 0; i < 10; ++i)
    {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());

        OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
        ASSERT_NE(nullptr, saved);

        // Check uniqueness
        for (const auto &existingId : orderIds)
        {
            EXPECT_NE(existingId, saved->orderId_);
        }
        orderIds.push_back(saved->orderId_);
    }
}

// =============================================================================
// Order Lookup by OrderId Tests
// =============================================================================

TEST_F(OrderStorageTest, LocateByOrderIdFindsOrder)
{
    auto order = createCorrectOrder();
    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);

    OrderEntry *found = storage()->locateByOrderId(saved->orderId_);

    ASSERT_NE(nullptr, found);
    EXPECT_EQ(saved->orderId_, found->orderId_);
}

TEST_F(OrderStorageTest, LocateByOrderIdReturnsNullForMissing)
{
    IdT nonExistentId(9999, 9999);

    OrderEntry *found = storage()->locateByOrderId(nonExistentId);

    EXPECT_EQ(nullptr, found);
}

// =============================================================================
// Order Lookup by ClOrderId Tests
// =============================================================================

TEST_F(OrderStorageTest, LocateByClOrderIdFindsOrder)
{
    auto order = createCorrectOrder();
    assignClOrderId(order.get());
    RawDataEntry clOrdId = order->clOrderId_.get();

    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);

    OrderEntry *found = storage()->locateByClOrderId(clOrdId);

    ASSERT_NE(nullptr, found);
    EXPECT_EQ(saved->orderId_, found->orderId_);
}

TEST_F(OrderStorageTest, LocateByClOrderIdReturnsNullForMissing)
{
    RawDataEntry nonExistentClId(STRING_RAWDATATYPE, "NonExistent", 11);

    OrderEntry *found = storage()->locateByClOrderId(nonExistentClId);

    EXPECT_EQ(nullptr, found);
}

// =============================================================================
// Order Restore Tests
// =============================================================================

TEST_F(OrderStorageTest, RestoreLoadsOrder)
{
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
    OrderEntry *found = storage()->locateByOrderId(savedOrderId);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(NEW_ORDSTATUS, found->status_);
    EXPECT_DOUBLE_EQ(99.99, found->price_);
}

// =============================================================================
// Order Numbers (#58)
// =============================================================================

/// Restores an order with this id and a unique ClOrdID, as a load from persistence does
OrderEntry *restoreOrder(const IdT &id)
{
    auto order = test::createCorrectOrder();
    test::assignClOrderId(order.get());
    order->orderId_ = id;
    OrderEntry *raw = order.release();
    OrderStorage::instance()->restore(raw);
    return raw;
}

TEST_F(OrderStorageTest, LocateByOrderNumberFindsTheOrderWithIt)
{
    restoreOrder(IdT(3, 1790000000));
    OrderEntry *seven = restoreOrder(IdT(7, 1790000100));
    restoreOrder(IdT(9, 1790000200));

    bool ambiguous = true;
    EXPECT_EQ(seven, storage()->locateByOrderNumber(7, &ambiguous));
    EXPECT_FALSE(ambiguous);
}

TEST_F(OrderStorageTest, LocateByOrderNumberReturnsNullForAMissingNumber)
{
    restoreOrder(IdT(7, 1790000100));

    bool ambiguous = true;
    EXPECT_EQ(nullptr, storage()->locateByOrderNumber(8, &ambiguous));
    EXPECT_FALSE(ambiguous);
    EXPECT_EQ(nullptr, storage()->locateByOrderNumber(6));
}

TEST_F(OrderStorageTest, LocateByOrderNumberRefusesANumberTwoOrdersShare)
{
    // A data directory written before #58 can hold two orders with one number, from different runs
    restoreOrder(IdT(7, 1790000100));
    restoreOrder(IdT(7, 1790009999));

    bool ambiguous = false;
    EXPECT_EQ(nullptr, storage()->locateByOrderNumber(7, &ambiguous));
    EXPECT_TRUE(ambiguous);
}

TEST_F(OrderStorageTest, MaxOrderNumberIsTheLargestStored)
{
    EXPECT_EQ(0u, storage()->maxOrderNumber());
    restoreOrder(IdT(12, 1790000000));
    restoreOrder(IdT(4, 1790009999)); // newer, but with a smaller number
    EXPECT_EQ(12u, storage()->maxOrderNumber());
}

// =============================================================================
// Execution Save Tests
// =============================================================================

TEST_F(OrderStorageTest, SaveExecutionAssignsId)
{
    auto order = createCorrectOrder();
    OrderEntry *savedOrder = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedOrder);

    ExecutionEntry exec;
    exec.orderId_ = savedOrder->orderId_;
    exec.type_ = NEW_EXECTYPE;

    ExecutionEntry *savedExec = storage()->save(exec, IdTGenerator::instance());

    ASSERT_NE(nullptr, savedExec);
    EXPECT_TRUE(savedExec->execId_.isValid());
}

TEST_F(OrderStorageTest, SaveAndLocateExecution)
{
    auto order = createCorrectOrder();
    OrderEntry *savedOrder = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedOrder);

    // Use the reference version which assigns the ID
    ExecutionEntry exec;
    exec.orderId_ = savedOrder->orderId_;
    exec.type_ = NEW_EXECTYPE;

    ExecutionEntry *savedExec = storage()->save(exec, IdTGenerator::instance());

    ASSERT_NE(nullptr, savedExec);
    EXPECT_TRUE(savedExec->execId_.isValid());

    // Verify by locating
    ExecutionEntry *found = storage()->locateByExecId(savedExec->execId_);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(savedExec->execId_, found->execId_);
}

// =============================================================================
// Execution Lookup Tests
// =============================================================================

TEST_F(OrderStorageTest, LocateByExecIdFindsExecution)
{
    auto order = createCorrectOrder();
    OrderEntry *savedOrder = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedOrder);

    ExecutionEntry exec;
    exec.orderId_ = savedOrder->orderId_;
    exec.type_ = NEW_EXECTYPE;

    ExecutionEntry *savedExec = storage()->save(exec, IdTGenerator::instance());
    ASSERT_NE(nullptr, savedExec);

    ExecutionEntry *found = storage()->locateByExecId(savedExec->execId_);

    ASSERT_NE(nullptr, found);
    EXPECT_EQ(savedExec->execId_, found->execId_);
}

TEST_F(OrderStorageTest, LocateByExecIdReturnsNullForMissing)
{
    IdT nonExistentId(8888, 8888);

    ExecutionEntry *found = storage()->locateByExecId(nonExistentId);

    EXPECT_EQ(nullptr, found);
}

// =============================================================================
// Concurrent Order Save Tests
// =============================================================================

TEST_F(OrderStorageTest, ConcurrentOrderSaves)
{
    const int numThreads = 4;
    const int numOrdersPerThread = 50;
    std::atomic<int> totalSaved{ 0 };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back(
            [this, &totalSaved, numOrdersPerThread]()
            {
                for (int i = 0; i < numOrdersPerThread; ++i)
                {
                    auto order = createCorrectOrder();
                    assignClOrderId(order.get());

                    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
                    EXPECT_NE(nullptr, saved);
                    ++totalSaved;
                }
            });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(numThreads * numOrdersPerThread, totalSaved.load());
}

// =============================================================================
// Concurrent Order Lookup Tests
// =============================================================================

TEST_F(OrderStorageTest, ConcurrentOrderLookups)
{
    // Save some orders first
    std::vector<IdT> orderIds;
    for (int i = 0; i < 20; ++i)
    {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());
        OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
        ASSERT_NE(nullptr, saved);
        orderIds.push_back(saved->orderId_);
    }

    // Concurrent lookups
    const int numThreads = 8;
    const int numLookupsPerThread = 100;
    std::atomic<int> totalLookups{ 0 };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back(
            [this, &orderIds, &totalLookups, numLookupsPerThread]()
            {
                for (int i = 0; i < numLookupsPerThread; ++i)
                {
                    IdT id = orderIds[i % orderIds.size()];
                    OrderEntry *found = storage()->locateByOrderId(id);
                    EXPECT_NE(nullptr, found);
                    ++totalLookups;
                }
            });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(numThreads * numLookupsPerThread, totalLookups.load());
}

// =============================================================================
// Concurrent Execution Save/Lookup Tests
// =============================================================================

TEST_F(OrderStorageTest, ConcurrentExecutionOperations)
{
    // Create orders first
    std::vector<IdT> orderIds;
    for (int i = 0; i < 10; ++i)
    {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());
        OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
        ASSERT_NE(nullptr, saved);
        orderIds.push_back(saved->orderId_);
    }

    const int numThreads = 4;
    const int numExecsPerThread = 25;
    std::atomic<int> totalSaved{ 0 };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back(
            [this, &orderIds, &totalSaved, numExecsPerThread]()
            {
                for (int i = 0; i < numExecsPerThread; ++i)
                {
                    ExecutionEntry exec;
                    exec.orderId_ = orderIds[i % orderIds.size()];
                    exec.type_ = TRADE_EXECTYPE;

                    ExecutionEntry *saved = storage()->save(exec, IdTGenerator::instance());
                    EXPECT_NE(nullptr, saved);
                    ++totalSaved;
                }
            });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(numThreads * numExecsPerThread, totalSaved.load());
}

// =============================================================================
// Mixed Concurrent Operations Tests
// =============================================================================

TEST_F(OrderStorageTest, MixedConcurrentWritesAndReads)
{
    // Initial orders
    std::vector<IdT> orderIds;
    for (int i = 0; i < 10; ++i)
    {
        auto order = createCorrectOrder();
        assignClOrderId(order.get());
        OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
        orderIds.push_back(saved->orderId_);
    }

    const int numWriters = 2;
    const int numReaders = 4;
    const int numOps = 50;

    std::atomic<int> totalWrites{ 0 };
    std::atomic<int> totalReads{ 0 };

    std::vector<std::thread> threads;

    // Writers
    for (int t = 0; t < numWriters; ++t)
    {
        threads.emplace_back(
            [this, &totalWrites, numOps]()
            {
                for (int i = 0; i < numOps; ++i)
                {
                    auto order = createCorrectOrder();
                    assignClOrderId(order.get());
                    storage()->save(*order, IdTGenerator::instance());
                    ++totalWrites;
                }
            });
    }

    // Readers
    for (int t = 0; t < numReaders; ++t)
    {
        threads.emplace_back(
            [this, &orderIds, &totalReads, numOps]()
            {
                for (int i = 0; i < numOps; ++i)
                {
                    IdT id = orderIds[i % orderIds.size()];
                    storage()->locateByOrderId(id);
                    ++totalReads;
                }
            });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(numWriters * numOps, totalWrites.load());
    EXPECT_EQ(numReaders * numOps, totalReads.load());
}

// =============================================================================
// Saved execution reports keep their fields (#56)
// =============================================================================

/// Sets every ExecParams field but the execId, so a test can check that the saved copy kept all of them
void fillReport(ExecutionEntry &report, ExecType type, OrderStatus status)
{
    report.type_ = type;
    report.transactTime_ = 12345;
    report.orderId_ = IdT(42, 1);
    report.orderStatus_ = status;
    report.market_ = "XNAS";
}

void expectReport(const ExecutionEntry *saved, ExecType type, OrderStatus status)
{
    ASSERT_NE(nullptr, saved);
    EXPECT_TRUE(saved->execId_.isValid());
    EXPECT_EQ(type, saved->type_);
    EXPECT_EQ(12345u, saved->transactTime_);
    EXPECT_EQ(IdT(42, 1), saved->orderId_);
    EXPECT_EQ(status, saved->orderStatus_);
    EXPECT_EQ("XNAS", saved->market_);
}

TEST_F(OrderStorageTest, SavedReportKeepsItsFields)
{
    // The bug this covers: save() stored a default-constructed copy of every report except a trade, so the report
    // pushed to clients had no type, status, order id, time or market (#56).
    ExecutionEntry report;
    fillReport(report, NEW_EXECTYPE, NEW_ORDSTATUS);

    expectReport(storage()->save(report, IdTGenerator::instance()), NEW_EXECTYPE, NEW_ORDSTATUS);
}

TEST_F(OrderStorageTest, SavedRejectReportKeepsItsReason)
{
    RejectExecEntry report;
    fillReport(report, REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    report.rejectReason_ = "Invalid value of the side!";

    ExecutionEntry *saved = storage()->save(report, IdTGenerator::instance());
    expectReport(saved, REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    auto *reject = dynamic_cast<RejectExecEntry *>(saved);
    ASSERT_NE(nullptr, reject);
    EXPECT_EQ("Invalid value of the side!", reject->rejectReason_);
}

TEST_F(OrderStorageTest, SavedReplaceReportKeepsTheOriginalOrder)
{
    ReplaceExecEntry report;
    fillReport(report, REPLACE_EXECTYPE, REPLACED_ORDSTATUS);
    report.origOrderId_ = IdT(77, 1);

    ExecutionEntry *saved = storage()->save(report, IdTGenerator::instance());
    expectReport(saved, REPLACE_EXECTYPE, REPLACED_ORDSTATUS);
    auto *replace = dynamic_cast<ReplaceExecEntry *>(saved);
    ASSERT_NE(nullptr, replace);
    EXPECT_EQ(IdT(77, 1), replace->origOrderId_);
}

TEST_F(OrderStorageTest, SavedCorrectReportKeepsItsFields)
{
    ExecCorrectExecEntry report;
    fillReport(report, CORRECT_EXECTYPE, PARTFILL_ORDSTATUS);
    report.cumQty_ = 40;
    report.leavesQty_ = 60;
    report.lastQty_ = 40;
    report.lastPx_ = 10.5;
    report.currency_ = USD_CURRENCY;
    report.tradeDate_ = 20261002;
    report.origOrderId_ = IdT(5, 1);
    report.execRefId_ = IdT(9, 1);

    ExecutionEntry *saved = storage()->save(report, IdTGenerator::instance());
    expectReport(saved, CORRECT_EXECTYPE, PARTFILL_ORDSTATUS);
    auto *correct = dynamic_cast<ExecCorrectExecEntry *>(saved);
    ASSERT_NE(nullptr, correct);
    EXPECT_EQ(40u, correct->cumQty_);
    EXPECT_EQ(60u, correct->leavesQty_);
    EXPECT_EQ(40u, correct->lastQty_);
    EXPECT_DOUBLE_EQ(10.5, correct->lastPx_);
    EXPECT_EQ(USD_CURRENCY, correct->currency_);
    EXPECT_EQ(20261002u, correct->tradeDate_);
    EXPECT_EQ(IdT(5, 1), correct->origOrderId_);
    EXPECT_EQ(IdT(9, 1), correct->execRefId_);
}

TEST_F(OrderStorageTest, SavedTradeCancelReportKeepsItsReference)
{
    TradeCancelExecEntry report;
    fillReport(report, CANCEL_EXECTYPE, NEW_ORDSTATUS);
    report.execRefId_ = IdT(9, 1);

    ExecutionEntry *saved = storage()->save(report, IdTGenerator::instance());
    expectReport(saved, CANCEL_EXECTYPE, NEW_ORDSTATUS);
    auto *cancel = dynamic_cast<TradeCancelExecEntry *>(saved);
    ASSERT_NE(nullptr, cancel);
    EXPECT_EQ(IdT(9, 1), cancel->execRefId_);
}

TEST_F(OrderStorageTest, SavedTradeReportKeepsItsMarket)
{
    // Trades were always copied, but the ExecParams copy constructor dropped market_ (#56)
    TradeExecEntry report;
    fillReport(report, TRADE_EXECTYPE, FILLED_ORDSTATUS);
    report.lastQty_ = 40;
    report.lastPx_ = 10.5;
    report.currency_ = USD_CURRENCY;
    report.tradeDate_ = 20261002;

    ExecutionEntry *saved = storage()->save(report, IdTGenerator::instance());
    expectReport(saved, TRADE_EXECTYPE, FILLED_ORDSTATUS);
    auto *trade = dynamic_cast<TradeExecEntry *>(saved);
    ASSERT_NE(nullptr, trade);
    EXPECT_EQ(40u, trade->lastQty_);
    EXPECT_DOUBLE_EQ(10.5, trade->lastPx_);
    EXPECT_EQ(USD_CURRENCY, trade->currency_);
    EXPECT_EQ(20261002u, trade->tradeDate_);
}

} // namespace

// =============================================================================
// Publication lock (#13): a new order stays locked until its creator releases it
// =============================================================================

TEST_F(OrderStorageTest, SaveWithPublishGuardLocksNewOrderUntilReleased)
{
    auto order = createCorrectOrder();
    COP::Store::PublishGuard guard;

    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance(), &guard);
    ASSERT_NE(nullptr, saved);
    EXPECT_TRUE(guard.holds());
    // Another thread that located the order could not read it yet
    EXPECT_FALSE(saved->entryMutex_.try_lock_shared());

    guard.release();
    EXPECT_FALSE(guard.holds());
    ASSERT_TRUE(saved->entryMutex_.try_lock());
    saved->entryMutex_.unlock();
}

TEST_F(OrderStorageTest, SaveWithoutPublishGuardLeavesOrderUnlocked)
{
    auto order = createCorrectOrder();
    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    ASSERT_TRUE(saved->entryMutex_.try_lock());
    saved->entryMutex_.unlock();
}

TEST_F(OrderStorageTest, FailedSaveDoesNotLeaveAGuardHeld)
{
    auto order = createCorrectOrder();
    storage()->save(*order, IdTGenerator::instance());

    COP::Store::PublishGuard guard;
    // Same ClOrderId: rejected before the clone is created, so nothing is locked
    EXPECT_THROW(storage()->save(*order, IdTGenerator::instance(), &guard), std::runtime_error);
    EXPECT_FALSE(guard.holds());
}

// =============================================================================
// Booking mark (#83): a new order stays marked until its creator has enqueued the transaction that books it
// =============================================================================

TEST_F(OrderStorageTest, SaveWithPublishGuardMarksNewOrderUntilItsBookingIsEnqueued)
{
    auto order = createCorrectOrder();
    COP::Store::PublishGuard guard;

    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance(), &guard);
    ASSERT_NE(nullptr, saved);
    EXPECT_TRUE(saved->bookingPending_.load());
    // A copy is not the stored order, so nothing books it
    std::unique_ptr<OrderEntry> copy(saved->clone());
    EXPECT_FALSE(copy->bookingPending_.load());

    // Initialised, the order is unlocked but stays marked
    guard.release();
    EXPECT_TRUE(saved->bookingPending_.load());

    guard.bookingEnqueued();
    EXPECT_FALSE(saved->bookingPending_.load());
}

TEST_F(OrderStorageTest, DestroyedPublishGuardLeavesNoOrderMarked)
{
    // A creator that throws before it enqueues must not leave a cancel of the order waiting for ever
    auto order = createCorrectOrder();
    OrderEntry *saved = nullptr;
    {
        COP::Store::PublishGuard guard;
        saved = storage()->save(*order, IdTGenerator::instance(), &guard);
        ASSERT_NE(nullptr, saved);
    }
    EXPECT_FALSE(saved->bookingPending_.load());
    ASSERT_TRUE(saved->entryMutex_.try_lock());
    saved->entryMutex_.unlock();
}

TEST_F(OrderStorageTest, SaveWithoutPublishGuardLeavesOrderUnmarked)
{
    // A restored order has no transaction to wait for
    auto order = createCorrectOrder();
    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    EXPECT_FALSE(saved->bookingPending_.load());
}
