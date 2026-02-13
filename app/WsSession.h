#pragma once

#include <memory>
#include <string>
#include <deque>
#include <set>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>

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

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(
        boost::asio::ip::tcp::socket&& socket,
        SessionManager* sessionMgr,
        Store::WideParamsDataStorage* wideData,
        Store::OrderDataStorage* orderStorage,
        Queues::InQueues* inQueues,
        IdTValueGenerator* idGen,
        OrderBookImpl* orderBook);

    void run();
    void send(const std::string& msg);

    boost::asio::any_io_executor executor() { return ws_.get_executor(); }
    bool isSubscribedTo(const std::string& symbol) const;

private:
    void onAccept(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytesTransferred);
    void handleMessage(const std::string& msg);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytesTransferred);

    boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer buffer_;

    SessionManager* sessionMgr_;
    Store::WideParamsDataStorage* wideData_;
    Store::OrderDataStorage* orderStorage_;
    Queues::InQueues* inQueues_;
    IdTValueGenerator* idGen_;
    OrderBookImpl* orderBook_;

    std::deque<std::string> writeQueue_;
    std::set<std::string> bookSubscriptions_;
};

}}
