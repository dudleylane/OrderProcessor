#include "WsOutQueues.h"
#include "SessionManager.h"
#include "JsonSerializer.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "Logger.h"
#include "ExchUtils.h"

using namespace COP;
using namespace COP::App;

WsOutQueues::WsOutQueues(boost::asio::io_context &ioc, SessionManager *sessionMgr,
                         Store::WideParamsDataStorage *wideData, Store::OrderDataStorage *orderStorage,
                         OrderBookImpl *orderBook, std::chrono::milliseconds bookInterval)
    : sessionMgr_(sessionMgr), wideData_(wideData), orderStorage_(orderStorage),
      books_(BookPublisher::create(ioc, sessionMgr, orderBook, orderStorage, bookInterval))
{
}

void WsOutQueues::stop()
{
    books_->stop();
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

    // 3. The book, to its subscribers, whatever their accounts: it shows no account (#225). Marked only when a session
    // subscribes to it: every report walked the instrument's book, and the broadcast then dropped it unless one did
    // (#525). The IO thread builds and sends it, at most once an interval, where every report built it here, walking
    // the whole book (#694). The report's figures are published, and every change it reports is in the book, before
    // this push, so a build after it shows them. A session that subscribes meanwhile gets the book as it subscribes.
    const std::string &symbol = order->instrument_.get().symbol_;
    if (sessionMgr_->hasBookSubscribers(symbol))
    {
        books_->changed(order->instrument_.getId(), symbol);
    }
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

void WsOutQueues::push(const Queues::OrderRejectEvent &evnt, const std::string &target)
{
    // The order was never stored, so it has no id: a REJECT report with orderId 0, and the reason (#67), its own
    // execution id and the ClOrdIDs (#545), and the request's id (#683). Its time is the clock's: it has no stored
    // order whose decision it reports (#546).
    // To whoever sent it: nothing was stored, and the report names no order (#225)
    sessionMgr_->sendToSource(target, serializeUnstoredReject(evnt, aux::currentDateTime()));
}
