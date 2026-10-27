#include "WsOutQueues.h"
#include "SessionManager.h"
#include "JsonSerializer.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "Logger.h"
#include "ExchUtils.h"

using namespace COP;
using namespace COP::App;

WsOutQueues::WsOutQueues(SessionManager *sessionMgr, Store::WideParamsDataStorage *wideData,
                         Store::OrderDataStorage *orderStorage, OrderBookImpl *orderBook)
    : sessionMgr_(sessionMgr), wideData_(wideData), orderStorage_(orderStorage), orderBook_(orderBook)
{
}

void WsOutQueues::push(const Queues::ExecReportEvent &evnt, const std::string &target)
{
    // The order first: its account decides who may see the report (#225). One the storage doesn't have goes only to
    // the source it came from; never to every session, which would show it to another identity's.
    OrderEntry *order = orderStorage_->locateByOrderId(evnt.exec_->orderId_);
    if (nullptr == order) [[unlikely]]
    {
        sessionMgr_->sendToSource(target, serializeExecReport(evnt.exec_));
        return;
    }
    // 1. The execution report, and 2. the order as the report says it is, to the sessions that may see its account. A
    // later fill of the order can be applied before a fill's report goes out, so the order's own figures can count it
    // (#411). A report built without figures, as a test may build one, gives the order as it is.
    std::optional<OrderFigures> figures = evnt.figures_;
    if (!figures)
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
        figures.emplace(*order);
    }
    const SourceIdT account = order->account_.getId();
    sessionMgr_->broadcastToAccount(account, serializeExecReport(evnt.exec_));
    sessionMgr_->broadcastToAccount(account, serializeOrderUpdate(*order, *figures));

    // 3. The book, to its subscribers, whatever their accounts: it shows no account (#225)
    const std::string &symbol = order->instrument_.get().symbol_;
    SourceIdT instrId = order->instrument_.getId();
    BookSnapshot snap = orderBook_->getSnapshot(instrId, orderStorage_);
    std::string bookJson = serializeBookUpdate(symbol, snap);
    sessionMgr_->broadcastBookUpdate(symbol, bookJson);
}

void WsOutQueues::push(const Queues::CancelRejectEvent &evnt, const std::string &target)
{
    // To the sessions that may see the order, or, for an order no session may name, to the requester (#225)
    const std::string reject = serializeCancelReject(evnt.id_.id_, cancelRejectReason(evnt));
    const OrderEntry *order = evnt.id_.isValid() ? orderStorage_->locateByOrderId(evnt.id_) : nullptr;
    if (nullptr != order)
    {
        sessionMgr_->broadcastToAccount(order->account_.getId(), reject);
    }
    else
    {
        sessionMgr_->sendToSource(target, reject);
    }
}

void WsOutQueues::push(const Queues::BusinessRejectEvent &evnt, const std::string &target)
{
    sessionMgr_->sendToSource(target, serializeBusinessReject(evnt.id_.id_, "Business reject"));
}

void WsOutQueues::push(const Queues::OrderRejectEvent &evnt, const std::string &target)
{
    // The order was never stored, so it has no id: a REJECT report with orderId 0, and the reason (#67)
    RejectExecEntry report;
    report.type_ = REJECT_EXECTYPE;
    report.orderStatus_ = REJECTED_ORDSTATUS;
    report.transactTime_ = aux::currentDateTime();
    report.market_ = INTERNAL_EXECUTION;
    report.rejectReason_ = evnt.reason_;
    // To whoever sent it: nothing was stored, and the report names no order (#225)
    sessionMgr_->sendToSource(target, serializeExecReport(&report));
}
