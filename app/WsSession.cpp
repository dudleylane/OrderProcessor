#include "WsSession.h"
#include "SessionManager.h"
#include "JsonSerializer.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "IdTGenerator.h"
#include "QueuesDef.h"
#include "Logger.h"

#include <chrono>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace COP;
using namespace COP::App;

WsSession::WsSession(
    tcp::socket&& socket,
    SessionManager* sessionMgr,
    Store::WideParamsDataStorage* wideData,
    Store::OrderDataStorage* orderStorage,
    Queues::InQueues* inQueues,
    IdTValueGenerator* idGen,
    OrderBookImpl* orderBook)
    : ws_(std::move(socket))
    , sessionMgr_(sessionMgr)
    , wideData_(wideData)
    , orderStorage_(orderStorage)
    , inQueues_(inQueues)
    , idGen_(idGen)
    , orderBook_(orderBook)
{
}

void WsSession::run() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(boost::beast::http::field::server, "OrderProcessorServer");
        }));

    ws_.async_accept(
        beast::bind_front_handler(&WsSession::onAccept, shared_from_this()));
}

void WsSession::onAccept(beast::error_code ec) {
    if (ec) return;

    sessionMgr_->addSession(shared_from_this());

    // Send initial state
    send(serializeConnected());
    send(serializeInstrumentList(wideData_));
    send(serializeAccountList(wideData_));
    send(serializeOrderSnapshot(orderStorage_));

    doRead();
}

void WsSession::doRead() {
    ws_.async_read(
        buffer_,
        beast::bind_front_handler(&WsSession::onRead, shared_from_this()));
}

void WsSession::onRead(beast::error_code ec, std::size_t /*bytesTransferred*/) {
    if (ec) {
        sessionMgr_->removeSession(shared_from_this());
        return;
    }

    std::string msg = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    handleMessage(msg);
    doRead();
}

void WsSession::handleMessage(const std::string& msgStr) {
    auto msg = parseClientMessage(msgStr);

    if (msg.type == "new_order") {
        auto& no = msg.newOrder;

        // Look up instrument
        SourceIdT instrId = wideData_->findInstrumentBySymbol(no.symbol);
        if (!instrId.isValid()) {
            send(serializeError("Unknown instrument: " + no.symbol));
            return;
        }

        // Look up account (optional)
        SourceIdT acctId;
        if (!no.account.empty()) {
            acctId = wideData_->findAccountByName(no.account);
            if (!acctId.isValid()) {
                send(serializeError("Unknown account: " + no.account));
                return;
            }
        }

        // Create clOrderId RawDataEntry
        std::string clOrdStr = "WS-" + std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto* clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(),
                                          static_cast<u32>(clOrdStr.size()));
        SourceIdT clOrdId = Store::WideDataStorage::instance()->add(clOrdRaw);

        // Empty IDs for optional fields
        SourceIdT emptyId;
        SourceIdT clearingId;

        // Allocate execution list
        auto* execList = new ExecutionsT();
        SourceIdT execListId = Store::WideDataStorage::instance()->add(execList);

        // Create source string
        std::string srcStr = "WebSocket";
        auto* srcStrPtr = new StringT(srcStr);
        SourceIdT srcId = Store::WideDataStorage::instance()->add(srcStrPtr);

        // Create destination string
        auto* destStr = new StringT("Internal");
        SourceIdT destId = Store::WideDataStorage::instance()->add(destStr);

        auto* order = new OrderEntry(srcId, destId, clOrdId, emptyId,
                                     instrId, acctId, clearingId, execListId);
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
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        order->creationTime_ = static_cast<DateTimeT>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        Queues::OrderEvent evt(order);
        inQueues_->push("WebSocket", evt);

    } else if (msg.type == "cancel_order") {
        IdT orderId(msg.cancelOrder.orderId, 1);
        Queues::OrderCancelEvent evt(orderId, "Canceled by user");
        inQueues_->push("WebSocket", evt);

    } else if (msg.type == "replace_order") {
        auto& ro = msg.replaceOrder;
        IdT origOrderId(ro.orderId, 1);

        // Look up existing order to clone
        OrderEntry* existing = orderStorage_->locateByOrderId(origOrderId);
        if (!existing) {
            send(serializeError("Order not found for replace: " + std::to_string(ro.orderId)));
            return;
        }

        OrderEntry* replacement = existing->clone();
        if (ro.hasPrice) replacement->price_ = ro.price;
        if (ro.hasQty) {
            replacement->orderQty_ = ro.orderQty;
            replacement->leavesQty_ = ro.orderQty;
        }
        if (ro.hasTif) replacement->tif_ = ro.tif;

        Queues::OrderReplaceEvent evt(origOrderId, replacement);
        inQueues_->push("WebSocket", evt);

    } else if (msg.type == "subscribe_book") {
        bookSubscriptions_.insert(msg.symbol);

        // Send initial snapshot
        SourceIdT instrId = wideData_->findInstrumentBySymbol(msg.symbol);
        if (instrId.isValid()) {
            BookSnapshot snap = orderBook_->getSnapshot(instrId, orderStorage_);
            send(serializeBookUpdate(msg.symbol, snap));
        }

    } else if (msg.type == "unsubscribe_book") {
        bookSubscriptions_.erase(msg.symbol);

    } else {
        send(serializeError("Unknown message type: " + msg.type));
    }
}

void WsSession::send(const std::string& msg) {
    writeQueue_.push_back(msg);
    if (writeQueue_.size() == 1)
        doWrite();
}

void WsSession::doWrite() {
    if (writeQueue_.empty()) return;

    ws_.text(true);
    ws_.async_write(
        net::buffer(writeQueue_.front()),
        beast::bind_front_handler(&WsSession::onWrite, shared_from_this()));
}

void WsSession::onWrite(beast::error_code ec, std::size_t /*bytesTransferred*/) {
    if (ec) {
        sessionMgr_->removeSession(shared_from_this());
        return;
    }

    writeQueue_.pop_front();
    if (!writeQueue_.empty())
        doWrite();
}

bool WsSession::isSubscribedTo(const std::string& symbol) const {
    return bookSubscriptions_.count(symbol) > 0;
}
