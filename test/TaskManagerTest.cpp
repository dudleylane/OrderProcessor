/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 Migrated from testTaskManager.cpp
*/

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"

#include "TaskManager.h"
#include "TransactionMgr.h"
#include "Processor.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"

using namespace COP;
using namespace COP::Tasks;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace test;

namespace
{

// =============================================================================
// Test Output Queue - Captures outgoing events
// =============================================================================

class TestOutQueues : public OutQueues
{
public:
    TestOutQueues() : execReportCount_(0), cancelRejectCount_(0), businessRejectCount_(0) {}

    /// What one execution report said when it was pushed; the report itself belongs to OrderStorage
    struct RecordedReport
    {
        ExecType type;
        OrderStatus status;
        IdT orderId;
        DateTimeT transactTime;
        std::string market;
        std::string rejectReason; // set for a RejectExecEntry only
    };

    void push(const ExecReportEvent &evnt, const std::string &) override
    {
        RecordedReport report{ evnt.exec_->type_,         evnt.exec_->orderStatus_, evnt.exec_->orderId_,
                               evnt.exec_->transactTime_, evnt.exec_->market_,      std::string() };
        if (auto *reject = dynamic_cast<const RejectExecEntry *>(evnt.exec_))
        {
            report.rejectReason = reject->rejectReason_;
        }
        {
            std::lock_guard<std::mutex> lock(reportsLock_);
            reports_.push_back(report);
        }
        ++execReportCount_;
    }

    void push(const OrderRejectEvent &evnt, const std::string &target) override
    {
        std::lock_guard<std::mutex> lock(reportsLock_);
        orderRejects_.emplace_back(evnt, target);
    }

    /// The order rejects pushed so far, each with its target
    std::vector<std::pair<OrderRejectEvent, std::string>> orderRejects() const
    {
        std::lock_guard<std::mutex> lock(reportsLock_);
        return orderRejects_;
    }

    /// The first recorded report of the given type, if any
    std::optional<RecordedReport> report(ExecType type) const
    {
        std::lock_guard<std::mutex> lock(reportsLock_);
        for (const auto &r : reports_)
        {
            if (type == r.type)
            {
                return r;
            }
        }
        return std::nullopt;
    }

    void push(const CancelRejectEvent &evnt, const std::string &target) override
    {
        lastCancelRejectOrderId_ = evnt.id_.id_;
        {
            std::lock_guard<std::mutex> lock(reportsLock_);
            cancelRejects_.emplace_back(evnt, target);
        }
        ++cancelRejectCount_;
    }

    /// The cancel rejects pushed so far, each with its target
    std::vector<std::pair<CancelRejectEvent, std::string>> cancelRejects() const
    {
        std::lock_guard<std::mutex> lock(reportsLock_);
        return cancelRejects_;
    }

    /// The types of the reports recorded for the order, in the order they were pushed
    std::vector<ExecType> reportTypes(const IdT &orderId) const
    {
        std::lock_guard<std::mutex> lock(reportsLock_);
        std::vector<ExecType> types;
        for (const auto &r : reports_)
        {
            if (orderId == r.orderId)
            {
                types.push_back(r.type);
            }
        }
        return types;
    }

    /// How many reports of the type were recorded for the order
    int reportCount(ExecType type, const IdT &orderId) const
    {
        const auto types = reportTypes(orderId);
        return static_cast<int>(std::count(types.begin(), types.end(), type));
    }

    size_t reportCountAll() const
    {
        std::lock_guard<std::mutex> lock(reportsLock_);
        return reports_.size();
    }

    void push(const BusinessRejectEvent &, const std::string &) override
    {
        ++businessRejectCount_;
    }

    int totalEvents() const
    {
        return execReportCount_.load() + cancelRejectCount_.load() + businessRejectCount_.load();
    }

    std::atomic<int> execReportCount_;
    std::atomic<int> cancelRejectCount_;
    std::atomic<int> businessRejectCount_;
    std::atomic<u64> lastCancelRejectOrderId_{ 0 };

    mutable std::mutex reportsLock_;
    std::vector<RecordedReport> reports_;
    std::vector<std::pair<OrderRejectEvent, std::string>> orderRejects_;
    std::vector<std::pair<CancelRejectEvent, std::string>> cancelRejects_;
};

/// Records each order version the storage persists
struct RecordingSaver final : public COP::OrderSaver
{
    struct Version
    {
        IdT orderId;
        OrderStatus status;
        QuantityT orderQty;
        QuantityT leavesQty;
    };
    u32 save(const OrderEntry &order) override
    {
        std::lock_guard<std::mutex> lock(lock_);
        versions_.push_back({ order.orderId_, order.status_, order.orderQty_, order.leavesQty_ });
        return static_cast<u32>(versions_.size());
    }
    void erase(const IdT &, u32) override {}
    std::vector<Version> versions()
    {
        std::lock_guard<std::mutex> lock(lock_);
        return versions_;
    }
    std::mutex lock_;
    std::vector<Version> versions_;
};

/// Throws instead of processing the first transaction it is given, and hands later ones to a real processor. It owns
/// that processor, since TaskManager deletes this one.
class ThrowOnceTransactionProcessor : public TransactionProcessor
{
public:
    explicit ThrowOnceTransactionProcessor(std::unique_ptr<Processor> processor) : processor_(std::move(processor)) {}

    void process(const TransactionId &id, Transaction *tr) override
    {
        if (!thrown_.exchange(true))
        {
            throw std::runtime_error("injected transaction failure");
        }
        processor_->process(id, tr);
    }

private:
    std::unique_ptr<Processor> processor_;
    std::atomic<bool> thrown_{ false };
};

// =============================================================================
// Task Manager Test Fixture
// =============================================================================

class TaskManagerTest : public ProcessorFixture
{
protected:
    void SetUp() override
    {
        ProcessorFixture::SetUp();

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<TestOutQueues>();

        // Initialize transaction manager
        TransactionMgrParams transParams(IdTGenerator::instance());
        transMgr_ = std::make_unique<TransactionMgr>();
        transMgr_->init(transParams);

        // Initialize processor parameters
        procParams_ =
            std::make_unique<ProcessorParams>(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(),
                                              inQueues_.get(), outQueues_.get(), inQueues_.get(), transMgr_.get());
    }

    void TearDown() override
    {
        if (transMgr_)
        {
            transMgr_->stop();
        }
        procParams_.reset();
        transMgr_.reset();
        outQueues_.reset();
        inQueues_.reset();

        ProcessorFixture::TearDown();
    }

    // Helper to create a task manager with specified number of processors
    std::unique_ptr<TaskManager> createTaskManager(int eventProcessors, int transactionProcessors)
    {
        TaskManagerParams params;
        params.transactMgr_ = transMgr_.get();
        params.inQueues_ = inQueues_.get();

        for (int i = 0; i < eventProcessors; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(*procParams_);
            params.evntProcessors_.push_back(proc.release());
        }

        for (int i = 0; i < transactionProcessors; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(*procParams_);
            params.transactProcessors_.push_back(proc.release());
        }

        return std::make_unique<TaskManager>(params);
    }

    /// Pushes a limit order from the source, and returns its ClOrdID to find it by once it is processed
    RawDataEntry pushOrder(Side side, PriceT price, QuantityT qty, const std::string &source)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        order->side_ = side;
        order->price_ = price;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;
        RawDataEntry clOrdId = order->clOrderId_.get();
        inQueues_->push(source, OrderEvent(order.release()));
        return clOrdId;
    }

    /// Polls until the pool is idle, where waitUntilTransactionsFinished(N) takes at least two seconds: it sleeps a
    /// second between its two checks. One passing check is not enough, because TaskManager hands a finished
    /// transaction's processor back before it removes the transaction from TransactionMgr, which may then release its
    /// children: for that instant the pool looks idle with work left. So the check must pass ten times in a row, 10 ms
    /// apart, which also covers a thread descheduled inside that window for up to about 90 ms.
    bool waitUntilIdle(TaskManager &manager, std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int idleChecks = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            idleChecks = manager.waitUntilTransactionsFinished(0) ? idleChecks + 1 : 0;
            if (10 == idleChecks)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

protected:
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<TestOutQueues> outQueues_;
    std::unique_ptr<TransactionMgr> transMgr_;
    std::unique_ptr<ProcessorParams> procParams_;
};

// =============================================================================
// Basic Task Manager Tests
// =============================================================================

TEST_F(TaskManagerTest, CreateWithSingleProcessor)
{
    auto manager = createTaskManager(1, 1);
    ASSERT_NE(nullptr, manager);
}

TEST_F(TaskManagerTest, CreateWithMultipleProcessors)
{
    auto manager = createTaskManager(3, 3);
    ASSERT_NE(nullptr, manager);
}

// =============================================================================
// Single Order Processing Tests
// =============================================================================

TEST_F(TaskManagerTest, ProcessSingleOrder)
{
    auto manager = createTaskManager(1, 1);

    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());

    inQueues_->push("test", OrderEvent(order.release()));

    // Wait for transaction to complete
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));

    // Should have generated at least one execution report
    EXPECT_GE(outQueues_->execReportCount_.load(), 1);
}

TEST_F(TaskManagerTest, ProcessMultipleOrders)
{
    auto manager = createTaskManager(2, 2);

    const int numOrders = 10;
    for (int i = 0; i < numOrders; ++i)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        inQueues_->push("test", OrderEvent(order.release()));
    }

    // Wait for transactions to complete
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(10));

    // Should have generated execution reports for each order
    EXPECT_GE(outQueues_->execReportCount_.load(), numOrders);
}

// =============================================================================
// Buy/Sell Matching Tests
// =============================================================================

TEST_F(TaskManagerTest, MatchBuyAndSellOrders)
{
    auto manager = createTaskManager(3, 3);

    // Create a sell order
    auto sellOrder = createCorrectOrder(instrumentId1_);
    assignClOrderId(sellOrder.get());
    sellOrder->side_ = SELL_SIDE;
    sellOrder->price_ = 10.0;
    sellOrder->leavesQty_ = 100;
    inQueues_->push("test", OrderEvent(sellOrder.release()));

    // Create a buy order that should match
    auto buyOrder = createCorrectOrder(instrumentId1_);
    assignClOrderId(buyOrder.get());
    buyOrder->side_ = BUY_SIDE;
    buyOrder->price_ = 10.0;
    buyOrder->leavesQty_ = 100;
    inQueues_->push("test", OrderEvent(buyOrder.release()));

    // Wait for transactions to complete
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(10));

    // Should have generated execution reports for both orders
    EXPECT_GE(outQueues_->execReportCount_.load(), 2);
}

// =============================================================================
// Concurrent Processing Tests
// =============================================================================

TEST_F(TaskManagerTest, ConcurrentOrderSubmission)
{
    auto manager = createTaskManager(3, 3);

    const int numOrders = 100;
    std::atomic<int> ordersSubmitted{ 0 };

    // Submit orders from multiple threads
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back(
            [this, &ordersSubmitted, numOrders]()
            {
                for (int i = 0; i < numOrders / 4; ++i)
                {
                    auto order = createCorrectOrder(instrumentId1_);
                    assignClOrderId(order.get());
                    inQueues_->push("test", OrderEvent(order.release()));
                    ++ordersSubmitted;
                }
            });
    }

    // Wait for all threads to finish submitting
    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(numOrders, ordersSubmitted.load());

    // Wait for transactions to complete
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(30));

    // Should have processed all orders
    EXPECT_GE(outQueues_->totalEvents(), numOrders);
}

// =============================================================================
// Timeout Tests
// =============================================================================

TEST_F(TaskManagerTest, WaitUntilTransactionsFinishedWithEmptyQueue)
{
    auto manager = createTaskManager(1, 1);

    // With no transactions, should return immediately
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(1));
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(TaskManagerTest, HandleHighVolume)
{
    auto manager = createTaskManager(3, 3);

    const int numOrders = 500;
    for (int i = 0; i < numOrders; ++i)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        inQueues_->push("test", OrderEvent(order.release()));
    }

    // Wait for transactions to complete with extended timeout
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(60));

    // Should have processed all orders
    EXPECT_GE(outQueues_->totalEvents(), numOrders);
}

// =============================================================================
// Mixed Order Types Tests
// =============================================================================

TEST_F(TaskManagerTest, ProcessMixedBuySellOrders)
{
    auto manager = createTaskManager(3, 3);

    const int numPairs = 50;

    for (int i = 0; i < numPairs; ++i)
    {
        // Submit a buy order
        auto buyOrder = createCorrectOrder(instrumentId1_);
        assignClOrderId(buyOrder.get());
        buyOrder->side_ = BUY_SIDE;
        buyOrder->price_ = 10.0;
        buyOrder->leavesQty_ = 100;
        inQueues_->push("test", OrderEvent(buyOrder.release()));

        // Submit a sell order
        auto sellOrder = createCorrectOrder(instrumentId1_);
        assignClOrderId(sellOrder.get());
        sellOrder->side_ = SELL_SIDE;
        sellOrder->price_ = 10.0;
        sellOrder->leavesQty_ = 100;
        inQueues_->push("test", OrderEvent(sellOrder.release()));
    }

    // Wait for transactions to complete
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(60));

    // Should have generated execution reports for all orders
    EXPECT_GE(outQueues_->totalEvents(), numPairs * 2);
}

// =============================================================================
// Execution Report Content (#56)
// =============================================================================

TEST_F(TaskManagerTest, NewOrderReportCarriesItsContent)
{
    // The bug this covers: OrderStorage kept a default-constructed copy of every report except a trade, and that copy
    // is what clients received: no type, status, order id, time or market (#56).
    auto manager = createTaskManager(1, 1);

    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));

    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    OrderEntry *saved = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, saved);
    auto ack = outQueues_->report(NEW_EXECTYPE);
    ASSERT_TRUE(ack.has_value()) << "no NEW report";
    EXPECT_EQ(NEW_ORDSTATUS, ack->status);
    EXPECT_EQ(saved->orderId_, ack->orderId);
    EXPECT_NE(0u, ack->transactTime);
    EXPECT_EQ(INTERNAL_EXECUTION, ack->market);
}

TEST_F(TaskManagerTest, RejectReportCarriesItsReason)
{
    auto manager = createTaskManager(1, 1);

    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->side_ = INVALID_SIDE; // fails OrderEntry::isValid(), so the engine rejects the order
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));

    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    OrderEntry *saved = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(REJECTED_ORDSTATUS, saved->status_);
    auto reject = outQueues_->report(REJECT_EXECTYPE);
    ASSERT_TRUE(reject.has_value()) << "no REJECT report";
    EXPECT_EQ(REJECTED_ORDSTATUS, reject->status);
    EXPECT_EQ(saved->orderId_, reject->orderId);
    EXPECT_EQ("Invalid value of the side!", reject->rejectReason);
}

// =============================================================================
// Orders Refused Without Being Stored (#67)
// =============================================================================

TEST_F(TaskManagerTest, OrderWithAClOrdIdInUseIsRefusedWithoutBeingStored)
{
    // The bug this covers: such an order was rejected through a transaction although it was never stored, so the
    // reject carried no order id. Debug aborted on TransactionScope's id assert; Release dereferenced null (#67).
    auto manager = createTaskManager(1, 1);

    auto first = createCorrectOrder(instrumentId1_);
    assignClOrderId(first.get());
    RawDataEntry clOrdId = first->clOrderId_.get();
    const std::string clOrdText(clOrdId.data_, clOrdId.length_);
    auto second = createCorrectOrder(instrumentId1_);
    second->clOrderId_ = first->clOrderId_;
    second->side_ = SELL_SIDE;
    inQueues_->push("first-client", OrderEvent(first.release()));
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    inQueues_->push("second-client", OrderEvent(second.release()));
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));

    OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, stored);
    EXPECT_EQ(NEW_ORDSTATUS, stored->status_);
    EXPECT_EQ(BUY_SIDE, stored->side_);
    EXPECT_EQ(0, manager->tasksFailed());
    auto rejects = outQueues_->orderRejects();
    ASSERT_EQ(1u, rejects.size());
    EXPECT_EQ("second-client", rejects[0].second);
    EXPECT_EQ(clOrdText, rejects[0].first.clOrderId_);
    EXPECT_FALSE(rejects[0].first.replacement_);
    EXPECT_TRUE(rejects[0].first.duplicateClOrderId_);
    EXPECT_EQ(SELL_SIDE, rejects[0].first.side_);
    EXPECT_EQ(77u, rejects[0].first.orderQty_);
    EXPECT_EQ("aaa", rejects[0].first.symbol_);
    EXPECT_EQ("Order refused: ClOrdID " + clOrdText + " is already in use", rejects[0].first.reason_);
}

TEST_F(TaskManagerTest, ReplacementWithAClOrdIdInUseIsRefusedWithoutBeingStored)
{
    // A cancel/replace request's replacement can reuse a ClOrdID too: here, the original's own (#67). The replacement
    // is a clone, so it carries the original's order id, and the old code rejected and persisted it under that id: the
    // refused replacement's state became the newest version of the original's record.
    RecordingSaver saver;
    OrderStorage::instance()->attach(&saver);
    auto manager = createTaskManager(1, 1);

    auto original = createCorrectOrder(instrumentId1_);
    assignClOrderId(original.get());
    RawDataEntry clOrdId = original->clOrderId_.get();
    const std::string clOrdText(clOrdId.data_, clOrdId.length_);
    inQueues_->push("client", OrderEvent(original.release()));
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    OrderEntry *stored = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, stored);

    // Built as the FIX gateway builds one, a clone of the original, but keeping the original's ClOrdID. Nothing frees
    // a replacement order today (a separate leak); this one is refused, so nothing refers to it afterwards.
    std::unique_ptr<OrderEntry> replacement(stored->clone());
    replacement->origClOrderId_ = stored->clOrderId_;
    replacement->orderQty_ = 50;
    inQueues_->push("client", OrderReplaceEvent(stored->orderId_, replacement.get()));
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));

    EXPECT_EQ(NEW_ORDSTATUS, stored->status_);
    EXPECT_EQ(77u, stored->orderQty_);
    EXPECT_EQ(0, manager->tasksFailed());
    for (const auto &version : saver.versions())
    {
        if (stored->orderId_ == version.orderId)
        {
            EXPECT_EQ(NEW_ORDSTATUS, version.status) << "the refused replacement was persisted as the original";
            EXPECT_EQ(77u, version.orderQty);
        }
    }
    auto rejects = outQueues_->orderRejects();
    ASSERT_EQ(1u, rejects.size());
    EXPECT_EQ("client", rejects[0].second);
    EXPECT_TRUE(rejects[0].first.replacement_);
    EXPECT_TRUE(rejects[0].first.duplicateClOrderId_);
    EXPECT_EQ(clOrdText, rejects[0].first.clOrderId_);
    EXPECT_EQ(clOrdText, rejects[0].first.origClOrderId_);
    EXPECT_EQ(50u, rejects[0].first.orderQty_);
}

// =============================================================================
// Failure Containment Tests (#57)
// =============================================================================

TEST_F(TaskManagerTest, CancelOfAnUnknownOrderIsRejectedAndProcessingContinues)
{
    // The bug this covers: a cancel for an order that does not exist threw out of its task. That cancelled the task
    // group, so no later event was processed until restart (#57).
    auto manager = createTaskManager(1, 1);

    inQueues_->push("test", OrderCancelEvent(IdT(999, 1), "no such order"));
    inQueues_->push("test", OrderCancelEvent(IdT(), "invalid id"));
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));

    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    EXPECT_EQ(2, outQueues_->cancelRejectCount_.load());
    OrderEntry *saved = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(NEW_ORDSTATUS, saved->status_);
    EXPECT_EQ(0, manager->tasksFailed());
}

TEST_F(TaskManagerTest, CancelRejectNamesTheOrder)
{
    auto manager = createTaskManager(1, 1);

    inQueues_->push("test", OrderCancelEvent(IdT(999, 1), "no such order"));

    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    EXPECT_EQ(1, outQueues_->cancelRejectCount_.load());
    EXPECT_EQ(999u, outQueues_->lastCancelRejectOrderId_.load());
}

TEST_F(TaskManagerTest, EventThatThrowsDoesNotStopProcessing)
{
    // Any exception out of an event task is contained: it is logged and counted, and later events still run (#57).
    // A state change for an order that does not exist still throws.
    auto manager = createTaskManager(1, 1);

    inQueues_->push("test", OrderChangeStateEvent(IdT(999, 1), OrderChangeStateEvent::SUSPEND));
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    RawDataEntry clOrdId = order->clOrderId_.get();
    inQueues_->push("test", OrderEvent(order.release()));

    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    OrderEntry *saved = OrderStorage::instance()->locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(NEW_ORDSTATUS, saved->status_);
    EXPECT_EQ(1, manager->tasksFailed());
}

TEST_F(TaskManagerTest, TransactionThatThrowsDoesNotStopProcessing)
{
    // A transaction task's exception is contained too, and finishTransaction() still runs, so the transactions
    // ordered after it are not blocked (#57).
    TaskManagerParams params;
    params.transactMgr_ = transMgr_.get();
    params.inQueues_ = inQueues_.get();
    auto evntProc = std::make_unique<Processor>();
    evntProc->init(*procParams_);
    params.evntProcessors_.push_back(evntProc.release());
    auto trProc = std::make_unique<Processor>();
    trProc->init(*procParams_);
    params.transactProcessors_.push_back(new ThrowOnceTransactionProcessor(std::move(trProc)));
    auto manager = std::make_unique<TaskManager>(params);

    // Both orders are on one instrument, so the second order's transaction runs after the first's
    for (int i = 0; i < 2; ++i)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        inQueues_->push("test", OrderEvent(order.release()));
    }

    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));
    EXPECT_EQ(1, manager->tasksFailed());
    EXPECT_GE(outQueues_->execReportCount_.load(), 1);
}

// =============================================================================
// Cancel Tests (#73)
// =============================================================================

TEST_F(TaskManagerTest, CancelOfARestingOrderCompletesAndTheOrderStopsTrading)
{
    // The bug this covers: a cancel only reached Pending Cancel. The order stayed in the book with its status
    // unchanged, and a later order on the other side filled it (#73).
    auto manager = createTaskManager(1, 1);
    RawDataEntry buyId = pushOrder(BUY_SIDE, 10.0, 100, "buyer");
    EXPECT_TRUE(waitUntilIdle(*manager));
    OrderEntry *buy = OrderStorage::instance()->locateByClOrderId(buyId);
    ASSERT_NE(nullptr, buy);

    inQueues_->push("buyer", OrderCancelEvent(buy->orderId_, "user"));
    RawDataEntry sellId = pushOrder(SELL_SIDE, 10.0, 100, "seller");
    EXPECT_TRUE(waitUntilIdle(*manager));

    EXPECT_EQ(CANCELED_ORDSTATUS, buy->status_);
    EXPECT_EQ(0u, buy->leavesQty_);
    EXPECT_EQ(0u, buy->cumQty_);
    EXPECT_EQ(1, outQueues_->reportCount(CANCEL_EXECTYPE, buy->orderId_));
    EXPECT_EQ(0, outQueues_->reportCount(TRADE_EXECTYPE, buy->orderId_));
    EXPECT_EQ(0, outQueues_->cancelRejectCount_.load());
    OrderEntry *sell = OrderStorage::instance()->locateByClOrderId(sellId);
    ASSERT_NE(nullptr, sell);
    EXPECT_EQ(NEW_ORDSTATUS, sell->status_) << "the sell traded with the cancelled buy";
    EXPECT_EQ(0, manager->tasksFailed());
}

TEST_F(TaskManagerTest, CancelOfAPartiallyFilledOrderKeepsItsFills)
{
    auto manager = createTaskManager(1, 1);
    RawDataEntry buyId = pushOrder(BUY_SIDE, 10.0, 100, "buyer");
    pushOrder(SELL_SIDE, 10.0, 40, "seller");
    EXPECT_TRUE(waitUntilIdle(*manager));
    OrderEntry *buy = OrderStorage::instance()->locateByClOrderId(buyId);
    ASSERT_NE(nullptr, buy);
    ASSERT_EQ(PARTFILL_ORDSTATUS, buy->status_);

    inQueues_->push("buyer", OrderCancelEvent(buy->orderId_, "rest of it"));
    EXPECT_TRUE(waitUntilIdle(*manager));

    EXPECT_EQ(CANCELED_ORDSTATUS, buy->status_);
    EXPECT_EQ(40u, buy->cumQty_);
    EXPECT_EQ(0u, buy->leavesQty_);
    EXPECT_EQ(1, outQueues_->reportCount(CANCEL_EXECTYPE, buy->orderId_));
}

TEST_F(TaskManagerTest, CancelOfAFilledOrderIsRejectedToTheRequester)
{
    // The bug this covers: a cancel of a filled order got a Pending Cancel report, and no reject (#73)
    auto manager = createTaskManager(1, 1);
    RawDataEntry sellId = pushOrder(SELL_SIDE, 20.0, 100, "seller");
    pushOrder(BUY_SIDE, 20.0, 100, "buyer");
    EXPECT_TRUE(waitUntilIdle(*manager));
    OrderEntry *sell = OrderStorage::instance()->locateByClOrderId(sellId);
    ASSERT_NE(nullptr, sell);
    ASSERT_EQ(FILLED_ORDSTATUS, sell->status_);
    const size_t reportsBefore = outQueues_->reportCountAll();

    inQueues_->push("requester", OrderCancelEvent(sell->orderId_, "too late"));
    EXPECT_TRUE(waitUntilIdle(*manager));

    auto rejects = outQueues_->cancelRejects();
    ASSERT_EQ(1u, rejects.size());
    EXPECT_EQ(sell->orderId_, rejects[0].first.id_);
    EXPECT_EQ(CancelRejectEvent::TOO_LATE, rejects[0].first.reason_);
    EXPECT_EQ(FILLED_ORDSTATUS, rejects[0].first.ordStatus_);
    EXPECT_EQ("requester", rejects[0].second);
    EXPECT_EQ(reportsBefore, outQueues_->reportCountAll()) << "a refused cancel sent a report";
    EXPECT_EQ(FILLED_ORDSTATUS, sell->status_);
}

TEST_F(TaskManagerTest, SecondCancelOfAnOrderIsRejected)
{
    // The bug this covers: a second cancel found no transition, and the state machine swallowed the error, so the
    // client got no answer at all (#73)
    auto manager = createTaskManager(1, 1);
    RawDataEntry buyId = pushOrder(BUY_SIDE, 10.0, 100, "buyer");
    EXPECT_TRUE(waitUntilIdle(*manager));
    OrderEntry *buy = OrderStorage::instance()->locateByClOrderId(buyId);
    ASSERT_NE(nullptr, buy);

    inQueues_->push("buyer", OrderCancelEvent(buy->orderId_, "first"));
    inQueues_->push("buyer", OrderCancelEvent(buy->orderId_, "second"));
    EXPECT_TRUE(waitUntilIdle(*manager));

    EXPECT_EQ(1, outQueues_->reportCount(CANCEL_EXECTYPE, buy->orderId_));
    auto rejects = outQueues_->cancelRejects();
    ASSERT_EQ(1u, rejects.size());
    EXPECT_EQ(buy->orderId_, rejects[0].first.id_);
    EXPECT_EQ(CancelRejectEvent::TOO_LATE, rejects[0].first.reason_);
    EXPECT_EQ(CANCELED_ORDSTATUS, rejects[0].first.ordStatus_);
}

TEST_F(TaskManagerTest, CancelPersistsTheCancelledOrder)
{
    // The cancelled state is what a restart restores, and restore does not re-rest a cancelled order
    RecordingSaver saver;
    OrderStorage::instance()->attach(&saver);
    auto manager = createTaskManager(1, 1);
    RawDataEntry buyId = pushOrder(BUY_SIDE, 10.0, 100, "buyer");
    EXPECT_TRUE(waitUntilIdle(*manager));
    OrderEntry *buy = OrderStorage::instance()->locateByClOrderId(buyId);
    ASSERT_NE(nullptr, buy);

    inQueues_->push("buyer", OrderCancelEvent(buy->orderId_, "user"));
    EXPECT_TRUE(waitUntilIdle(*manager));

    std::optional<RecordingSaver::Version> newest;
    for (const auto &version : saver.versions())
    {
        if (buy->orderId_ == version.orderId)
        {
            newest = version;
        }
    }
    ASSERT_TRUE(newest.has_value());
    EXPECT_EQ(CANCELED_ORDSTATUS, newest->status);
    EXPECT_EQ(0u, newest->leavesQty);
}

TEST_F(TaskManagerTest, CancelsRacingTradesLeaveBothSidesConsistent)
{
    // Cancels, and orders that would trade with the orders being cancelled, all processed concurrently. Each cancel
    // must be answered once, a cancelled order must not trade after its cancel, and every fill must have its other
    // side: decided as soon as it arrived, a cancel could overtake a trade already matched against the order (#73).
    auto manager = createTaskManager(3, 3);
    const int count = 40;
    std::vector<RawDataEntry> buyIds;
    for (int i = 0; i < count; ++i)
    {
        buyIds.push_back(pushOrder(BUY_SIDE, 10.0, 100, "buyer"));
    }
    EXPECT_TRUE(waitUntilIdle(*manager));
    std::vector<OrderEntry *> buys;
    for (const auto &id : buyIds)
    {
        buys.push_back(OrderStorage::instance()->locateByClOrderId(id));
        ASSERT_NE(nullptr, buys.back());
    }

    std::vector<RawDataEntry> sellIds;
    for (int i = 0; i < count; ++i)
    {
        inQueues_->push("buyer", OrderCancelEvent(buys[i]->orderId_, "race"));
        sellIds.push_back(pushOrder(SELL_SIDE, 10.0, 100, "seller"));
    }
    EXPECT_TRUE(waitUntilIdle(*manager, std::chrono::seconds(20)));

    const auto rejects = outQueues_->cancelRejects();
    u64 bought = 0;
    for (OrderEntry *buy : buys)
    {
        bought += buy->cumQty_;
        const int rejected = static_cast<int>(std::count_if(rejects.begin(), rejects.end(),
                                                            [buy](const auto &r)
                                                            {
                                                                return buy->orderId_ == r.first.id_;
                                                            }));
        const auto types = outQueues_->reportTypes(buy->orderId_);
        const int cancels = static_cast<int>(std::count(types.begin(), types.end(), CANCEL_EXECTYPE));
        EXPECT_EQ(1, rejected + cancels) << "order " << buy->orderId_.id_ << " got no answer, or more than one";
        if (0 < cancels)
        {
            EXPECT_EQ(CANCELED_ORDSTATUS, buy->status_) << "order " << buy->orderId_.id_;
            EXPECT_EQ(0u, buy->leavesQty_) << "order " << buy->orderId_.id_;
            const auto cancelled = std::find(types.begin(), types.end(), CANCEL_EXECTYPE);
            EXPECT_EQ(types.end(), std::find(cancelled, types.end(), TRADE_EXECTYPE))
                << "order " << buy->orderId_.id_ << " traded after it was cancelled";
        }
        else
        {
            // a live order's cancel always succeeds, so only a filled one can have been refused
            EXPECT_EQ(FILLED_ORDSTATUS, buy->status_) << "order " << buy->orderId_.id_;
        }
    }
    u64 sold = 0;
    for (const auto &id : sellIds)
    {
        OrderEntry *sell = OrderStorage::instance()->locateByClOrderId(id);
        ASSERT_NE(nullptr, sell);
        sold += sell->cumQty_;
    }
    EXPECT_EQ(bought, sold) << "a fill without its other side";
    EXPECT_EQ(0, manager->tasksFailed());
}

} // namespace
