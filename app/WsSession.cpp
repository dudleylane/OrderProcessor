#include "WsSession.h"
#include "SessionManager.h"
#include "JsonSerializer.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "IdTGenerator.h"
#include "QueuesDef.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <exception>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace COP;
using namespace COP::App;

namespace
{
/// Exceptions contained at the session boundary since the process started. Each is logged with its number,
/// so the count stays visible in the log without a protocol field.
std::atomic<u64> containedExceptions{ 0 };

/// Numbers the ClOrdIDs this server makes up for WebSocket orders, so that two in the same microsecond differ (#67)
std::atomic<u64> clOrderSequence{ 0 };

/// A ClOrdID for an order or replacement the server makes up. The client did not choose it, so it must not collide
/// with one already in use, which is refused (#67).
SourceIdT newClOrderId()
{
    std::string clOrdStr = "WS-" +
                           std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count()) +
                           "-" + std::to_string(clOrderSequence.fetch_add(1, std::memory_order_relaxed) + 1);
    auto *clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size()));
    return Store::WideDataStorage::instance()->add(clOrdRaw);
}

DateTimeT nowMillis()
{
    return static_cast<DateTimeT>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// Longest prefix of a client's request that goes into the log.
const size_t MAX_LOGGED_REQUEST = 256;

void logContained(const char *where, const std::string &request, const char *what)
{
    const u64 n = containedExceptions.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string text = "WsSession: contained exception #" + std::to_string(n) + " in " + where + ": " + what;
    if (!request.empty())
    {
        text += "; request: " + request.substr(0, MAX_LOGGED_REQUEST);
    }
    aux::ExchLogger::instance()->error(text);
}
} // namespace

WsSession::WsSession(tcp::socket &&socket, SessionManager *sessionMgr, Store::WideParamsDataStorage *wideData,
                     Store::OrderDataStorage *orderStorage, Queues::InQueues *inQueues, IdTValueGenerator *idGen,
                     OrderBookImpl *orderBook, SourceIdT defaultClearingId, SourceIdT defaultAccountId,
                     const WsLimits &limits, std::shared_ptr<std::atomic<std::size_t>> connections, const WsAuth &auth)
    : ws_(std::move(socket)), auth_(auth), sessionMgr_(sessionMgr), wideData_(wideData), orderStorage_(orderStorage),
      inQueues_(inQueues), idGen_(idGen), orderBook_(orderBook), defaultClearingId_(defaultClearingId),
      defaultAccountId_(defaultAccountId), limits_(limits), connections_(std::move(connections))
{
}

WsSession::~WsSession()
{
    if (connections_)
    {
        connections_->fetch_sub(1, std::memory_order_relaxed);
    }
}

void WsSession::run()
{
    // A client that connects and sends nothing is dropped, as the WebSocket's own timeouts drop one later
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    http::async_read(ws_.next_layer(), buffer_, upgradeRequest_,
                     beast::bind_front_handler(&WsSession::onRequest, shared_from_this()));
}

void WsSession::onRequest(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        return;
    }
    // A plain HTTP request, such as the image's health check, gets an answer rather than a dropped connection
    if (!websocket::is_upgrade(upgradeRequest_))
    {
        refuse(http::status::upgrade_required, "This is a WebSocket server: open a WebSocket on /ws.");
        return;
    }
    // A client may send the header more than once; a browser sends one, comma-separated
    std::string subprotocols;
    const auto fields = upgradeRequest_.equal_range(http::field::sec_websocket_protocol);
    for (auto field = fields.first; field != fields.second; ++field)
    {
        subprotocols.append(subprotocols.empty() ? "" : ",").append(field->value());
    }
    switch (decideUpgrade(auth_, subprotocols, upgradeRequest_[http::field::origin], &identity_))
    {
    case WsUpgrade::ForbiddenOrigin:
        refuse(http::status::forbidden, "This server doesn't accept WebSocket connections from that Origin.");
        return;
    case WsUpgrade::MissingToken:
    case WsUpgrade::WrongToken:
        refuse(http::status::unauthorized, "A WebSocket connection to this server needs its access token.");
        return;
    case WsUpgrade::Accept:
        break;
    }

    beast::get_lowest_layer(ws_).expires_never();
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    // A larger message fails the read, and the session ends: Beast's default let a client send 16 MiB (#159)
    ws_.read_message_max(limits_.maxMessageBytes);
    const bool answerSubprotocol = offersSubprotocol(subprotocols);
    ws_.set_option(websocket::stream_base::decorator(
        [answerSubprotocol](websocket::response_type &res)
        {
            res.set(http::field::server, "OrderProcessorServer");
            // Never the token's entry: the answer names one of the client's offers, and only ours
            if (answerSubprotocol)
            {
                res.set(http::field::sec_websocket_protocol, std::string(WS_SUBPROTOCOL));
            }
        }));
    ws_.async_accept(upgradeRequest_, beast::bind_front_handler(&WsSession::onAccept, shared_from_this()));
}

void WsSession::refuse(http::status status, const std::string &reason)
{
    auto response = std::make_shared<http::response<http::string_body>>(status, upgradeRequest_.version());
    response->set(http::field::server, "OrderProcessorServer");
    response->set(http::field::content_type, "text/plain");
    response->keep_alive(false);
    response->body() = reason + "\n";
    response->prepare_payload();
    http::async_write(ws_.next_layer(), *response,
                      [self = shared_from_this(), response](beast::error_code, std::size_t)
                      {
                          beast::error_code ignored;
                          beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_send, ignored);
                      });
}

void WsSession::onAccept(beast::error_code ec)
{
    if (ec)
    {
        return;
    }

    sessionMgr_->addSession(shared_from_this());

    // Send initial state. Handlers run inside io_context::run(), so an exception leaving one terminates the
    // server (#36): contain it, tell the client, and keep the session open.
    try
    {
        // Under an identity, only its own accounts and their orders (#225)
        const AccountFilter maySeeAccount = [this](const SourceIdT &account)
        {
            return maySee(account);
        };
        send(serializeConnected());
        send(serializeInstrumentList(wideData_));
        send(serializeAccountList(wideData_, maySeeAccount));
        send(serializeOrderSnapshot(orderStorage_, maySeeAccount));
    }
    catch (const std::exception &ex)
    {
        logContained("initial state", std::string(), ex.what());
        send(serializeError("Internal error: initial state is incomplete"));
    }
    catch (...)
    {
        logContained("initial state", std::string(), "unknown exception");
        send(serializeError("Internal error: initial state is incomplete"));
    }

    doRead();
}

void WsSession::doRead()
{
    ws_.async_read(buffer_, beast::bind_front_handler(&WsSession::onRead, shared_from_this()));
}

void WsSession::onRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        sessionMgr_->removeSession(shared_from_this());
        return;
    }

    std::string msg = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    // One client's request must not take the server down (#36). Without this, an exception would leave
    // io_context::run() and terminate the process, and this session would stop reading.
    try
    {
        handleMessage(msg);
    }
    catch (const std::exception &ex)
    {
        logContained("request", msg, ex.what());
        send(serializeError("Internal error: request not processed"));
    }
    catch (...)
    {
        logContained("request", msg, "unknown exception");
        send(serializeError("Internal error: request not processed"));
    }
    doRead();
}

void WsSession::handleMessage(const std::string &msgStr)
{
    auto msg = parseClientMessage(msgStr);

    if (msg.type == "error")
    {
        send(serializeError("Invalid message: " + msg.error));
        return;
    }

    if (msg.type == "new_order")
    {
        auto &no = msg.newOrder;

        // A side, type, time in force, currency or capacity that is missing, or isn't one of the protocol's values, parses
        // to INVALID (ClientMessageParser). No such order can be valid (OrderEntry::isValid). Stored to be rejected, it
        // reached every client with "INVALID" in its fields, a value none of their types has (#166). Refuse it before
        // anything is stored.
        const char *unknown = nullptr;
        if (INVALID_SIDE == no.side)
        {
            unknown = "side";
        }
        else if (INVALID_ORDERTYPE == no.ordType)
        {
            unknown = "ordType";
        }
        else if (INVALID_TIF == no.tif)
        {
            unknown = "tif";
        }
        else if (INVALID_CURRENCY == no.currency)
        {
            unknown = "currency";
        }
        else if (INVALID_CAPACITY == no.capacity)
        {
            unknown = "capacity";
        }
        if (nullptr != unknown)
        {
            send(serializeError(std::string("Order refused: ") + unknown +
                                " is missing, or isn't one the server knows"));
            return;
        }

        // Look up instrument
        SourceIdT instrId = wideData_->findInstrumentBySymbol(no.symbol);
        if (!instrId.isValid())
        {
            send(serializeError("Unknown instrument: " + no.symbol));
            return;
        }

        // Look up the account. Under an identity it must be one of the identity's, and an order that names none trades
        // the first (#225), as a FIX counterparty's does (#147). The refusal doesn't say whether the account exists.
        SourceIdT acctId = defaultAccountId_;
        if (identity_)
        {
            acctId = no.account.empty() ? identity_->defaultAccount : wideData_->findAccountByName(no.account);
            if (!acctId.isValid() || !identity_->mayUse(acctId))
            {
                send(serializeError("Order refused: account " + no.account + " isn't one this connection may trade"));
                return;
            }
        }
        // Otherwise an order that names none goes to the server's default account (#34)
        else if (!no.account.empty())
        {
            acctId = wideData_->findAccountByName(no.account);
            if (!acctId.isValid())
            {
                send(serializeError("Unknown account: " + no.account));
                return;
            }
        }
        else if (!acctId.isValid())
        {
            send(serializeError("Order refused: it names no account, and the server has no default account"));
            return;
        }

        // Every order clears through the server's clearing firm (#34)
        if (!defaultClearingId_.isValid())
        {
            send(serializeError("Order refused: the server has no clearing firm"));
            return;
        }

        SourceIdT clOrdId = newClOrderId();

        // Empty ID for the unset origClOrderId
        SourceIdT emptyId;

        // Allocate execution list
        auto *execList = new ExecutionsT();
        SourceIdT execListId = Store::WideDataStorage::instance()->add(execList);

        // The source: the identity that sent it, or "WebSocket" without one (#225)
        const std::string srcStr = source();
        auto *srcStrPtr = new StringT(srcStr);
        SourceIdT srcId = Store::WideDataStorage::instance()->add(srcStrPtr);

        // Create destination string
        auto *destStr = new StringT("Internal");
        SourceIdT destId = Store::WideDataStorage::instance()->add(destStr);

        auto *order = new OrderEntry(srcId, destId, clOrdId, emptyId, instrId, acctId, defaultClearingId_, execListId);
        order->side_ = no.side;
        order->ordType_ = no.ordType;
        order->price_ = no.price;
        order->stopPx_ = no.stopPx;
        order->orderQty_ = no.orderQty;
        order->leavesQty_ = no.orderQty;
        order->minQty_ = no.minQty;
        order->tif_ = no.tif;
        order->currency_ = no.currency;
        order->capacity_ = no.capacity;
        // The protocol has no settlement type, and an order without one settles regular, as in FIX (#34)
        order->settlType_ = _0_SETTLTYPE;
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        order->creationTime_ = nowMillis();
        order->lastUpdateTime_ = order->creationTime_;

        Queues::OrderEvent evt(order);
        inQueues_->push(srcStr, evt);
    }
    else if (msg.type == "new_swap_order")
    {
        auto &so = msg.swapOrder;

        // As for new_order (#166): a side, currency or capacity that is missing, or a side, time in force, currency or
        // capacity that isn't one of the protocol's values, parses to INVALID. Refuse it before anything is stored. A swap
        // that gives no time in force is GTC (ClientMessageParser).
        const char *unknown = nullptr;
        if (INVALID_SIDE == so.side)
        {
            unknown = "side";
        }
        else if (INVALID_TIF == so.tif)
        {
            unknown = "tif";
        }
        else if (INVALID_CURRENCY == so.currency)
        {
            unknown = "currency";
        }
        else if (INVALID_CAPACITY == so.capacity)
        {
            unknown = "capacity";
        }
        if (nullptr != unknown)
        {
            send(serializeError(std::string("Order refused: ") + unknown +
                                " is missing, or isn't one the server knows"));
            return;
        }

        SourceIdT instrId = wideData_->findInstrumentBySymbol(so.symbol);
        if (!instrId.isValid())
        {
            send(serializeError("Unknown instrument: " + so.symbol));
            return;
        }

        // As for new_order: under an identity the account must be one of the identity's, and a swap that names none
        // trades the first (#225); otherwise the server's default account and clearing firm (#34)
        SourceIdT acctId = defaultAccountId_;
        if (identity_)
        {
            acctId = so.account.empty() ? identity_->defaultAccount : wideData_->findAccountByName(so.account);
            if (!acctId.isValid() || !identity_->mayUse(acctId))
            {
                send(serializeError("Order refused: account " + so.account + " isn't one this connection may trade"));
                return;
            }
        }
        else if (!so.account.empty())
        {
            acctId = wideData_->findAccountByName(so.account);
            if (!acctId.isValid())
            {
                send(serializeError("Unknown account: " + so.account));
                return;
            }
        }
        else if (!acctId.isValid())
        {
            send(serializeError("Order refused: it names no account, and the server has no default account"));
            return;
        }
        if (!defaultClearingId_.isValid())
        {
            send(serializeError("Order refused: the server has no clearing firm"));
            return;
        }

        // As for new_order: a sequence number keeps ClOrdIDs made in the same microsecond apart (#67)
        std::string clOrdStr = "WS-" +
                               std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::system_clock::now().time_since_epoch())
                                                  .count()) +
                               "-" + std::to_string(clOrderSequence.fetch_add(1, std::memory_order_relaxed) + 1);
        auto *clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size()));
        SourceIdT clOrdId = Store::WideDataStorage::instance()->add(clOrdRaw);

        SourceIdT emptyId;

        auto *execList = new ExecutionsT();
        SourceIdT execListId = Store::WideDataStorage::instance()->add(execList);

        // Its identity's name, or WebSocket without one, as for new_order: its reports go by it (#225)
        const std::string srcStr = source();
        auto *srcStrPtr = new StringT(srcStr);
        SourceIdT srcId = Store::WideDataStorage::instance()->add(srcStrPtr);

        auto *destStr = new StringT("Internal");
        SourceIdT destId = Store::WideDataStorage::instance()->add(destStr);

        auto *order = new OrderEntry(srcId, destId, clOrdId, emptyId, instrId, acctId, defaultClearingId_, execListId);
        order->side_ = so.side;
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->price_ = so.nearPrice;
        order->farPrice_ = so.farPrice;
        order->orderQty_ = so.orderQty;
        order->leavesQty_ = so.orderQty;
        order->tif_ = so.tif;
        order->currency_ = so.currency;
        order->capacity_ = so.capacity;
        order->settlDate_ = so.settlDate;
        order->farSettlDate_ = so.farSettlDate;
        order->settlType_ = _2_SETTLTYPE;
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        order->creationTime_ = static_cast<DateTimeT>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        order->lastUpdateTime_ = order->creationTime_;

        Queues::OrderEvent evt(order);
        inQueues_->push(srcStr, evt);
    }
    else if (msg.type == "cancel_order")
    {
        // An order the client names that doesn't exist still goes to the engine, under an invalid id that keeps the
        // number, so the client gets a cancel_reject naming it (#58)
        const u64 number = msg.cancelOrder.orderId;
        bool answered = false;
        OrderEntry *order = findOrder(number, msg.cancelOrder.clOrderId, &answered);
        if (answered)
        {
            return;
        }
        Queues::OrderCancelEvent evt((nullptr != order) ? order->orderId_ : IdT(number, 0), "Canceled by user");
        inQueues_->push(source(), evt);
    }
    else if (msg.type == "replace_order")
    {
        const auto &ro = msg.replaceOrder;
        if (!ro.hasPrice && !ro.hasQty && !ro.hasTif)
        {
            send(serializeError("replace_order changes nothing: give price, orderQty or tif"));
            return;
        }
        // As for a new order (#166): a tif the server doesn't know would reach every client as "INVALID"
        if (ro.hasTif && (INVALID_TIF == ro.tif))
        {
            send(serializeError("Replace refused: tif isn't one the server knows"));
            return;
        }
        bool answered = false;
        OrderEntry *original = findOrder(ro.orderId, ro.clOrderId, &answered);
        if (answered)
        {
            return;
        }
        if (nullptr == original)
        {
            send(serializeError("Order not found for replace: " + std::to_string(ro.orderId)));
            return;
        }

        // The replacement is the original with the client's changes, a fresh id and a ClOrdID of its own (#74). The
        // engine decides it, and fills in what the original has done.
        std::unique_ptr<OrderEntry> replacement;
        {
            // read lock: a transaction worker may be updating the original
            oneapi::tbb::spin_rw_mutex::scoped_lock lock(original->entryMutex_, false);
            replacement.reset(original->clone());
        }
        replacement->orderId_ = IdT();
        replacement->clOrderId_ = newClOrderId();
        replacement->origClOrderId_ = original->clOrderId_;
        if (ro.hasPrice)
        {
            replacement->price_ = ro.price;
        }
        if (ro.hasQty)
        {
            replacement->orderQty_ = ro.orderQty;
        }
        if (ro.hasTif)
        {
            replacement->tif_ = ro.tif;
        }
        replacement->status_ = RECEIVEDNEW_ORDSTATUS;
        replacement->lastUpdateTime_ = nowMillis();

        Queues::OrderReplaceEvent evt(original->orderId_, replacement.release());
        inQueues_->push(source(), evt);
    }
    else if (msg.type == "subscribe_book")
    {
        // Only a symbol the server trades: any string was kept, so a client could grow the set without limit (#159)
        SourceIdT instrId = wideData_->findInstrumentBySymbol(msg.symbol);
        if (!instrId.isValid())
        {
            send(serializeError("Unknown instrument: " + msg.symbol));
            return;
        }
        sessionMgr_->subscribe(shared_from_this(), msg.symbol);

        // Send initial snapshot
        BookSnapshot snap = orderBook_->getSnapshot(instrId, orderStorage_);
        send(serializeBookUpdate(msg.symbol, snap));
    }
    else if (msg.type == "unsubscribe_book")
    {
        sessionMgr_->unsubscribe(shared_from_this(), msg.symbol);
    }
    else
    {
        send(serializeError("Unknown message type: " + msg.type));
    }
}

void WsSession::send(const std::string &msg)
{
    send(std::make_shared<const std::string>(msg));
}

void WsSession::send(std::shared_ptr<const std::string> msg)
{
    if (dropped_)
    {
        return;
    }
    // A client that doesn't read made the server keep everything sent to it, without limit (#159). A message is always
    // taken when nothing waits: the order snapshot a client gets when it connects can be larger than the limit.
    if (!writeQueue_.empty() && (queuedBytes_ + msg->size() > limits_.maxQueuedBytes))
    {
        drop();
        return;
    }
    queuedBytes_ += msg->size();
    writeQueue_.push_back(std::move(msg));
    if (writeQueue_.size() == 1)
    {
        doWrite();
    }
}

void WsSession::drop()
{
    dropped_ = true;
    beast::error_code ec;
    const tcp::endpoint peer = beast::get_lowest_layer(ws_).socket().remote_endpoint(ec);
    aux::ExchLogger::instance()->warn(
        "WsSession: dropped the client at " +
        (ec ? std::string("an unknown address") : peer.address().to_string() + ":" + std::to_string(peer.port())) +
        ", which isn't reading: " + std::to_string(queuedBytes_) + " bytes were waiting for it, and the limit is " +
        std::to_string(limits_.maxQueuedBytes) + " (--max-session-queue-mb)");
    sessionMgr_->removeSession(shared_from_this());
    // The write in progress still uses the first message; the rest go now
    if (writeQueue_.size() > 1)
    {
        writeQueue_.erase(writeQueue_.begin() + 1, writeQueue_.end());
    }
    queuedBytes_ = writeQueue_.empty() ? 0 : writeQueue_.front()->size();
    // Ends the read and the write in progress, which then fail and let the session go
    beast::get_lowest_layer(ws_).close();
}

std::string WsSession::source() const
{
    return identity_ ? identity_->name : std::string(ANONYMOUS_SOURCE);
}

bool WsSession::maySee(const SourceIdT &account) const
{
    return !identity_ || identity_->mayUse(account);
}

OrderEntry *WsSession::findOrder(u64 number, const std::string &clOrdId, bool *answered)
{
    *answered = false;
    // Another identity's order is one this session can't name (#225): as if there were none, so the client learns
    // nothing of it, and a cancel of it reaches the engine as an unknown order. Its account never changes, so it is read
    // without the order's lock.
    const auto visible = [this](OrderEntry *order)
    {
        return ((nullptr != order) && maySee(order->account_.getId())) ? order : nullptr;
    };
    if (!clOrdId.empty())
    {
        RawDataEntry key(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size()));
        OrderEntry *order = visible(orderStorage_->locateByClOrderId(key));
        if ((nullptr != order) && (number != order->orderId_.id_))
        {
            send(serializeError("Order " + std::to_string(number) + " does not have ClOrdID " + clOrdId));
            *answered = true;
            return nullptr;
        }
        return order;
    }
    bool ambiguous = false;
    OrderEntry *order = orderStorage_->locateByOrderNumber(number, &ambiguous);
    if (ambiguous)
    {
        send(serializeError("More than one order has number " + std::to_string(number) +
                            ": name it by clOrderId as well"));
        *answered = true;
    }
    return visible(order);
}

void WsSession::doWrite()
{
    if (writeQueue_.empty())
    {
        return;
    }

    ws_.text(true);
    ws_.async_write(net::buffer(*writeQueue_.front()),
                    beast::bind_front_handler(&WsSession::onWrite, shared_from_this()));
}

void WsSession::onWrite(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        sessionMgr_->removeSession(shared_from_this());
        return;
    }

    queuedBytes_ -= writeQueue_.front()->size();
    writeQueue_.pop_front();
    if (!writeQueue_.empty() && !dropped_)
    {
        doWrite();
    }
}
