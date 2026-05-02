#include "MetricsPublisher.h"
#include "JsonSerializer.h"
#include "SessionManager.h"
#include "TaskManager.h"
#include "IncomingQueues.h"
#include "OrderStorage.h"
#include "TransactionScopePool.h"
#include "TransactionScope.h"

#include <chrono>

using namespace COP::App;

MetricsPublisher::MetricsPublisher(boost::asio::io_context &ioc, SessionManager *sessionMgr,
                                   Tasks::TaskManager *taskMgr, Queues::InQueuesContainer *inQueues,
                                   Store::OrderDataStorage *orderStorage, const ACID::TransactionScopePool *scopePool,
                                   std::chrono::milliseconds interval)
    : timer_(ioc), interval_(interval), running_(false), sessionMgr_(sessionMgr), taskMgr_(taskMgr),
      inQueues_(inQueues), orderStorage_(orderStorage), scopePool_(scopePool)
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
    SystemMetrics m{};

    // TaskManager stats (all relaxed reads — no hot-path overhead)
    m.eventsCreated = taskMgr_->eventsCreated();
    m.eventsProcessed = taskMgr_->eventsProcessed();
    m.eventsFinished = taskMgr_->eventsFinished();
    m.transactionsCreated = taskMgr_->transactionsCreated();
    m.transactionsProcessed = taskMgr_->transactionsProcessed();
    m.transactionsFinished = taskMgr_->transactionsFinished();
    m.availableEventProcessors = taskMgr_->availableEventProcessors();
    m.totalEventProcessors = taskMgr_->totalEventProcessors();
    m.availableTransactProcessors = taskMgr_->availableTransactProcessors();
    m.totalTransactProcessors = taskMgr_->totalTransactProcessors();

    // Queue depth
    m.queueDepth = inQueues_->size();

    // Pool stats
    m.poolSize = scopePool_->poolSize();
    m.poolCacheMisses = scopePool_->cacheMisses();
    m.poolArenaSize = ACID::TransactionScope::ARENA_SIZE;

    // Sessions
    m.activeSessions = sessionMgr_->sessionCount();

    // Order count
    size_t orderCount = 0;
    orderStorage_->forEachOrder(
        [&orderCount](const IdT &, const OrderEntry &)
        {
            ++orderCount;
        });
    m.activeOrders = orderCount;

    // Timestamp
    m.timestamp = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    sessionMgr_->broadcast(serializeMetricsUpdate(m));
}
