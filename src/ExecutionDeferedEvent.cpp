/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include "DeferedEvents.h"
#include "Processor.h"
#include "TransactionDef.h"
#include "DataModelDef.h"
#include "OrderStorage.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

ExecutionDeferedEvent::ExecutionDeferedEvent(): baseOrder_(nullptr)
{}

ExecutionDeferedEvent::ExecutionDeferedEvent(OrderEntry *ord): baseOrder_(ord)
{
	assert(nullptr != baseOrder_);
}

void ExecutionDeferedEvent::execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope, const TradeParams &param)
{
	//aux::ExchLogger::instance()->debug("ExecutionDeferedEvent: Start execution.");

	TradeExecEntry trade;
	trade.lastQty_ = param.lastQty_;
	trade.lastPx_ = param.lastPx_;
	trade.currency_ = param.order_->currency_;
	trade.tradeDate_ = 1;
	trade.type_ = TRADE_EXECTYPE;
	trade.transactTime_ = 1;
	trade.orderId_ = param.order_->orderId_;
	trade.execId_;
	trade.orderStatus_;
	onTradeExecution evnt4Proc(&trade);
	evnt4Proc.orderBook_ = cnxt.orderBook_;
	evnt4Proc.transaction_ = scope;

	func->process(evnt4Proc, param.order_, cnxt);

	//aux::ExchLogger::instance()->debug("ExecutionDeferedEvent: Finish execution.");
}

void ExecutionDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
	//aux::ExchLogger::instance()->debug("ExecutionDeferedEvent: Start execution of trades.");

	assert(nullptr != func);
	assert(nullptr != baseOrder_);
	assert(0 < trades_.size());

	TradeParams baseOrderParam;
	baseOrderParam.order_ = baseOrder_;
	baseOrderParam.lastQty_ = 0;
	baseOrderParam.lastPx_ = 0.0;
	for(TradesT::const_iterator it = trades_.begin(); it != trades_.end(); ++it){
		assert(nullptr != it->order_);
		assert(0 < it->lastQty_);
		assert(0 < it->lastPx_);

		QuantityT quantBefore = it->order_->leavesQty_;
		execute(func, cnxt, scope, *it);

		/// ToDo: should be redesigned: if execution applied - increase executed quantity 
		if(quantBefore > it->order_->leavesQty_){
			baseOrderParam.lastQty_ += it->lastQty_;
			baseOrderParam.lastPx_ += it->lastQty_*it->lastPx_;
		}
	}
	if(0 < baseOrderParam.lastQty_){
		baseOrderParam.lastPx_ /= baseOrderParam.lastQty_;

		execute(func, cnxt, scope, baseOrderParam);
	}

	//aux::ExchLogger::instance()->debug("ExecutionDeferedEvent: Finish execution of trades.");
}

