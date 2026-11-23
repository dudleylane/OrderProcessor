#include "MetricsPublisher.h"
#include "JsonSerializer.h"
#include "SessionManager.h"
#include "TaskManager.h"
#include "IncomingQueues.h"
#include "LMDBStorage.h"
#include "OrderStorage.h"
#include "TransactionScopePool.h"
#include "TransactionScope.h"
#include "TransactionDef.h"

#include <chrono>

using namespace COP::App;

MetricsPublisher::MetricsPublisher(boost::asio::io_context &ioc, SessionManager *sessionMgr,
                                   Tasks::TaskManager *taskMgr, Queues::InQueuesContainer *inQueues,
                                   const ACID::TransactionManager *transactMgr, Store::OrderDataStorage *orderStorage,
                                   std::vector<const ACID::TransactionScopePool *> scopePools,
                                   const Store::LMDBStorage *storage, u64 serverStart,
                                   std::chrono::milliseconds interval)
    : timer_(ioc), interval_(interval), running_(false), sessionMgr_(sessionMgr), taskMgr_(taskMgr),
      inQueues_(inQueues), transactMgr_(transactMgr), orderStorage_(orderStorage), scopePools_(std::move(scopePools)),
      storage_(storage), serverStart_(serverStart)
{
}

void MetricsPublisher::start()
{
    running_ = true;
    timer_.expires_after(interval_);
    timer_.async_wait(
        [self = shared_from_this()](boost::system::error_code ec)
        {
            self->onTimer(ec);
        });
}

void MetricsPublisher::stop()
{
    running_ = false;
    timer_.cancel();
}

void MetricsPublisher::onTimer(boost::system::error_code ec)
{
    if (ec || !running_)
    {
        return;
    }
    collectAndBroadcast();
    timer_.expires_after(interval_);
    timer_.async_wait(
        [self = shared_from_this()](boost::system::error_code ec2)
        {
            self->onTimer(ec2);
        });
}

void MetricsPublisher::collectAndBroadcast()
{
    sessionMgr_->broadcast(serializeMetricsUpdate(collect()));
}

SystemMetrics MetricsPublisher::collect() const
{
    SystemMetrics m{};

    // TaskManager stats (all relaxed reads — no hot-path overhead)
    m.eventsCreated = taskMgr_->eventsCreated();
    m.eventsProcessed = taskMgr_->eventsProcessed();
    m.eventsFinished = taskMgr_->eventsFinished();
    m.transactionsCreated = taskMgr_->transactionsCreated();
    m.transactionsProcessed = taskMgr_->transactionsProcessed();
    m.transactionsFinished = taskMgr_->transactionsFinished();
    m.transactionsFailed = ACID::TransactionScope::failedCount(); // every engine in the process (#514)
    m.availableEventProcessors = taskMgr_->availableEventProcessors();
    m.totalEventProcessors = taskMgr_->totalEventProcessors();
    m.availableTransactProcessors = taskMgr_->availableTransactProcessors();
    m.totalTransactProcessors = taskMgr_->totalTransactProcessors();

    // Queue depth, and the transactions not yet run: together the work in flight --max-in-flight bounds (#519)
    m.queueDepth = inQueues_->size();
    m.pendingTransactions = transactMgr_->pendingTransactions();

    // Pool stats, summed over every processor's pool (#542)
    const ACID::PoolFigures pools = ACID::sumPoolFigures(scopePools_);
    m.poolSize = pools.poolSize;
    m.poolCacheMisses = pools.cacheMisses;
    m.poolAllocations = pools.allocations; // one per transaction (#515)
    m.poolArenaSize = ACID::TransactionScope::ARENA_SIZE;

    // Sessions
    m.activeSessions = sessionMgr_->sessionCount();

    // The orders stored, open or closed: the store's size, where a walk over every order under its lock counted them,
    // and called the count active (#543)
    m.storedOrders = orderStorage_->orderCount();

    // LMDB's map: its size, which doubles when a write finds it full, and how much of it is in use (#148)
    if (nullptr != storage_)
    {
        m.storageMapSize = storage_->mapSize();
        m.storageMapUsed = storage_->mapUsed();
    }

    // The server's start, the same in every sample it sends, and the sample's own time
    m.serverStart = serverStart_;
    m.timestamp = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    return m;
}
