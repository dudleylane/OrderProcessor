/**
 Concurrent Order Processor library - New Test File

 Authors: dudleylane, Claude
 Test Implementation: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>

#include "TestFixtures.h"
#include "TestAux.h"
#include "OrderStorage.h"
#include "QueuesDef.h"

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
// A report kept past its push keeps its fields (#56, #708)
// =============================================================================

/// Sets every ExecParams field, so a test can check that the kept copy has all of them
void fillReport(ExecutionEntry &report, ExecType type, OrderStatus status)
{
    report.execId_ = IdT(7, 1);
    report.type_ = type;
    report.transactTime_ = 12345;
    report.orderId_ = IdT(42, 1);
    report.orderStatus_ = status;
    report.market_ = "XNAS";
}

/// The copy of its execution that a report kept past its push holds: the report's own ends with the push (#708). Until
/// #708 the store saved a copy of each report, which its clients read.
std::shared_ptr<const ExecutionEntry> keptCopyOf(ExecutionEntry &report)
{
    return Queues::KeptExecReport(Queues::ExecReportEvent(&report)).exec_;
}

void expectReport(const ExecutionEntry *saved, ExecType type, OrderStatus status)
{
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(IdT(7, 1), saved->execId_);
    EXPECT_EQ(type, saved->type_);
    EXPECT_EQ(12345u, saved->transactTime_);
    EXPECT_EQ(IdT(42, 1), saved->orderId_);
    EXPECT_EQ(status, saved->orderStatus_);
    EXPECT_EQ("XNAS", saved->market_);
}

TEST_F(OrderStorageTest, KeptReportKeepsItsFields)
{
    // The bug this covers: save() stored a default-constructed copy of every report except a trade, so the report
    // pushed to clients had no type, status, order id, time or market (#56). A kept report's copy is the same clone.
    ExecutionEntry report;
    fillReport(report, NEW_EXECTYPE, NEW_ORDSTATUS);

    expectReport(keptCopyOf(report).get(), NEW_EXECTYPE, NEW_ORDSTATUS);
}

TEST_F(OrderStorageTest, KeptRejectReportKeepsItsReason)
{
    RejectExecEntry report;
    fillReport(report, REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    report.rejectReason_ = "Invalid value of the side!";

    std::shared_ptr<const ExecutionEntry> kept = keptCopyOf(report);
    const ExecutionEntry *saved = kept.get();
    expectReport(saved, REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    auto *reject = dynamic_cast<const RejectExecEntry *>(saved);
    ASSERT_NE(nullptr, reject);
    EXPECT_EQ("Invalid value of the side!", reject->rejectReason_);
}

TEST_F(OrderStorageTest, KeptReplaceReportKeepsTheOriginalOrder)
{
    ReplaceExecEntry report;
    fillReport(report, REPLACE_EXECTYPE, REPLACED_ORDSTATUS);
    report.origOrderId_ = IdT(77, 1);

    std::shared_ptr<const ExecutionEntry> kept = keptCopyOf(report);
    const ExecutionEntry *saved = kept.get();
    expectReport(saved, REPLACE_EXECTYPE, REPLACED_ORDSTATUS);
    auto *replace = dynamic_cast<const ReplaceExecEntry *>(saved);
    ASSERT_NE(nullptr, replace);
    EXPECT_EQ(IdT(77, 1), replace->origOrderId_);
}

TEST_F(OrderStorageTest, KeptCorrectReportKeepsItsFields)
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

    std::shared_ptr<const ExecutionEntry> kept = keptCopyOf(report);
    const ExecutionEntry *saved = kept.get();
    expectReport(saved, CORRECT_EXECTYPE, PARTFILL_ORDSTATUS);
    auto *correct = dynamic_cast<const ExecCorrectExecEntry *>(saved);
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

TEST_F(OrderStorageTest, KeptTradeCancelReportKeepsItsReference)
{
    TradeCancelExecEntry report;
    fillReport(report, CANCEL_EXECTYPE, NEW_ORDSTATUS);
    report.execRefId_ = IdT(9, 1);

    std::shared_ptr<const ExecutionEntry> kept = keptCopyOf(report);
    const ExecutionEntry *saved = kept.get();
    expectReport(saved, CANCEL_EXECTYPE, NEW_ORDSTATUS);
    auto *cancel = dynamic_cast<const TradeCancelExecEntry *>(saved);
    ASSERT_NE(nullptr, cancel);
    EXPECT_EQ(IdT(9, 1), cancel->execRefId_);
}

TEST_F(OrderStorageTest, KeptTradeReportKeepsItsMarket)
{
    // Trades were always copied, but the ExecParams copy constructor dropped market_ (#56)
    TradeExecEntry report;
    fillReport(report, TRADE_EXECTYPE, FILLED_ORDSTATUS);
    report.lastQty_ = 40;
    report.lastPx_ = 10.5;
    report.currency_ = USD_CURRENCY;
    report.tradeDate_ = 20261002;

    std::shared_ptr<const ExecutionEntry> kept = keptCopyOf(report);
    const ExecutionEntry *saved = kept.get();
    expectReport(saved, TRADE_EXECTYPE, FILLED_ORDSTATUS);
    auto *trade = dynamic_cast<const TradeExecEntry *>(saved);
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

TEST_F(OrderStorageTest, AClOrdIdInUseRefusesTheOrderWithoutAnException)
{
    // A new order's receive stores through this, so that a client's duplicate ClOrdID is refused without the exceptions
    // save() raised, for the store and again for the reject's own store (#605)
    auto order = createCorrectOrder();
    OrderEntry *first = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, first);

    COP::Store::PublishGuard guard;
    const char *refusal = nullptr;
    OrderEntry *second = nullptr;
    EXPECT_NO_THROW(second = storage()->saveUnlessClOrdIdInUse(*order, IdTGenerator::instance(), &guard, &refusal));
    EXPECT_EQ(nullptr, second);
    ASSERT_NE(nullptr, refusal);
    EXPECT_STREQ(COP::Store::OrderDataStorage::CLORDID_IN_USE, refusal);
    EXPECT_FALSE(guard.holds()) << "refused before the clone is made, so nothing is locked";
    EXPECT_EQ(first, storage()->locateByClOrderId(order->clOrderId_.get())) << "the stored order is untouched";

    // save() still throws, with the same reason, for its other callers
    try
    {
        storage()->save(*order, IdTGenerator::instance());
        ADD_FAILURE() << "save() stored a second order with the same ClOrdID";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_STREQ(COP::Store::OrderDataStorage::CLORDID_IN_USE, ex.what());
    }

    // and a new ClOrdID is stored as save() stores it, the reason left alone
    auto other = createCorrectOrder();
    assignClOrderId(other.get());
    refusal = nullptr;
    OrderEntry *stored = storage()->saveUnlessClOrdIdInUse(*other, IdTGenerator::instance(), nullptr, &refusal);
    ASSERT_NE(nullptr, stored);
    EXPECT_EQ(nullptr, refusal);
    EXPECT_TRUE(stored->orderId_.isValid());
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

// =============================================================================
// Orders a running server drops (#722)
// =============================================================================

TEST_F(OrderStorageTest, ADroppedOrderIsFoundByNoLookupAndKeptUntilALaterDaysFree)
{
    // A running server drops each closed order of a day before the kept ones (#722): no lookup finds it, forEachOrder()
    // passes it by, and its entry stays allocated until a later business day's freeDropped(), for a reader that took
    // its pointer before
    auto keptOrder = createCorrectOrder();
    assignClOrderId(keptOrder.get());
    OrderEntry *kept = storage()->save(*keptOrder, IdTGenerator::instance());
    auto droppedOrder = createCorrectOrder();
    assignClOrderId(droppedOrder.get());
    OrderEntry *dropped = storage()->save(*droppedOrder, IdTGenerator::instance());
    ASSERT_NE(nullptr, kept);
    ASSERT_NE(nullptr, dropped);
    const RawDataEntry clOrdId = dropped->clOrderId_.get();
    EXPECT_EQ(0u, storage()->droppedMaxOrderNumber());

    const DateTimeT day = 1792011600000ULL; // 2026-10-14 17:00 New York, 21:00 UTC
    ASSERT_TRUE(storage()->drop(dropped, day));
    EXPECT_EQ(nullptr, storage()->locateByOrderId(dropped->orderId_));
    EXPECT_EQ(nullptr, storage()->locateByClOrderId(clOrdId));
    EXPECT_EQ(nullptr, storage()->locateByOrderNumber(dropped->orderId_.id_));
    EXPECT_EQ(1u, storage()->orderCount());
    std::vector<const OrderEntry *> walked;
    storage()->forEachOrder(
        [&walked](const IdT &, const OrderEntry &order)
        {
            walked.push_back(&order);
        });
    EXPECT_EQ(std::vector<const OrderEntry *>{ kept }, walked);
    EXPECT_EQ(1u, storage()->droppedHeld());
    EXPECT_EQ(dropped->orderId_.id_, storage()->droppedMaxOrderNumber());
    EXPECT_EQ(std::string(clOrdId.data_, clOrdId.length_),
              std::string(dropped->clOrderId_.get().data_, dropped->clOrderId_.get().length_))
        << "the dropped order can't be read";
    EXPECT_FALSE(storage()->drop(dropped, day)) << "an order was dropped twice";

    EXPECT_EQ(0u, storage()->freeDropped(day)) << "an order was freed on the day it was dropped";
    EXPECT_EQ(1u, storage()->droppedHeld());
    EXPECT_EQ(1u, storage()->freeDropped(day + 1));
    EXPECT_EQ(0u, storage()->droppedHeld());
    EXPECT_EQ(0u, storage()->freeDropped(day + 1));
    EXPECT_EQ(kept, storage()->locateByOrderId(kept->orderId_));

    // One dropped and not yet freed goes with the store, as the orders it holds do: LeakSanitizer sees one left
    ASSERT_TRUE(storage()->drop(kept, day + 1));
    EXPECT_EQ(0u, storage()->orderCount());
    EXPECT_EQ(1u, storage()->droppedHeld());
}

TEST_F(OrderStorageTest, AnOrderPutBackAfterItsDropIsFoundAgain)
{
    // A drop whose record can't be erased rolls back, and puts the order back (#722)
    auto order = createCorrectOrder();
    assignClOrderId(order.get());
    OrderEntry *saved = storage()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    const RawDataEntry clOrdId = saved->clOrderId_.get();
    EXPECT_FALSE(storage()->undrop(saved)) << "an order the store holds was put back";

    const DateTimeT day = 1792011600000ULL; // 2026-10-14 17:00 New York, 21:00 UTC
    ASSERT_TRUE(storage()->drop(saved, day));
    ASSERT_TRUE(storage()->undrop(saved));
    EXPECT_EQ(saved, storage()->locateByOrderId(saved->orderId_));
    EXPECT_EQ(saved, storage()->locateByClOrderId(clOrdId));
    EXPECT_EQ(1u, storage()->orderCount());
    EXPECT_EQ(0u, storage()->droppedHeld());
    EXPECT_FALSE(storage()->undrop(saved)) << "an order was put back twice";
    EXPECT_EQ(0u, storage()->freeDropped(day + 1)) << "an order put back was freed";
    EXPECT_EQ(saved, storage()->locateByOrderId(saved->orderId_));
}
