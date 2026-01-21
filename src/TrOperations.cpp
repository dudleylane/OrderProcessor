/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

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

namespace COP{ namespace ACID{
	
	void processEvent_GenerateExecution(const Context &cnxt, const IdT &orderId, 
				const ExecutionEntry &exec)
	{
		/// locate order and push event into the executions_
		/// find source of the order and push ExecReport to the source
		assert(nullptr != cnxt.orderStorage_);
		OrderEntry *ord = cnxt.orderStorage_->locateByOrderId(orderId);
		assert(nullptr != ord);
		
		StringT src = ord->source_.get();

		ExecutionEntry *execReport = cnxt.orderStorage_->save(exec, cnxt.idGenerator_);
		assert(nullptr != execReport);

		ord->addExecution(execReport->execId_);

		assert(nullptr != cnxt.outQueues_);
		cnxt.outQueues_->push(ExecReportEvent(execReport), src);
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

	void rollbackEnqueueOrderEvent(const OrdState::onReplaceReceived &, const Context &)
	{
	}

	void executeEnqueueOrderEvent(const OrdState::onOrderAccepted &event_, const Context &cnxt)
	{
		assert(nullptr != cnxt.inQueues_);
		cnxt.inQueues_->push("_", ProcessEvent(ProcessEvent::ON_ORDER_ACCEPTED, event_.orderId_));

		ExecutionEntry execReport;
		execReport.orderId_ = event_.orderId_;
		execReport.type_ = NEW_EXECTYPE;
		execReport.transactTime_ = aux::currentDateTime();
		execReport.execId_ = IdT(); /// will be assigned when saved into the storage
		execReport.orderStatus_ = NEW_ORDSTATUS;
		execReport.market_ = INTERNAL_EXECUTION;
		processEvent_GenerateExecution(cnxt, event_.orderId_, execReport);
	}

	void rollbackEnqueueOrderEvent(const OrdState::onOrderAccepted &, const Context &)
	{
		
	}

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

	void rollbackEnqueueOrderEvent(const OrdState::onExecReplace &, const Context &)
	{
		
	}

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

	void rollbackEnqueueOrderEvent(const OrdState::onReplaceRejected &, const Context &)
	{
		
	}

}}

CreateExecReportTrOperation::CreateExecReportTrOperation(OrderStatus status, ExecType execType, 
														 const OrderEntry &order):
Operation(CREATE_EXECREPORT_TROPERATION, order.orderId_), status_(status), execType_(execType)
{

}

CreateExecReportTrOperation::~CreateExecReportTrOperation()
{}

void CreateExecReportTrOperation::execute(const Context &cnxt)
{
	assert(nullptr != cnxt.outQueues_);
	/// locate source of the order, 
	/// prepare ExecReport and send to the source of the order
	switch(execType_){
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
			execReport.execId_ = IdT(); /// will be assigned when saved into the storage
			execReport.orderStatus_ = status_;//DFD_ORDSTATUS;
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
			execReport.execId_ = IdT(); /// will be assigned when saved into the storage
			execReport.orderStatus_ = status_;//EXPIRED_ORDSTATUS;
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
			execReport.execId_ = IdT(); /// will be assigned when saved into the storage
			execReport.orderStatus_ = status_;//SUSPENDED_ORDSTATUS;
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

void CreateExecReportTrOperation::rollback(const Context &)
{}

CreateTradeExecReportTrOperation::CreateTradeExecReportTrOperation(const TradeExecEntry *trade, 
													OrderStatus status, const OrderEntry &order):
Operation(CREATE_TRADE_EXECREPORT_TROPERATION, order.orderId_), status_(status)
{
	assert(nullptr != trade);
	lastQty_ = trade->lastQty_;
	lastPx_ = trade->lastPx_;
	tradeDate_ = trade->tradeDate_;
	currency_ = trade->currency_;
}

CreateTradeExecReportTrOperation::~CreateTradeExecReportTrOperation()
{}

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

	execReport.orderId_ = getObjectId();
	execReport.type_ = TRADE_EXECTYPE;
	execReport.transactTime_ = aux::currentDateTime();
	execReport.execId_ = IdT(); /// will be assigned when saved into the storage
	execReport.orderStatus_ = status_;
	execReport.market_ = INTERNAL_EXECUTION;
	processEvent_GenerateExecution(cnxt, getObjectId(), execReport);
}

void CreateTradeExecReportTrOperation::rollback(const Context &)
{}

CreateCorrectExecReportTrOperation::CreateCorrectExecReportTrOperation(const ExecCorrectExecEntry *correct, 
													OrderStatus status, const OrderEntry &order):
Operation(CREATE_CORRECT_EXECREPORT_TROPERATION, order.orderId_), status_(status)
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

CreateCorrectExecReportTrOperation::~CreateCorrectExecReportTrOperation()
{}

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

void CreateCorrectExecReportTrOperation::rollback(const Context &)
{}

CreateRejectExecReportTrOperation::CreateRejectExecReportTrOperation(const std::string &reason, 
													OrderStatus status, const OrderEntry &order):
Operation(CREATE_REJECT_EXECREPORT_TROPERATION, order.orderId_), status_(status), reason_(reason)
{}

CreateRejectExecReportTrOperation::~CreateRejectExecReportTrOperation()
{}

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

void CreateRejectExecReportTrOperation::rollback(const Context &)
{}

CreateReplaceExecReportTrOperation::CreateReplaceExecReportTrOperation(const IdT &origOrderId, 
													OrderStatus status, const OrderEntry &order):
Operation(CREATE_REPLACE_EXECREPORT_TROPERATION, order.orderId_), status_(status), origOrderId_(origOrderId)
{}

CreateReplaceExecReportTrOperation::~CreateReplaceExecReportTrOperation()
{}

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

void CreateReplaceExecReportTrOperation::rollback(const Context &)
{}


AddToOrderBookTrOperation::AddToOrderBookTrOperation(const OrderEntry &order, const IdT &instrId):
	Operation(ADD_ORDERBOOK_TROPERATION, order.orderId_, instrId), order_(order)
{}
AddToOrderBookTrOperation::~AddToOrderBookTrOperation()
{}

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

RemoveFromOrderBookTrOperation::RemoveFromOrderBookTrOperation(const OrderEntry &order, const IdT &instrId):
	Operation(REMOVE_ORDERBOOK_TROPERATION, order.orderId_, instrId), order_(order)
{}
RemoveFromOrderBookTrOperation::~RemoveFromOrderBookTrOperation()
{}

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

CancelRejectTrOperation::CancelRejectTrOperation(OrderStatus status, const OrderEntry &order):
	Operation(CANCEL_REJECT_TROPERATION, order.orderId_), status_(status)
{
}

CancelRejectTrOperation::~CancelRejectTrOperation()
{}

void CancelRejectTrOperation::execute(const Context &cnxt)
{
	assert(nullptr != cnxt.outQueues_);
	cnxt.outQueues_->push(CancelRejectEvent(), "tgt");
}

void CancelRejectTrOperation::rollback(const Context &)
{
}

MatchOrderTrOperation::MatchOrderTrOperation(OrderEntry *order):
	Operation(MATCH_ORDER_TROPERATION, order->orderId_), order_(order), eventCountBefore_(0)
{
}

MatchOrderTrOperation::~MatchOrderTrOperation()
{}

void MatchOrderTrOperation::execute(const Context &cnxt)
{
	assert(nullptr != order_);

	// Record event count before matching so we can rollback if needed
	if (cnxt.deferedEvents_ != nullptr) {
		eventCountBefore_ = cnxt.deferedEvents_->deferedEventCount();
	}

	cnxt.orderMatch_->match(order_, cnxt);
}

void MatchOrderTrOperation::rollback(const Context &cnxt)
{
	// Remove any deferred events that were added during execute()
	if (cnxt.deferedEvents_ != nullptr) {
		cnxt.deferedEvents_->removeDeferedEventsFrom(eventCountBefore_);
	}
}
