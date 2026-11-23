/**
 Concurrent Order Processor library - Google Test

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 What metrics_update carries (#660): MetricsPublisher::collect(), each figure against its source. No test compiled
 the publisher before, so a figure it stopped filling would have gone out as 0 unnoticed.
*/

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "TestFixtures.h"
#include "TestAux.h"
#include "MetricsPublisher.h"
#include "JsonSerializer.h"
#include "SessionManager.h"
#include "TaskManager.h"
#include "TransactionMgr.h"
#include "TransactionScope.h"
#include "TransactionScopePool.h"
#include "Processor.h"
#include "IncomingQueues.h"
#include "LMDBStorage.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::Tasks;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace test;

namespace
{

/// Reports go nowhere: the figures are the point
class NullOutQueues final : public OutQueues
{
public:
    void push(const ExecReportEvent &, const std::string &) override {}
    void push(const CancelRejectEvent &, const std::string &) override {}
    void push(const OrderRejectEvent &, const std::string &) override {}
};

/// A load that keeps nothing: the directory starts empty
class NullObserver final : public FileStorageObserver
{
public:
    void startLoad() override {}
    void onRecordLoaded(const IdT &, u32, const char *, size_t) override {}
    void finishLoad() override {}
};

/// An operation that fails its transaction, as transactionsFailed counts (#514)
class ThrowingOperation final : public Operation
{
public:
    ThrowingOperation() : Operation(MATCH_ORDER_TROPERATION, IdT(1, 1)) {}
    void execute(const Context &) override
    {
        throw std::runtime_error("injected: the transaction fails");
    }
    void rollback(const Context &) override {}
};

/// A server's start, which the publisher is given and every sample carries (#677): a day long past, so that it can't
/// be taken for a sample's own time
constexpr u64 SERVER_START = 1763856000000; // 2025-11-23T00:00:00Z

u64 nowMs()
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

class MetricsPublisherTest : public ProcessorFixture
{
protected:
    void SetUp() override
    {
        ProcessorFixture::SetUp();
        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<NullOutQueues>();
        TransactionMgrParams transParams(IdTGenerator::instance());
        transMgr_ = std::make_unique<TransactionMgr>();
        transMgr_->init(transParams);
        procParams_ =
            std::make_unique<ProcessorParams>(IdTGenerator::instance(), OrderStorage::instance(), orderBook_.get(),
                                              inQueues_.get(), outQueues_.get(), inQueues_.get(), transMgr_.get());
        storageDir_ = uniqueTestPath("metrics_lmdb");
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
        std::filesystem::remove_all(storageDir_);
    }

    /// A manager over two event and two transaction processors; pools_ gets each one's scope pool
    std::unique_ptr<TaskManager> createTaskManager()
    {
        TaskManagerParams params;
        params.transactMgr_ = transMgr_.get();
        params.inQueues_ = inQueues_.get();
        for (int i = 0; i < 2; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(*procParams_);
            pools_.push_back(proc->scopePool());
            params.evntProcessors_.push_back(proc.release());
        }
        for (int i = 0; i < 2; ++i)
        {
            auto proc = std::make_unique<Processor>();
            proc->init(*procParams_);
            pools_.push_back(proc->scopePool());
            params.transactProcessors_.push_back(proc.release());
        }
        return std::make_unique<TaskManager>(params);
    }

    bool waitUntilIdle(TaskManager &manager)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (manager.waitUntilTransactionsFinished(0))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<NullOutQueues> outQueues_;
    std::unique_ptr<TransactionMgr> transMgr_;
    std::unique_ptr<ProcessorParams> procParams_;
    std::vector<const TransactionScopePool *> pools_;
    std::string storageDir_;
};

TEST_F(MetricsPublisherTest, EachFigureComesFromItsSource)
{
    std::unique_ptr<TaskManager> manager = createTaskManager();
    NullObserver observer;
    LMDBStorage storage;
    storage.load(storageDir_, &observer);
    App::SessionManager sessions;
    boost::asio::io_context ioc;
    auto publisher =
        std::make_shared<App::MetricsPublisher>(ioc, &sessions, manager.get(), inQueues_.get(), transMgr_.get(),
                                                OrderStorage::instance(), pools_, &storage, SERVER_START);

    // One order through the engine, which rests: every counter moves from 0
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    inQueues_->push("test", OrderEvent(order.release()));
    ASSERT_TRUE(waitUntilIdle(*manager));

    const u64 before = nowMs();
    const App::SystemMetrics m = publisher->collect();
    const u64 after = nowMs();

    EXPECT_EQ(manager->eventsCreated(), m.eventsCreated);
    EXPECT_LT(0u, m.eventsCreated);
    EXPECT_EQ(manager->eventsProcessed(), m.eventsProcessed);
    EXPECT_LT(0u, m.eventsProcessed);
    EXPECT_EQ(manager->eventsFinished(), m.eventsFinished);
    EXPECT_LT(0u, m.eventsFinished);
    EXPECT_EQ(manager->transactionsCreated(), m.transactionsCreated);
    EXPECT_LT(0u, m.transactionsCreated);
    EXPECT_EQ(manager->transactionsProcessed(), m.transactionsProcessed);
    EXPECT_LT(0u, m.transactionsProcessed);
    EXPECT_EQ(manager->transactionsFinished(), m.transactionsFinished);
    EXPECT_LT(0u, m.transactionsFinished);
    EXPECT_EQ(TransactionScope::failedCount(), m.transactionsFailed);

    EXPECT_EQ(2u, m.totalEventProcessors);
    EXPECT_EQ(2u, m.totalTransactProcessors);
    EXPECT_EQ(manager->availableEventProcessors(), m.availableEventProcessors);
    EXPECT_EQ(manager->availableTransactProcessors(), m.availableTransactProcessors);

    EXPECT_EQ(inQueues_->size(), m.queueDepth);
    EXPECT_EQ(transMgr_->pendingTransactions(), m.pendingTransactions);

    // Every processor's pool, summed (#542), each figure from the pools themselves
    size_t poolSize = 0;
    size_t cacheMisses = 0;
    size_t allocations = 0;
    for (const TransactionScopePool *pool : pools_)
    {
        poolSize += pool->poolSize();
        cacheMisses += pool->cacheMisses();
        allocations += pool->allocations();
    }
    EXPECT_EQ(poolSize, m.poolSize);
    EXPECT_LT(0u, m.poolSize);
    EXPECT_EQ(cacheMisses, m.poolCacheMisses);
    EXPECT_EQ(allocations, m.poolAllocations);
    EXPECT_EQ(TransactionScope::ARENA_SIZE, m.poolArenaSize);

    EXPECT_EQ(sessions.sessionCount(), m.activeSessions);
    EXPECT_EQ(1u, m.activeOrders);

    EXPECT_EQ(storage.mapSize(), m.storageMapSize);
    EXPECT_LT(0u, m.storageMapSize);
    EXPECT_EQ(storage.mapUsed(), m.storageMapUsed);

    EXPECT_EQ(SERVER_START, m.serverStart);
    EXPECT_LE(before, m.timestamp);
    EXPECT_GE(after, m.timestamp);
}

TEST_F(MetricsPublisherTest, AFailedTransactionIsCounted)
{
    std::unique_ptr<TaskManager> manager = createTaskManager();
    App::SessionManager sessions;
    boost::asio::io_context ioc;
    auto publisher =
        std::make_shared<App::MetricsPublisher>(ioc, &sessions, manager.get(), inQueues_.get(), transMgr_.get(),
                                                OrderStorage::instance(), pools_, nullptr, SERVER_START);
    const u64 failedBefore = publisher->collect().transactionsFailed;

    TransactionScope scope;
    std::unique_ptr<Operation> failing(new ThrowingOperation());
    scope.addOperation(failing);
    EXPECT_FALSE(scope.executeTransaction(Context()));

    const App::SystemMetrics m = publisher->collect();
    EXPECT_EQ(failedBefore + 1, m.transactionsFailed);
    // Without LMDB, as in a test of the engine alone, the map's figures stay 0
    EXPECT_EQ(0u, m.storageMapSize);
    EXPECT_EQ(0u, m.storageMapUsed);
}

TEST_F(MetricsPublisherTest, TheQueueAndTheWaitingTransactionsAreTheirSourcesSizes)
{
    // An idle engine has both at 0, which a figure never filled would also give: so a queue no manager drains, holding
    // two events, and a transaction manager no processor takes from, holding one transaction
    std::unique_ptr<TaskManager> manager = createTaskManager();
    IncomingQueues waiting;
    for (int i = 0; i < 2; ++i)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        waiting.push("test", OrderEvent(order.release()));
    }
    TransactionMgr holding;
    holding.init(TransactionMgrParams(IdTGenerator::instance()));
    std::unique_ptr<Transaction> transaction(new TransactionScope());
    holding.addTransaction(transaction);
    ASSERT_EQ(2u, waiting.size());
    ASSERT_EQ(1u, holding.pendingTransactions());

    App::SessionManager sessions;
    boost::asio::io_context ioc;
    auto publisher = std::make_shared<App::MetricsPublisher>(ioc, &sessions, manager.get(), &waiting, &holding,
                                                             OrderStorage::instance(), pools_, nullptr, SERVER_START);
    const App::SystemMetrics m = publisher->collect();
    EXPECT_EQ(2u, m.queueDepth);
    EXPECT_EQ(1u, m.pendingTransactions);
    holding.stop();
}
