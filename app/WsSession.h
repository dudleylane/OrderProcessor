#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <deque>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>

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
struct OrderEntry;

namespace Queues
{
class InQueues;
}

namespace App
{

class SessionManager;

class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    /// defaultClearingId goes on every new order, and defaultAccountId on one that names no account. Either may be
    /// unset when the server could not resolve it at startup; the session then refuses the orders that need it (#34).
    WsSession(boost::asio::ip::tcp::socket &&socket, SessionManager *sessionMgr, Store::WideParamsDataStorage *wideData,
              Store::OrderDataStorage *orderStorage, Queues::InQueues *inQueues, IdTValueGenerator *idGen,
              OrderBookImpl *orderBook, SourceIdT defaultClearingId, SourceIdT defaultAccountId,
              const WsLimits &limits = WsLimits(), std::shared_ptr<std::atomic<std::size_t>> connections = nullptr);
    ~WsSession();

    void run();
    void send(const std::string &msg);
    /// Queues a message that other sessions may share: a broadcast is one copy, however many clients get it (#159)
    void send(std::shared_ptr<const std::string> msg);

    boost::asio::any_io_executor executor()
    {
        return ws_.get_executor();
    }

private:
    void onAccept(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytesTransferred);
    void handleMessage(const std::string &msg);
    /// The order a client names: by its ClOrdID when it gives one, or else by its number (#58). Null when no order has
    /// it. Also null, with answered set and an error already sent, when the number is ambiguous or the two disagree.
    OrderEntry *findOrder(u64 number, const std::string &clOrdId, bool *answered);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytesTransferred);
    /// Ends the session of a client that doesn't read, once what waits for it passes the limit (#159)
    void drop();

    boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer buffer_;

    SessionManager *sessionMgr_;
    Store::WideParamsDataStorage *wideData_;
    Store::OrderDataStorage *orderStorage_;
    Queues::InQueues *inQueues_;
    IdTValueGenerator *idGen_;
    OrderBookImpl *orderBook_;
    SourceIdT defaultClearingId_;
    SourceIdT defaultAccountId_;
    WsLimits limits_;
    /// Counts the server's sessions, for its connection limit; null for a session no WsServer made (#159)
    std::shared_ptr<std::atomic<std::size_t>> connections_;

    std::deque<std::shared_ptr<const std::string>> writeQueue_;
    std::size_t queuedBytes_ = 0; // what writeQueue_ holds, against limits_.maxQueuedBytes (#159)
    bool dropped_ = false;
};

} // namespace App
} // namespace COP
