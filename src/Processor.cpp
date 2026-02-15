/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include <cassert>
#include "Processor.h"
#include "TransactionDef.h"
#include "StateMachine.h"
#include "TransactionScope.h"
#include "DataModelDef.h"
#include "OrderStorage.h"

using namespace std;
using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;
using COP::ACID::PooledTransactionScope;

Processor::Processor(void): generator_(nullptr), orderStorage_(nullptr), orderBook_(nullptr),
	inQueues_(nullptr), outQueues_(nullptr), testStateMachine_(false),
	testStateMachineCheckResult_(false),
	scopePool_(std::make_unique<ACID::TransactionScopePool>())
{
}

Processor::~Processor(void)
{
}

void Processor::init(const ProcessorParams &params)
{
	generator_ = params.generator_;
	orderStorage_ = params.orderStorage_;
	orderBook_ = params.orderBook_;
	inQueues_ = params.inQueues_;
	outQueues_ = params.outQueues_;
	inQueue_ = params.inQueue_;
	transactMgr_ = params.transactMgr_;

	testStateMachine_ = params.testStateMachine_;
	testStateMachineCheckResult_ = params.testStateMachineCheckResult_;

	assert(nullptr != generator_);
	assert(nullptr != orderStorage_);
	assert(nullptr != orderBook_);
	assert(nullptr != inQueues_);
	assert(nullptr != outQueues_);
	assert(nullptr != inQueue_);
	assert(nullptr != transactMgr_);

	stateMachine_ = std::make_unique<OrderState>();
	stateMachine_->start();
	initialSMState_ = stateMachine_->getPersistence();

	matcher_.init(this);
}

bool Processor::process()
{
	assert(nullptr != inQueue_);
	bool rez = inQueue_->pop(this);
	return rez;
}

void Processor::onEvent(const std::string &/*source*/, const OrderEvent &evnt)
{
	if(nullptr == evnt.order_)
		throw std::runtime_error("Processor::onEvent(OrderEvent): order pointer is null!");
	if(!events_.empty())
		throw std::runtime_error("Processor::onEvent(OrderEvent): events queue is not empty!");

	// prepare scope
	PooledTransactionScope scope(scopePool_.get());

	// restore event to process
	onOrderReceived evnt2Proc(evnt.order_);
	evnt2Proc.generator_ = generator_;
	evnt2Proc.transaction_ = scope.get();
	evnt2Proc.orderStorage_ = orderStorage_;
	evnt2Proc.orderBook_ = orderBook_;

	// restore state of the state machine
	assert(nullptr != stateMachine_);
	stateMachine_->setPersistance(initialSMState_);
	// process event
	stateMachine_->process_event(evnt2Proc);
	// save state machine state into the order
	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	// enqueue transaction, prepared by state machine
	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);

	// process defered events
	processDeferedEvent();
}

void Processor::onEvent(const std::string &/*source*/, const OrderCancelEvent &evnt)
{
	if(!evnt.id_.isValid())
		throw std::runtime_error("Processor::onEvent(OrderCancelEvent): order id is invalid!");

	PooledTransactionScope scope(scopePool_.get());

	// locate the order to cancel
	OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
	if(nullptr == ord)
		throw std::runtime_error("Processor::onEvent(OrderCancelEvent): unable to locate order!");

	// create cancel received event
	onCancelReceived evnt2Proc;
	evnt2Proc.generator_ = generator_;
	evnt2Proc.transaction_ = scope.get();
	evnt2Proc.orderStorage_ = orderStorage_;
	evnt2Proc.orderBook_ = orderBook_;

	// restore state machine from order and process event
	assert(nullptr != stateMachine_);
	stateMachine_->setPersistance(ord->stateMachinePersistance());
	stateMachine_->process_event(evnt2Proc);

	// save updated state back to order
	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	// enqueue transaction
	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);

	processDeferedEvent();
}

void Processor::onEvent(const std::string &/*source*/, const OrderReplaceEvent &evnt)
{
	if(!evnt.id_.isValid())
		throw std::runtime_error("Processor::onEvent(OrderReplaceEvent): order id is invalid!");

	PooledTransactionScope scope(scopePool_.get());

	if(nullptr != evnt.replacementOrder_) {
		// New replacement order submission - process via state machine
		onRplOrderReceived evnt2Proc(evnt.replacementOrder_);
		evnt2Proc.generator_ = generator_;
		evnt2Proc.transaction_ = scope.get();
		evnt2Proc.orderStorage_ = orderStorage_;
		evnt2Proc.orderBook_ = orderBook_;

		// use initial state for new replacement order
		assert(nullptr != stateMachine_);
		stateMachine_->setPersistance(initialSMState_);
		stateMachine_->process_event(evnt2Proc);

		// save state machine state into the replacement order
		OrderStatePersistence smState = stateMachine_->getPersistence();
		assert(nullptr != smState.orderData_);
		smState.orderData_->setStateMachinePersistance(smState);
	} else {
		// Notify existing order about replace received (similar to ProcessEvent::ON_REPLACE_RECEVIED)
		OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
		if(nullptr == ord)
			throw std::runtime_error("Processor::onEvent(OrderReplaceEvent): unable to locate order!");

		onReplaceReceived evnt2Proc(evnt.id_);
		evnt2Proc.generator_ = generator_;
		evnt2Proc.transaction_ = scope.get();
		evnt2Proc.orderStorage_ = orderStorage_;

		assert(nullptr != stateMachine_);
		stateMachine_->setPersistance(ord->stateMachinePersistance());
		stateMachine_->process_event(evnt2Proc);

		// save updated state back to order
		OrderStatePersistence smState = stateMachine_->getPersistence();
		assert(nullptr != smState.orderData_);
		smState.orderData_->setStateMachinePersistance(smState);
	}

	// enqueue transaction
	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);

	processDeferedEvent();
}

void Processor::onEvent(const std::string &/*source*/, const COP::Queues::OrderChangeStateEvent &evnt)
{
	if(!evnt.id_.isValid())
		throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): order id is invalid!");
	if(evnt.changeType_ == OrderChangeStateEvent::INVALID_CHANGE)
		throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): invalid change type!");

	PooledTransactionScope scope(scopePool_.get());

	// locate the order
	OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
	if(nullptr == ord)
		throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): unable to locate order!");

	// restore state machine from order
	assert(nullptr != stateMachine_);
	stateMachine_->setPersistance(ord->stateMachinePersistance());

	// process the appropriate state change event
	switch(evnt.changeType_) {
	case OrderChangeStateEvent::SUSPEND:
		{
			onSuspended evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	case OrderChangeStateEvent::RESUME:
		{
			onContinue evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	case OrderChangeStateEvent::FINISH:
		{
			onFinished evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	default:
		throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): unknown change type!");
	}

	// save updated state back to order
	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	// enqueue transaction
	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);

	processDeferedEvent();
}

void Processor::onEvent(const std::string &/*source*/, const ProcessEvent &evnt)
{
	assert(events_.empty());
	PooledTransactionScope scope(scopePool_.get());

	switch(evnt.type_){
	case ProcessEvent::ON_REPLACE_RECEVIED:
		{
			// restore event ot process
			onReplaceReceived evnt2Proc(evnt.id_);
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;

			assert(nullptr != stateMachine_);
			OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
			if(nullptr == ord)
				throw std::runtime_error("Processor::onEvent(ProcessEvent): unable to locate order for ON_REPLACE_RECEVIED!");
			stateMachine_->setPersistance(ord->stateMachinePersistance());
			// process event
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	case ProcessEvent::ON_EXEC_REPLACE:
		{
			// restore event to process
			onExecReplace evnt2Proc(evnt.id_);
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;

			assert(nullptr != stateMachine_);
			OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
			if(nullptr == ord)
				throw std::runtime_error("Processor::onEvent(ProcessEvent): unable to locate order for ON_EXEC_REPLACE!");
			stateMachine_->setPersistance(ord->stateMachinePersistance());
			// process event
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	case ProcessEvent::ON_REPLACE_REJECTED:
		{
			// restore event to process
			onReplaceRejected evnt2Proc(evnt.id_);
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;

			assert(nullptr != stateMachine_);
			OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
			if(nullptr == ord)
				throw std::runtime_error("Processor::onEvent(ProcessEvent): unable to locate order for ON_REPLACE_REJECTED!");
			stateMachine_->setPersistance(ord->stateMachinePersistance());
			// process event in state machine
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	default:
		throw std::runtime_error("Processor::onEvent() fails: unknown type of the ProcessEvent.");
	};

	// save state into the order
	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);//scope.executeTransaction(cntxt);

	processDeferedEvent();
}

void Processor::onEvent(const std::string &/*source*/, const TimerEvent &evnt)
{
	if(!evnt.id_.isValid())
		throw std::runtime_error("Processor::onEvent(TimerEvent): order id is invalid!");
	if(evnt.timerType_ == TimerEvent::INVALID_TIMER)
		throw std::runtime_error("Processor::onEvent(TimerEvent): invalid timer type!");

	PooledTransactionScope scope(scopePool_.get());

	// locate the order
	OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
	if(nullptr == ord)
		throw std::runtime_error("Processor::onEvent(TimerEvent): unable to locate order!");

	// restore state machine from order
	assert(nullptr != stateMachine_);
	stateMachine_->setPersistance(ord->stateMachinePersistance());

	// process the appropriate timer event
	switch(evnt.timerType_) {
	case TimerEvent::EXPIRATION:
		{
			onExpired evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	case TimerEvent::DAY_END:
		{
			onNewDay evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	case TimerEvent::DAY_START:
		{
			onContinue evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;
			stateMachine_->process_event(evnt2Proc);
		}
		break;
	default:
		throw std::runtime_error("Processor::onEvent(TimerEvent): unknown timer type!");
	}

	// save updated state back to order
	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	// enqueue transaction
	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);

	processDeferedEvent();
}

void Processor::addDeferedEvent(DeferedEventBase *evnt)
{
	assert(nullptr != evnt);
	events_.push_back(evnt);
}

size_t Processor::deferedEventCount() const
{
	return events_.size();
}

void Processor::removeDeferedEventsFrom(size_t startIndex)
{
	if (startIndex >= events_.size()) {
		return;
	}

	DeferedEventsT::iterator it = events_.begin();
	std::advance(it, startIndex);

	// Delete events from startIndex to end
	for (DeferedEventsT::iterator delIt = it; delIt != events_.end(); ++delIt) {
		delete *delIt;
	}
	events_.erase(it, events_.end());
}

void Processor::onEvent(DeferedEventBase *evnt)
{
	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_, this);
	PooledTransactionScope scope(scopePool_.get());

	evnt->execute(this, cntxt, scope.get());

	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);
}

void Processor::processDeferedEvent()
{
	/// Defered events may enqueue another chain of defered events - need to process them also.
	while(!events_.empty()){
		DeferedEventsT tmp;
		swap(tmp, events_);
		try{
			for(DeferedEventsT::iterator it = tmp.begin(); it != tmp.end(); ++it){
				std::unique_ptr<DeferedEventBase> evnt(*it);
				*it = nullptr;

				onEvent(evnt.get());
			}		
		}catch(const std::exception &){
			for(DeferedEventsT::iterator it = tmp.begin(); it != tmp.end(); ++it){
				if(nullptr != *it)
					delete *it;
			}
			// Clear any partial-chain events enqueued before the exception
			// to prevent processing inconsistent state
			DeferedEventsT partial;
			swap(partial, events_);
			for(DeferedEventsT::iterator it = partial.begin(); it != partial.end(); ++it){
				delete *it;
			}
			throw;
		}
	}
}

void Processor::process(onTradeExecution &evnt, OrderEntry *order, const ACID::Context &/*cnxt*/)
{
	evnt.generator_ = generator_;
	evnt.orderStorage_ = orderStorage_;

	stateMachine_->setPersistance(order->stateMachinePersistance());
	stateMachine_->process_event(evnt);

	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);
}

void Processor::process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context & /*cnxt*/)
{
	evnt.generator_ = generator_;
	evnt.orderStorage_ = orderStorage_;

	stateMachine_->setPersistance(order->stateMachinePersistance());
	stateMachine_->process_event(evnt);

	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);
}

void Processor::process(const ACID::TransactionId &id, ACID::Transaction *tr)
{
	assert(nullptr != tr);
	assert(id.isValid());

	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_, this);

	bool success = tr->executeTransaction(cntxt);

	if (success) {
		processDeferedEvent();
	} else {
		clearDeferedEvents();
	}
}

void Processor::clearDeferedEvents()
{
	for (DeferedEventsT::iterator it = events_.begin(); it != events_.end(); ++it) {
		delete *it;
	}
	events_.clear();
}