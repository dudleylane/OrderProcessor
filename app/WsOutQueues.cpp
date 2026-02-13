#include "WsOutQueues.h"
#include "SessionManager.h"
#include "JsonSerializer.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "Logger.h"

using namespace COP;
using namespace COP::App;

WsOutQueues::WsOutQueues(
    SessionManager* sessionMgr,
    Store::WideParamsDataStorage* wideData,
    Store::OrderDataStorage* orderStorage,
    OrderBookImpl* orderBook)
    : sessionMgr_(sessionMgr)
    , wideData_(wideData)
    , orderStorage_(orderStorage)
    , orderBook_(orderBook)
{
}

void WsOutQueues::push(const Queues::ExecReportEvent& evnt, const std::string& /*target*/) {
    // 1. Serialize and broadcast execution report
    sessionMgr_->broadcast(serializeExecReport(evnt.exec_));

    // 2. Look up and broadcast the updated order
    OrderEntry* order = orderStorage_->locateByOrderId(evnt.exec_->orderId_);
    if (order) {
        sessionMgr_->broadcast(serializeOrderUpdate(*order));

        // 3. Broadcast book update for subscribed sessions
        const std::string& symbol = order->instrument_.get().symbol_;
        SourceIdT instrId = order->instrument_.getId();
        BookSnapshot snap = orderBook_->getSnapshot(instrId, orderStorage_);
        std::string bookJson = serializeBookUpdate(symbol, snap);
        sessionMgr_->broadcastBookUpdate(symbol, bookJson);
    }
}

void WsOutQueues::push(const Queues::CancelRejectEvent& evnt, const std::string& /*target*/) {
    sessionMgr_->broadcast(serializeCancelReject(evnt.id_.id_, "Cancel rejected"));
}

void WsOutQueues::push(const Queues::BusinessRejectEvent& evnt, const std::string& /*target*/) {
    sessionMgr_->broadcast(serializeBusinessReject(evnt.id_.id_, "Business reject"));
}
