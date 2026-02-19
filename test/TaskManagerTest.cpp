/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 Migrated from testTaskManager.cpp
*/

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>

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

namespace {

// =============================================================================
// Test Output Queue - Captures outgoing events
// =============================================================================

class TestOutQueues : public OutQueues {
public:
    TestOutQueues() : execReportCount_(0), cancelRejectCount_(0), businessRejectCount_(0) {}

    void push(const ExecReportEvent &, const std::string &) override {
        ++execReportCount_;
    }

    void push(const CancelRejectEvent &, const std::string &) override {
        ++cancelRejectCount_;
    }

    void push(const BusinessRejectEvent &, const std::string &) override {
        ++businessRejectCount_;
    }

    int totalEvents() const {
        return execReportCount_.load() + cancelRejectCount_.load() + businessRejectCount_.load();
    }

    std::atomic<int> execReportCount_;
    std::atomic<int> cancelRejectCount_;
    std::atomic<int> businessRejectCount_;
};

// =============================================================================
// Task Manager Test Fixture
// =============================================================================

class TaskManagerTest : public ProcessorFixture {
protected:
    void SetUp() override {
        ProcessorFixture::SetUp();

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<TestOutQueues>();

        // Initialize transaction manager
        TransactionMgrParams transParams(IdTGenerator::instance());
        transMgr_ = std::make_unique<TransactionMgr>();
        transMgr_->init(transParams);

        // Initialize processor parameters
        procParams_ = std::make_unique<ProcessorParams>(
            IdTGenerator::instance(),
            OrderStorage::instance(),
            orderBook_.get(),
            inQueues_.get(),
            outQueues_.get(),
            inQueues_.get(),
            transMgr_.get());
    }

    void TearDown() override {
        if (transMgr_) {
            transMgr_->stop();
        }
        procParams_.reset();
        transMgr_.reset();
        outQueues_.reset();
        inQueues_.reset();

        ProcessorFixture::TearDown();
    }

    // Helper to create a task manager with specified number of processors
    std::unique_ptr<TaskManager> createTaskManager(int eventProcessors, int transactionProcessors) {
        TaskManagerParams params;
        params.transactMgr_ = transMgr_.get();
        params.inQueues_ = inQueues_.get();

        for (int i = 0; i < eventProcessors; ++i) {
            auto proc = std::make_unique<Processor>();
            proc->init(*procParams_);
            params.evntProcessors_.push_back(proc.release());
        }

        for (int i = 0; i < transactionProcessors; ++i) {
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

TEST_F(TaskManagerTest, CreateWithSingleProcessor) {
    auto manager = createTaskManager(1, 1);
    ASSERT_NE(nullptr, manager);
}

TEST_F(TaskManagerTest, CreateWithMultipleProcessors) {
    auto manager = createTaskManager(3, 3);
    ASSERT_NE(nullptr, manager);
}

// =============================================================================
// Single Order Processing Tests
// =============================================================================

TEST_F(TaskManagerTest, ProcessSingleOrder) {
    auto manager = createTaskManager(1, 1);

    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());

    inQueues_->push("test", OrderEvent(order.release()));

    // Wait for transaction to complete
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(5));

    // Should have generated at least one execution report
    EXPECT_GE(outQueues_->execReportCount_.load(), 1);
}

TEST_F(TaskManagerTest, ProcessMultipleOrders) {
    auto manager = createTaskManager(2, 2);

    const int numOrders = 10;
    for (int i = 0; i < numOrders; ++i) {
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

TEST_F(TaskManagerTest, MatchBuyAndSellOrders) {
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

TEST_F(TaskManagerTest, ConcurrentOrderSubmission) {
    auto manager = createTaskManager(3, 3);

    const int numOrders = 100;
    std::atomic<int> ordersSubmitted{0};

    // Submit orders from multiple threads
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([this, &ordersSubmitted, numOrders]() {
            for (int i = 0; i < numOrders / 4; ++i) {
                auto order = createCorrectOrder(instrumentId1_);
                assignClOrderId(order.get());
                inQueues_->push("test", OrderEvent(order.release()));
                ++ordersSubmitted;
            }
        });
    }

    // Wait for all threads to finish submitting
    for (auto& thread : threads) {
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

TEST_F(TaskManagerTest, WaitUntilTransactionsFinishedWithEmptyQueue) {
    auto manager = createTaskManager(1, 1);

    // With no transactions, should return immediately
    EXPECT_TRUE(manager->waitUntilTransactionsFinished(1));
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(TaskManagerTest, HandleHighVolume) {
    auto manager = createTaskManager(3, 3);

    const int numOrders = 500;
    for (int i = 0; i < numOrders; ++i) {
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

TEST_F(TaskManagerTest, ProcessMixedBuySellOrders) {
    auto manager = createTaskManager(3, 3);

    const int numPairs = 50;

    for (int i = 0; i < numPairs; ++i) {
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

} // namespace
