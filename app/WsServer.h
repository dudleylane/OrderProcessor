#pragma once

#include <memory>
#include <boost/beast/core.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "TypesDef.h"

namespace COP {

namespace Store {
    class WideParamsDataStorage;
    class OrderDataStorage;
}
class OrderBookImpl;
class IdTValueGenerator;

namespace Queues {
    class InQueues;
}

namespace App {

class SessionManager;

class WsServer : public std::enable_shared_from_this<WsServer> {
public:
    WsServer(
        boost::asio::io_context& ioc,
        boost::asio::ip::tcp::endpoint endpoint,
        SessionManager* sessionMgr,
        Store::WideParamsDataStorage* wideData,
        Store::OrderDataStorage* orderStorage,
        Queues::InQueues* inQueues,
        IdTValueGenerator* idGen,
        OrderBookImpl* orderBook);

    void run();

private:
    void doAccept();
    void onAccept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context& ioc_;
    SessionManager* sessionMgr_;
    Store::WideParamsDataStorage* wideData_;
    Store::OrderDataStorage* orderStorage_;
    Queues::InQueues* inQueues_;
    IdTValueGenerator* idGen_;
    OrderBookImpl* orderBook_;
};

}}
