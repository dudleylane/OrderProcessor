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

Processor::Processor(void): generator_(nullptr), orderStorage_(nullptr), orderBook_(nullptr),
	inQueues_(nullptr), outQueues_(nullptr), testStateMachine_(false),
	testStateMachineCheckResult_(false)
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

	// prepare context
	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);
	std::unique_ptr<TransactionScope> scope(new TransactionScope());

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
	transactMgr_->addTransaction(tr);//scope.executeTransaction(cntxt);

	// process defered events
	processDeferedEvent();
}

void Processor::onEvent(const std::string &/*source*/, const OrderCancelEvent &/*evnt*/)
{
}

void Processor::onEvent(const std::string &/*source*/, const OrderReplaceEvent &/*evnt*/)
{
}

void Processor::onEvent(const std::string &/*source*/, const COP::Queues::OrderChangeStateEvent &/*evnt*/)
{
}

void Processor::onEvent(const std::string &/*source*/, const ProcessEvent &evnt)
{
	///prepare context
	assert(events_.empty());
	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);
	std::unique_ptr<TransactionScope> scope(new TransactionScope());

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
			assert(nullptr != ord);
			stateMachine_->setPersistance(ord->stateMachinePersistance());
			// process event
			stateMachine_->process_event(evnt2Proc);	
		}
		break;
	case ProcessEvent::ON_ORDER_ACCEPTED:
		{
			// restore event to process
			onOrderAccepted evnt2Proc;
			evnt2Proc.generator_ = generator_;
			evnt2Proc.transaction_ = scope.get();
			evnt2Proc.orderStorage_ = orderStorage_;
			evnt2Proc.orderBook_ = orderBook_;

			assert(nullptr != stateMachine_);
			OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
			assert(nullptr != ord);
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
			assert(nullptr != ord);
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
			assert(nullptr != ord);
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

void Processor::onEvent(const std::string &/*source*/, const TimerEvent &/*evnt*/)
{
}

void Processor::addDeferedEvent(DeferedEventBase *evnt)
{
	assert(nullptr != evnt);
	events_.push_back(evnt);
}

void Processor::onEvent(DeferedEventBase *evnt)
{
	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);
	std::unique_ptr<TransactionScope> scope(new TransactionScope());

	evnt->execute(this, cntxt, scope.get());

	assert(nullptr != transactMgr_);
	std::unique_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);//scope.executeTransaction(cntxt);
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

	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);

	tr->executeTransaction(cntxt);

	processDeferedEvent();
}