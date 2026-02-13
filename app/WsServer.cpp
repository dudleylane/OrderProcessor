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

WsServer::WsServer(
    net::io_context& ioc,
    tcp::endpoint endpoint,
    SessionManager* sessionMgr,
    Store::WideParamsDataStorage* wideData,
    Store::OrderDataStorage* orderStorage,
    Queues::InQueues* inQueues,
    IdTValueGenerator* idGen,
    OrderBookImpl* orderBook)
    : acceptor_(ioc)
    , ioc_(ioc)
    , sessionMgr_(sessionMgr)
    , wideData_(wideData)
    , orderStorage_(orderStorage)
    , inQueues_(inQueues)
    , idGen_(idGen)
    , orderBook_(orderBook)
{
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) throw std::runtime_error("WsServer open: " + ec.message());

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) throw std::runtime_error("WsServer reuse_address: " + ec.message());

    acceptor_.bind(endpoint, ec);
    if (ec) throw std::runtime_error("WsServer bind: " + ec.message());

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) throw std::runtime_error("WsServer listen: " + ec.message());
}

void WsServer::run() {
    doAccept();
}

void WsServer::doAccept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(&WsServer::onAccept, shared_from_this()));
}

void WsServer::onAccept(beast::error_code ec, tcp::socket socket) {
    if (ec) return;

    auto session = std::make_shared<WsSession>(
        std::move(socket),
        sessionMgr_,
        wideData_,
        orderStorage_,
        inQueues_,
        idGen_,
        orderBook_);
    session->run();

    doAccept();
}
