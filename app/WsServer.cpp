#include "WsServer.h"
#include "WsSession.h"
#include "SessionManager.h"
#include "Logger.h"

#include <boost/beast/core.hpp>

namespace beast = boost::beast;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace COP;
using namespace COP::App;

namespace
{
/// The pause before accepting again after an accept failed. Long enough that a process out of descriptors (EMFILE)
/// doesn't spin the IO thread, short enough that clients barely notice once they're back (#202).
const std::chrono::milliseconds ACCEPT_RETRY_DELAY(100);
} // namespace

void WsServer::ThrottledWarning::warn(const std::string &text)
{
    const auto now = std::chrono::steady_clock::now();
    if ((std::chrono::steady_clock::time_point() != last_) && (now - last_ < std::chrono::minutes(1)))
    {
        ++since_;
        return;
    }
    aux::ExchLogger::instance()->warn(
        "WsServer: " + text +
        ((0 < since_) ? " (and " + std::to_string(since_) + " more since the last warning)" : ""));
    last_ = now;
    since_ = 0;
}

WsServer::WsServer(net::io_context &ioc, tcp::endpoint endpoint, SessionManager *sessionMgr,
                   Store::WideParamsDataStorage *wideData, Store::OrderDataStorage *orderStorage,
                   Queues::InQueues *inQueues, IdTValueGenerator *idGen, OrderBookImpl *orderBook,
                   SourceIdT defaultClearingId, SourceIdT defaultAccountId, const WsLimits &limits, const WsAuth &auth)
    : acceptor_(ioc), ioc_(ioc), sessionMgr_(sessionMgr), wideData_(wideData), orderStorage_(orderStorage),
      inQueues_(inQueues), idGen_(idGen), orderBook_(orderBook), defaultClearingId_(defaultClearingId),
      defaultAccountId_(defaultAccountId), limits_(limits), auth_(auth), acceptRetry_(ioc)
{
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
    {
        throw std::runtime_error("WsServer open: " + ec.message());
    }

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec)
    {
        throw std::runtime_error("WsServer reuse_address: " + ec.message());
    }

    acceptor_.bind(endpoint, ec);
    if (ec)
    {
        throw std::runtime_error("WsServer bind: " + ec.message());
    }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec)
    {
        throw std::runtime_error("WsServer listen: " + ec.message());
    }
}

void WsServer::run()
{
    doAccept();
}

void WsServer::doAccept()
{
    acceptor_.async_accept(net::make_strand(ioc_), beast::bind_front_handler(&WsServer::onAccept, shared_from_this()));
}

void WsServer::onAccept(beast::error_code ec, tcp::socket socket)
{
    if (ec == net::error::operation_aborted)
    {
        return; // the acceptor was closed: the server is stopping
    }
    if (ec)
    {
        // Accept again, after a pause. A failed accept used to stop the server accepting for good, without a word
        // (#202). Running out of descriptors (EMFILE, ENFILE) passes once some are freed.
        acceptFailures_.warn("an accept failed (" + ec.message() + "); accepting again in " +
                             std::to_string(ACCEPT_RETRY_DELAY.count()) + " ms");
        acceptRetry_.expires_after(ACCEPT_RETRY_DELAY);
        acceptRetry_.async_wait(beast::bind_front_handler(&WsServer::onAcceptRetry, shared_from_this()));
        return;
    }
    if (connections_->load(std::memory_order_relaxed) >= limits_.maxConnections)
    {
        // Over the limit: closed at once (#159)
        refusals_.warn("refused a connection: " + std::to_string(limits_.maxConnections) +
                       " clients are connected, the limit (--max-connections)");
        beast::error_code ignored;
        socket.close(ignored);
        doAccept();
        return;
    }

    connections_->fetch_add(1, std::memory_order_relaxed);
    auto session =
        std::make_shared<WsSession>(std::move(socket), sessionMgr_, wideData_, orderStorage_, inQueues_, idGen_,
                                    orderBook_, defaultClearingId_, defaultAccountId_, limits_, connections_, auth_);
    session->run();

    doAccept();
}

void WsServer::onAcceptRetry(beast::error_code ec)
{
    if (ec)
    {
        return; // cancelled: the server is stopping
    }
    doAccept();
}
