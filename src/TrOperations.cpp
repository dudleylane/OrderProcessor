/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "TrOperations.h"
#include "QueuesDef.h"
#include "OrderMatcher.h"
#include "OrderStorage.h"
#include "ExchUtils.h"
#include "DeferedEvents.h"

using namespace std;
using namespace COP;
using namespace COP::ACID;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Store;

namespace COP
{
namespace ACID
{

void processEvent_GenerateExecution(const Context &cnxt, const IdT &orderId, const ExecutionEntry &exec,
                                    const std::string &requestClOrdId = std::string())
{
    /// locate order and push event into the executions_
    /// find source of the order and push ExecReport to the source
    assert(nullptr != cnxt.orderStorage_);
    OrderEntry *ord = cnxt.orderStorage_->locateByOrderId(orderId);
    assert(nullptr != ord);

    // write lock on the order during source read + addExecution
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, true);

    StringT src = ord->source_.get();

    ExecutionEntry *execReport = cnxt.orderStorage_->save(exec, cnxt.idGenerator_);
    assert(nullptr != execReport);

    ord->addExecution(execReport->execId_);

    ordLock.release();

    assert(nullptr != cnxt.outQueues_);
    ExecReportEvent report(execReport);
    report.requestClOrdId_ = requestClOrdId;
    cnxt.outQueues_->push(report, src);
}

void executeEnqueueOrderEvent(const OrdState::onReplaceReceived &event_, const Context &cnxt)
{
    assert(nullptr != cnxt.inQueues_);
    cnxt.inQueues_->push("_", ProcessEvent(ProcessEvent::ON_REPLACE_RECEVIED, event_.replId_));

    ReplaceExecEntry execReport;
    execReport.origOrderId_ = event_.replId_;
    execReport.orderId_ = event_.orderId_;
    execReport.type_ = PEND_REPLACE_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = PENDINGREPLACE_ORDSTATUS;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, event_.orderId_, execReport);
}

void rollbackEnqueueOrderEvent(const OrdState::onReplaceReceived &, const Context &) {}

void executeEnqueueOrderEvent(const OrdState::onExecReplace &event_, const Context &cnxt)
{
    assert(nullptr != cnxt.inQueues_);
    cnxt.inQueues_->push("_", ProcessEvent(ProcessEvent::ON_EXEC_REPLACE, event_.replId_));

    ReplaceExecEntry execReport;
    execReport.origOrderId_ = event_.replId_;
    execReport.orderId_ = event_.orderId_;
    execReport.type_ = REPLACE_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = REPLACED_ORDSTATUS;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, event_.orderId_, execReport);
}

void rollbackEnqueueOrderEvent(const OrdState::onExecReplace &, const Context &) {}

void executeEnqueueOrderEvent(const OrdState::onReplaceRejected &event_, const Context &cnxt)
{
    assert(nullptr != cnxt.inQueues_);
    cnxt.inQueues_->push("_", ProcessEvent(ProcessEvent::ON_REPLACE_REJECTED));

    RejectExecEntry execReport;
    execReport.orderId_ = event_.orderId_;
    execReport.type_ = REPLACE_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = REJECTED_ORDSTATUS;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, event_.orderId_, execReport);
}

void rollbackEnqueueOrderEvent(const OrdState::onReplaceRejected &, const Context &) {}

} // namespace ACID
} // namespace COP

CreateExecReportTrOperation::CreateExecReportTrOperation(OrderStatus status, ExecType execType, const OrderEntry &order)
    : Operation(CREATE_EXECREPORT_TROPERATION, order.orderId_), status_(status), execType_(execType)
{
}

CreateExecReportTrOperation::~CreateExecReportTrOperation() {}

void CreateExecReportTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    /// locate source of the order,
    /// prepare ExecReport and send to the source of the order
    switch (execType_)
    {
    case NEW_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case DFD_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT();        /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_; //DFD_ORDSTATUS;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case CORRECT_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case CANCEL_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case EXPIRED_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT();        /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_; //EXPIRED_ORDSTATUS;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case SUSPENDED_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT();        /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_; //SUSPENDED_ORDSTATUS;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case STATUS_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case RESTATED_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case PEND_CANCEL_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case PEND_REPLACE_EXECTYPE:
    {
        ExecutionEntry execReport;
        execReport.orderId_ = getObjectId();
        execReport.type_ = execType_;
        execReport.transactTime_ = aux::currentDateTime();
        execReport.execId_ = IdT(); /// will be assigned when saved into the storage
        execReport.orderStatus_ = status_;
        execReport.market_ = INTERNAL_EXECUTION;
        processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
    }
    break;
    case TRADE_EXECTYPE:
    case REJECT_EXECTYPE:
    case REPLACE_EXECTYPE:
    default:
        assert(false);
        throw std::runtime_error("ExecutionType not supported by the CreateExecReportTrOperation!");
    };
}

void CreateExecReportTrOperation::rollback(const Context &) {}

CreateCancelExecReportTrOperation::CreateCancelExecReportTrOperation(const OrderEntry &order,
                                                                     const std::string &requestClOrdId)
    : Operation(CREATE_EXECREPORT_TROPERATION, order.orderId_), requestClOrdId_(requestClOrdId)
{
}

CreateCancelExecReportTrOperation::~CreateCancelExecReportTrOperation() {}

void CreateCancelExecReportTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    ExecutionEntry execReport;
    execReport.orderId_ = getObjectId();
    execReport.type_ = CANCEL_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = CANCELED_ORDSTATUS;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, getObjectId(), execReport, requestClOrdId_);
}

void CreateCancelExecReportTrOperation::rollback(const Context &) {}

CreateTradeExecReportTrOperation::CreateTradeExecReportTrOperation(const TradeExecEntry *trade, OrderStatus status,
                                                                   const OrderEntry &order)
    : Operation(CREATE_TRADE_EXECREPORT_TROPERATION, order.orderId_), status_(status)
{
    assert(nullptr != trade);
    lastQty_ = trade->lastQty_;
    lastPx_ = trade->lastPx_;
    tradeDate_ = trade->tradeDate_;
    currency_ = trade->currency_;
    // The leg comes from the trade: a swap's near leg reaches this report through the state machine, and its report used
    // to say SINGLE_LEG (#153). A plain fill's trade says SINGLE_LEG itself.
    execLegType_ = trade->execLegType_;
    tradeLink_ = trade->tradeLink_; // a swap's near leg's link, as its far leg's report carries it (#199)
}

CreateTradeExecReportTrOperation::CreateTradeExecReportTrOperation(QuantityT lastQty, PriceT lastPx, Currency currency,
                                                                   OrderStatus status, ExecLegType legType,
                                                                   u64 tradeLink, const OrderEntry &order)
    : Operation(CREATE_TRADE_EXECREPORT_TROPERATION, order.orderId_), tradeLink_(tradeLink), status_(status),
      execLegType_(legType)
{
    lastQty_ = lastQty;
    lastPx_ = lastPx;
    tradeDate_ = 1;
    currency_ = currency;
}

CreateTradeExecReportTrOperation::~CreateTradeExecReportTrOperation() {}

void CreateTradeExecReportTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    /// locate source of the order,
    /// prepare ExecReport and send to the source of the order
    TradeExecEntry execReport;

    execReport.lastQty_ = lastQty_;
    execReport.lastPx_ = lastPx_;
    execReport.currency_ = currency_;
    execReport.tradeDate_ = tradeDate_;
    execReport.execLegType_ = execLegType_;
    execReport.tradeLink_ = tradeLink_;

    execReport.orderId_ = getObjectId();
    execReport.type_ = TRADE_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = status_;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
}

void CreateTradeExecReportTrOperation::rollback(const Context &) {}

CreateCorrectExecReportTrOperation::CreateCorrectExecReportTrOperation(const ExecCorrectExecEntry *correct,
                                                                       OrderStatus status, const OrderEntry &order)
    : Operation(CREATE_CORRECT_EXECREPORT_TROPERATION, order.orderId_), status_(status)
{
    assert(nullptr != correct);
    cumQty_ = correct->cumQty_;
    leavesQty_ = correct->leavesQty_;
    lastQty_ = correct->lastQty_;
    lastPx_ = correct->lastPx_;
    currency_ = correct->currency_;
    tradeDate_ = correct->tradeDate_;
    origOrderId_ = correct->origOrderId_;
    execRefId_ = correct->execRefId_;
}

CreateCorrectExecReportTrOperation::~CreateCorrectExecReportTrOperation() {}

void CreateCorrectExecReportTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    /// locate source of the order,
    /// prepare ExecReport and send to the source of the order
    ExecCorrectExecEntry execReport;

    execReport.cumQty_ = cumQty_;
    execReport.leavesQty_ = leavesQty_;
    execReport.lastQty_ = lastQty_;
    execReport.lastPx_ = lastPx_;
    execReport.currency_ = currency_;
    execReport.tradeDate_ = tradeDate_;
    execReport.origOrderId_ = origOrderId_;
    execReport.execRefId_ = execRefId_;

    execReport.orderId_ = getObjectId();
    execReport.type_ = CORRECT_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = status_;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
}

void CreateCorrectExecReportTrOperation::rollback(const Context &) {}

CreateRejectExecReportTrOperation::CreateRejectExecReportTrOperation(std::string_view reason, OrderStatus status,
                                                                     const OrderEntry &order)
    : Operation(CREATE_REJECT_EXECREPORT_TROPERATION, order.orderId_), reason_(reason), status_(status)
{
}

CreateRejectExecReportTrOperation::~CreateRejectExecReportTrOperation() {}

void CreateRejectExecReportTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    /// locate source of the order,
    /// prepare ExecReport and send to the source of the order
    RejectExecEntry execReport;

    execReport.rejectReason_ = reason_;

    execReport.orderId_ = getObjectId();
    execReport.type_ = REJECT_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = status_;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
}

void CreateRejectExecReportTrOperation::rollback(const Context &) {}

CreateReplaceExecReportTrOperation::CreateReplaceExecReportTrOperation(const IdT &origOrderId, OrderStatus status,
                                                                       const OrderEntry &order)
    : Operation(CREATE_REPLACE_EXECREPORT_TROPERATION, order.orderId_), origOrderId_(origOrderId), status_(status)
{
}

CreateReplaceExecReportTrOperation::~CreateReplaceExecReportTrOperation() {}

void CreateReplaceExecReportTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    /// locate source of the order,
    /// prepare ExecReport and send to the source of the order
    ReplaceExecEntry execReport;

    execReport.origOrderId_ = origOrderId_;

    execReport.orderId_ = getObjectId();
    execReport.type_ = REPLACE_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    execReport.orderStatus_ = status_;
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
}

void CreateReplaceExecReportTrOperation::rollback(const Context &) {}

AddToOrderBookTrOperation::AddToOrderBookTrOperation(const OrderEntry &order, const IdT &instrId)
    : Operation(ADD_ORDERBOOK_TROPERATION, order.orderId_, instrId), order_(order)
{
}
AddToOrderBookTrOperation::~AddToOrderBookTrOperation() {}

void AddToOrderBookTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->add(order_);
}

void AddToOrderBookTrOperation::rollback(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->remove(order_);
}

PersistOrderTrOperation::PersistOrderTrOperation(const OrderEntry &order)
    : Operation(PERSIST_ORDER_TROPERATION, order.orderId_), order_(order), version_(0), written_(false)
{
}
PersistOrderTrOperation::~PersistOrderTrOperation() {}

void PersistOrderTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.orderStorage_);
    // Read lock: this runs on a transaction worker while other threads may read the same order.
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(order_.entryMutex_, false);
    written_ = cnxt.orderStorage_->persist(order_, &version_);
}

void PersistOrderTrOperation::rollback(const Context &cnxt)
{
    assert(nullptr != cnxt.orderStorage_);
    if (written_)
    {
        // Exact undo: erase the version this operation appended, leaving earlier ones untouched.
        cnxt.orderStorage_->unpersist(order_.orderId_, version_);
        written_ = false;
    }
}

PersistLastTradeTrOperation::PersistLastTradeTrOperation(const SourceIdT &instrument, PriceT lastPx, const IdT &tradeId)
    : Operation(PERSIST_LASTTRADE_TROPERATION, lastTradeObject(instrument)), instrument_(instrument), lastPx_(lastPx),
      tradeId_(tradeId)
{
}

IdT PersistLastTradeTrOperation::lastTradeObject(const SourceIdT &instrument)
{
    // Reference data has dates 0 and 1, and an order the Unix second its id was drawn in, so nothing else has date 2
    return IdT(instrument.id_, 2);
}

void PersistLastTradeTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.orderStorage_);
    written_ = cnxt.orderStorage_->persistLastTrade(instrument_, lastPx_, tradeId_, &version_);
}

void PersistLastTradeTrOperation::rollback(const Context &cnxt)
{
    assert(nullptr != cnxt.orderStorage_);
    if (written_)
    {
        cnxt.orderStorage_->unpersistLastTrade(instrument_, version_);
        written_ = false;
    }
}

RemoveFromOrderBookTrOperation::RemoveFromOrderBookTrOperation(const OrderEntry &order, const IdT &instrId)
    : Operation(REMOVE_ORDERBOOK_TROPERATION, order.orderId_, instrId), order_(order)
{
}
RemoveFromOrderBookTrOperation::~RemoveFromOrderBookTrOperation() {}

void RemoveFromOrderBookTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->remove(order_);
}

void RemoveFromOrderBookTrOperation::rollback(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->add(order_);
}

CancelRejectTrOperation::CancelRejectTrOperation(const OrderEntry &order, CancelRejectEvent::Reason reason,
                                                 const std::string &requester, const std::string &requestClOrdId)
    : Operation(CANCEL_REJECT_TROPERATION, order.orderId_), status_(order.status_), reason_(reason),
      requester_(requester), requestClOrdId_(requestClOrdId)
{
}

CancelRejectTrOperation::~CancelRejectTrOperation() {}

void CancelRejectTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    CancelRejectEvent reject;
    reject.id_ = getObjectId();
    reject.reason_ = reason_;
    reject.ordStatus_ = status_;
    reject.requestClOrdId_ = requestClOrdId_;
    std::string target = requester_;
    if (target.empty())
    {
        assert(nullptr != cnxt.orderStorage_);
        OrderEntry *ord = cnxt.orderStorage_->locateByOrderId(getObjectId());
        if (nullptr != ord)
        {
            oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, false);
            target = ord->source_.get();
        }
    }
    cnxt.outQueues_->push(reject, target);
}

void CancelRejectTrOperation::rollback(const Context &) {}

ReplaceOrderTrOperation::ReplaceOrderTrOperation(OrderEntry *original, std::unique_ptr<OrderEntry> replacement,
                                                 const std::string &requester)
    : Operation(REPLACE_ORDER_TROPERATION, original->orderId_, original->instrument_.getId()), original_(original),
      replacement_(std::move(replacement)), requester_(requester), eventCountBefore_(0)
{
}

ReplaceOrderTrOperation::~ReplaceOrderTrOperation() {}

void ReplaceOrderTrOperation::execute(const Context &cnxt)
{
    if ((nullptr == cnxt.deferedEvents_) || (nullptr == replacement_)) [[unlikely]]
    {
        throw std::runtime_error("ReplaceOrderTrOperation: no deferred event container, or no replacement, to decide!");
    }
    eventCountBefore_ = cnxt.deferedEvents_->deferedEventCount();
    cnxt.deferedEvents_->addDeferedEvent(
        new ReplaceRequestDeferedEvent(original_, std::move(replacement_), requester_));
}

void ReplaceOrderTrOperation::rollback(const Context &cnxt)
{
    if (nullptr != cnxt.deferedEvents_)
    {
        cnxt.deferedEvents_->removeDeferedEventsFrom(eventCountBefore_);
    }
}

RefuseReplaceTrOperation::RefuseReplaceTrOperation(const OrderEntry &original, const OrderRejectEvent &refusal,
                                                   const std::string &requester)
    : Operation(REFUSE_REPLACE_TROPERATION, original.orderId_), refusal_(refusal), requester_(requester)
{
}

RefuseReplaceTrOperation::~RefuseReplaceTrOperation() {}

void RefuseReplaceTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.outQueues_);
    cnxt.outQueues_->push(refusal_, requester_);
}

void RefuseReplaceTrOperation::rollback(const Context &) {}

CancelOrderTrOperation::CancelOrderTrOperation(OrderEntry *order, const std::string &requester,
                                               const std::string &requestClOrdId)
    : Operation(CANCEL_ORDER_TROPERATION, order->orderId_, order->instrument_.getId()), order_(order),
      requester_(requester), requestClOrdId_(requestClOrdId), eventCountBefore_(0)
{
}

CancelOrderTrOperation::~CancelOrderTrOperation() {}

void CancelOrderTrOperation::execute(const Context &cnxt)
{
    if (nullptr == cnxt.deferedEvents_) [[unlikely]]
    {
        throw std::runtime_error("CancelOrderTrOperation: no deferred event container to decide the cancel!");
    }
    eventCountBefore_ = cnxt.deferedEvents_->deferedEventCount();
    cnxt.deferedEvents_->addDeferedEvent(new CancelRequestDeferedEvent(order_, requester_, requestClOrdId_));
}

void CancelOrderTrOperation::rollback(const Context &cnxt)
{
    if (nullptr != cnxt.deferedEvents_)
    {
        cnxt.deferedEvents_->removeDeferedEventsFrom(eventCountBefore_);
    }
}

ChangeOrderStateTrOperation::ChangeOrderStateTrOperation(OrderEntry *order, OrderStateChange change)
    : Operation(CHANGE_ORDER_STATE_TROPERATION, order->orderId_, order->instrument_.getId()), order_(order),
      change_(change), eventCountBefore_(0)
{
}

ChangeOrderStateTrOperation::~ChangeOrderStateTrOperation() {}

void ChangeOrderStateTrOperation::execute(const Context &cnxt)
{
    if (nullptr == cnxt.deferedEvents_) [[unlikely]]
    {
        throw std::runtime_error("ChangeOrderStateTrOperation: no deferred event container to decide the change!");
    }
    eventCountBefore_ = cnxt.deferedEvents_->deferedEventCount();
    cnxt.deferedEvents_->addDeferedEvent(new StateChangeDeferedEvent(order_, change_));
}

void ChangeOrderStateTrOperation::rollback(const Context &cnxt)
{
    if (nullptr != cnxt.deferedEvents_)
    {
        cnxt.deferedEvents_->removeDeferedEventsFrom(eventCountBefore_);
    }
}

// Related to the instrument as well as the order, as a cancel is: a match reads the instrument's book, so it runs after
// every earlier transaction on the instrument, and a market order's booking, which has no book operation, is ordered
// with the others too. Without it two orders could match the same resting order at once (#209).
MatchOrderTrOperation::MatchOrderTrOperation(OrderEntry *order)
    : Operation(MATCH_ORDER_TROPERATION, order->orderId_, order->instrument_.getId()), order_(order),
      eventCountBefore_(0)
{
}

MatchOrderTrOperation::~MatchOrderTrOperation() {}

AddToStopBookTrOperation::AddToStopBookTrOperation(const OrderEntry &order, const IdT &instrId)
    : Operation(ADD_STOPBOOK_TROPERATION, order.orderId_, instrId), order_(order)
{
}

void AddToStopBookTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->addStop(order_);
}

void AddToStopBookTrOperation::rollback(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->removeStop(order_);
}

RemoveFromStopBookTrOperation::RemoveFromStopBookTrOperation(const OrderEntry &order, const IdT &instrId)
    : Operation(REMOVE_STOPBOOK_TROPERATION, order.orderId_, instrId), order_(order)
{
}

void RemoveFromStopBookTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->removeStop(order_);
}

void RemoveFromStopBookTrOperation::rollback(const Context &cnxt)
{
    assert(nullptr != cnxt.orderBook_);
    cnxt.orderBook_->addStop(order_);
}

// Related to the instrument, as a match is: the release matches the order against the instrument's book (#209)
ReleaseStopTrOperation::ReleaseStopTrOperation(OrderEntry *order)
    : Operation(RELEASE_STOP_TROPERATION, order->orderId_, order->instrument_.getId()), order_(order)
{
}

void ReleaseStopTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != order_);
    assert(nullptr != cnxt.orderBook_);
    released_ = false;
    booked_ = false;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order_->entryMutex_, true);
        const bool working =
            ((NEW_ORDSTATUS == order_->status_) || (PARTFILL_ORDSTATUS == order_->status_)) && (0 < order_->leavesQty_);
        if (!order_->heldStop() || !working)
        {
            return; // cancelled, replaced or finished first, or released by an earlier trade's release
        }
        // ReleaseStopDeferedEvent enqueued the release only once the stop's booking was enqueued (#83)
        assert(!order_->bookingPending_.load());
        cnxt.orderBook_->removeStop(*order_);
        order_->stopTriggered_ = true;
        released_ = true;
    }
    if (nullptr != cnxt.deferedEvents_)
    {
        eventCountBefore_ = cnxt.deferedEvents_->deferedEventCount();
    }
    // Its first match: an IOC or FOK order's time in force applies now, when it is released, not when it arrived (#240)
    cnxt.orderMatch_->matchNew(order_, cnxt);
    // A STOPLIMIT rests with what is left, as a limit order does: booked even if its fills will take all of it, which
    // remove it again (processFill). A STOP is matched once, as a market order is, and so is an IOC or FOK order.
    if (OrderParams::Book::PRICE == order_->book())
    {
        cnxt.orderBook_->add(*order_);
        booked_ = true;
    }
}

void ReleaseStopTrOperation::rollback(const Context &cnxt)
{
    if (!released_)
    {
        return;
    }
    assert(nullptr != cnxt.orderBook_);
    if (booked_)
    {
        cnxt.orderBook_->remove(*order_);
    }
    if (nullptr != cnxt.deferedEvents_)
    {
        cnxt.deferedEvents_->removeDeferedEventsFrom(eventCountBefore_);
    }
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(order_->entryMutex_, true);
    order_->stopTriggered_ = false;
    cnxt.orderBook_->addStop(*order_);
}

ReportReleaseTrOperation::ReportReleaseTrOperation(const ReleaseStopTrOperation &release, const OrderEntry &order)
    : Operation(REPORT_RELEASE_TROPERATION, order.orderId_), release_(release)
{
}

void ReportReleaseTrOperation::execute(const Context &cnxt)
{
    if (!release_.released())
    {
        return;
    }
    assert(nullptr != cnxt.orderStorage_);
    const OrderEntry *order = cnxt.orderStorage_->locateByOrderId(getObjectId());
    assert(nullptr != order);

    ExecutionEntry execReport;
    execReport.orderId_ = getObjectId();
    execReport.type_ = TRIGGERED_EXECTYPE;
    execReport.transactTime_ = aux::currentDateTime();
    execReport.execId_ = IdT(); /// will be assigned when saved into the storage
    {
        // NEW: a held stop has no fills, and the release's own trades are applied after this transaction's operations
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
        execReport.orderStatus_ = order->status_;
    }
    execReport.market_ = INTERNAL_EXECUTION;
    processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
}

void ReportReleaseTrOperation::rollback(const Context &) {}

void MatchOrderTrOperation::execute(const Context &cnxt)
{
    assert(nullptr != order_);

    // Record event count before matching so we can rollback if needed
    if (cnxt.deferedEvents_ != nullptr)
    {
        eventCountBefore_ = cnxt.deferedEvents_->deferedEventCount();
    }

    // The order's first match, as its booking runs it: a FOK order's check comes first (#240)
    cnxt.orderMatch_->matchNew(order_, cnxt);
}

void MatchOrderTrOperation::rollback(const Context &cnxt)
{
    // Remove any deferred events that were added during execute()
    if (cnxt.deferedEvents_ != nullptr)
    {
        cnxt.deferedEvents_->removeDeferedEventsFrom(eventCountBefore_);
    }
}
