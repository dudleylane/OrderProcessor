#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <deque>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>

#include "TypesDef.h"
#include "ServerDefaults.h"
#include "WsAuth.h"

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
              const WsLimits &limits = WsLimits(), std::shared_ptr<std::atomic<std::size_t>> connections = nullptr,
              const WsAuth &auth = WsAuth());
    ~WsSession();

    void run();
    void send(const std::string &msg);
    /// Queues a message that other sessions may share: a broadcast is one copy, however many clients get it (#159)
    void send(std::shared_ptr<const std::string> msg);

    boost::asio::any_io_executor executor()
    {
        return ws_.get_executor();
    }
    /// The identity the session's token named (#225), or null without credentials, when the session may see and trade
    /// everything. Set before the session is added to its SessionManager, and never after, so other threads may read it
    /// under the manager's lock.
    const WsIdentity *identity() const
    {
        return identity_.get();
    }
    /// Gives a session that has no connection an identity, as an upgrade would: for the benchmarks, whose sessions never
    /// upgrade. Call it before the session is added to a SessionManager.
    void setIdentityForBenchmark(std::shared_ptr<const WsIdentity> identity)
    {
        identity_ = std::move(identity);
    }
    /// The source a session without an identity sends under, and answers to such sessions come back by
    static constexpr const char *ANONYMOUS_SOURCE = "WebSocket";

private:
    /// The upgrade request is read here, not by accept(), so that its token and Origin are checked first (#146)
    void onRequest(boost::beast::error_code ec, std::size_t bytesTransferred);
    /// Answers a request that may not open a session with an HTTP response, and ends the connection
    void refuse(boost::beast::http::status status, const std::string &reason);
    void onAccept(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytesTransferred);
    void handleMessage(const std::string &msg);
    /// The order a client names: by its ClOrdID when it gives one, or else by its number (#58). Null when no order has
    /// it. Also null, with answered set and an error already sent, when the number is ambiguous or the two disagree.
    OrderEntry *findOrder(u64 number, const std::string &clOrdId, bool *answered);
    /// The source a request goes to the engine under, and that its answers come back by: the identity's name, or
    /// ANONYMOUS_SOURCE (#225)
    std::string source() const;
    /// Whether the session may see an order of the account: always, without an identity (#225)
    bool maySee(const SourceIdT &account) const;
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytesTransferred);
    /// Ends the session of a client that doesn't read, once what waits for it passes the limit (#159)
    void drop();

    boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer buffer_;
    boost::beast::http::request<boost::beast::http::empty_body> upgradeRequest_;
    WsAuth auth_;
    std::shared_ptr<const WsIdentity> identity_;

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
