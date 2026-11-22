#pragma once

#include <memory>
#include <chrono>
#include <vector>
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
class LMDBStorage;
} // namespace Store
namespace ACID
{
class TransactionScopePool;
class TransactionManager;
} // namespace ACID

namespace App
{

class SessionManager;

class MetricsPublisher : public std::enable_shared_from_this<MetricsPublisher>
{
public:
    MetricsPublisher(boost::asio::io_context &ioc, SessionManager *sessionMgr, Tasks::TaskManager *taskMgr,
                     Queues::InQueuesContainer *inQueues, const ACID::TransactionManager *transactMgr,
                     Store::OrderDataStorage *orderStorage, std::vector<const ACID::TransactionScopePool *> scopePools,
                     const Store::LMDBStorage *storage,
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
    const ACID::TransactionManager *transactMgr_;
    Store::OrderDataStorage *orderStorage_;
    /// Every processor's scope pool, whose figures the metrics sum (#542)
    std::vector<const ACID::TransactionScopePool *> scopePools_;
    const Store::LMDBStorage *storage_;
};

} // namespace App
} // namespace COP
