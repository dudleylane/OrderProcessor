#ifdef BUILD_FIX

#include "FixOutQueues.h"
#include "FixGateway.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::App;
using namespace COP::Store;
using namespace COP::Queues;

FixOutQueues::FixOutQueues(FixGateway* gateway, OrderDataStorage* orderStorage)
    : gateway_(gateway)
    , orderStorage_(orderStorage)
{
}

void FixOutQueues::push(const ExecReportEvent& evnt, const std::string& /*target*/) {
    OrderEntry* order = orderStorage_->locateByOrderId(evnt.exec_->orderId_);
    if (!order) return;

    // FixGateway::sendExecutionReport checks sessionMap_ internally —
    // silently returns if this order didn't come from a FIX session
    gateway_->sendExecutionReport(evnt.exec_, *order);
}

void FixOutQueues::push(const CancelRejectEvent& evnt, const std::string& /*target*/) {
    OrderEntry* order = orderStorage_->locateByOrderId(evnt.id_);
    if (!order) return;

    const auto& clOrd = order->clOrderId_.get();
    std::string clOrdStr;
    if (clOrd.data_ && clOrd.length_ > 0)
        clOrdStr.assign(clOrd.data_, clOrd.length_);

    gateway_->sendCancelReject(evnt.id_, clOrdStr);
}

void FixOutQueues::push(const BusinessRejectEvent& evnt, const std::string& /*target*/) {
    // FIX BusinessMessageReject (35=j)
    // evnt.id_ references the order that triggered the business reject (or
    // invalid if no specific order). FixGateway resolves the target session
    // from the order's source string, or broadcasts if no reference.
    gateway_->sendBusinessReject(evnt.id_, "Business rule violation");
}

#endif // BUILD_FIX
