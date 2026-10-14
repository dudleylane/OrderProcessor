#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <boost/beast/core.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "TypesDef.h"
#include "ServerDefaults.h"

namespace COP
{

namespace Store
{
class WideParamsDataStorage;
class OrderDataStorage;
} // namespace Store
class OrderBookImpl;
class IdTValueGenerator;

namespace Queues
{
class InQueues;
}

namespace App
{

class SessionManager;

class WsServer : public std::enable_shared_from_this<WsServer>
{
public:
    WsServer(boost::asio::io_context &ioc, boost::asio::ip::tcp::endpoint endpoint, SessionManager *sessionMgr,
             Store::WideParamsDataStorage *wideData, Store::OrderDataStorage *orderStorage, Queues::InQueues *inQueues,
             IdTValueGenerator *idGen, OrderBookImpl *orderBook, SourceIdT defaultClearingId,
             SourceIdT defaultAccountId, const WsLimits &limits = WsLimits());

    void run();
    /// Where the server listens: the port the system chose, when the endpoint asked for port 0
    boost::asio::ip::tcp::endpoint localEndpoint() const
    {
        return acceptor_.local_endpoint();
    }

private:
    void doAccept();
    void onAccept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);
    void onAcceptRetry(boost::beast::error_code ec);

    /// At most one warning a minute about one kind of problem, with how many there were since the last: a flood of
    /// connections would otherwise flood the log, and each warning writes it out (#65)
    struct ThrottledWarning
    {
        std::chrono::steady_clock::time_point last_{};
        u64 since_ = 0;
        void warn(const std::string &text);
    };

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context &ioc_;
    SessionManager *sessionMgr_;
    Store::WideParamsDataStorage *wideData_;
    Store::OrderDataStorage *orderStorage_;
    Queues::InQueues *inQueues_;
    IdTValueGenerator *idGen_;
    OrderBookImpl *orderBook_;
    SourceIdT defaultClearingId_; // passed to every session; see WsSession (#34)
    SourceIdT defaultAccountId_;
    WsLimits limits_;
    /// Sessions alive, against limits_.maxConnections: each one's destructor counts itself out (#159)
    std::shared_ptr<std::atomic<std::size_t>> connections_ = std::make_shared<std::atomic<std::size_t>>(0);
    boost::asio::steady_timer acceptRetry_; // the pause before accepting again after an accept failed (#202)
    ThrottledWarning acceptFailures_;
    ThrottledWarning refusals_;
};

} // namespace App
} // namespace COP
