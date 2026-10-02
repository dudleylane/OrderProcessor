/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 Migrated from testTaskManager.cpp
*/

#include <gtest/gtest.h>
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

    void push(const CancelRejectEvent &evnt, const std::string &) override
    {
        lastCancelRejectOrderId_ = evnt.id_.id_;
        ++cancelRejectCount_;
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

} // namespace
