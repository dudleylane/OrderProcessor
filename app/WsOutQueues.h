#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <memory>

#include "BookPublisher.h"
#include "QueuesDef.h"

namespace COP
{
class OrderBookImpl;

namespace Store
{
class WideParamsDataStorage;
class OrderDataStorage;
} // namespace Store

namespace App
{

class SessionManager;

class WsOutQueues : public Queues::OutQueues
{
public:
    /// The books' depth goes out from ioc's thread, at most once each bookInterval (#694)
    WsOutQueues(boost::asio::io_context &ioc, SessionManager *sessionMgr, Store::WideParamsDataStorage *wideData,
                Store::OrderDataStorage *orderStorage, OrderBookImpl *orderBook,
                std::chrono::milliseconds bookInterval = BOOK_UPDATE_INTERVAL);

    void push(const Queues::ExecReportEvent &evnt, const std::string &target) override;
    void push(const Queues::CancelRejectEvent &evnt, const std::string &target) override;
    void push(const Queues::OrderRejectEvent &evnt, const std::string &target) override;
    /// Sends no book's depth from now on (BookPublisher::stop()): once ioc's thread has returned. The reports still go
    /// out, as the transaction workers push them, until this is freed.
    void stop();

private:
    SessionManager *sessionMgr_;
    Store::WideParamsDataStorage *wideData_;
    Store::OrderDataStorage *orderStorage_;
    /// Sends a watched book's depth from the IO thread, at most once an interval (#694)
    std::shared_ptr<BookPublisher> books_;
};

} // namespace App
} // namespace COP
