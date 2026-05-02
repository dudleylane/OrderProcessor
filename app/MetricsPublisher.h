#pragma once

#include <memory>
#include <chrono>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace COP
{

namespace Tasks
{
class TaskManager;
}
namespace Queues
{
class InQueuesContainer;
}
namespace Store
{
class OrderDataStorage;
}
namespace ACID
{
class TransactionScopePool;
}

namespace App
{

class SessionManager;

class MetricsPublisher : public std::enable_shared_from_this<MetricsPublisher>
{
public:
    MetricsPublisher(boost::asio::io_context &ioc, SessionManager *sessionMgr, Tasks::TaskManager *taskMgr,
                     Queues::InQueuesContainer *inQueues, Store::OrderDataStorage *orderStorage,
                     const ACID::TransactionScopePool *scopePool,
                     std::chrono::milliseconds interval = std::chrono::milliseconds{ 1000 });

    void start();
    void stop();

private:
    void onTimer(boost::system::error_code ec);
    void collectAndBroadcast();

    boost::asio::steady_timer timer_;
    std::chrono::milliseconds interval_;
    bool running_;

    SessionManager *sessionMgr_;
    Tasks::TaskManager *taskMgr_;
    Queues::InQueuesContainer *inQueues_;
    Store::OrderDataStorage *orderStorage_;
    const ACID::TransactionScopePool *scopePool_;
};

} // namespace App
} // namespace COP
