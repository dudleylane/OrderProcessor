/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

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
#include "Logger.h"

using namespace std;
using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

Processor::Processor(void): generator_(nullptr), orderStorage_(nullptr), orderBook_(nullptr), 
	inQueues_(nullptr), outQueues_(nullptr), stateMachine_(nullptr), testStateMachine_(false), 
	testStateMachineCheckResult_(nullptr)
{
	//aux::ExchLogger::instance()->note("Processor created.");
}

Processor::~Processor(void)
{
	delete stateMachine_;
	stateMachine_ = nullptr;
	//aux::ExchLogger::instance()->note("Processor destroyed.");
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

	stateMachine_ = new OrderState();
	stateMachine_->start();
	initialSMState_ = stateMachine_->getPersistence();

	matcher_.init(this);

	//aux::ExchLogger::instance()->note("Processor initialized.");
}

bool Processor::process()
{
	//aux::ExchLogger::instance()->debug("Processor start processing.");
	assert(nullptr != inQueue_);
	bool rez = inQueue_->pop(this);
	//aux::ExchLogger::instance()->debug("Processor finish processing.");
	return rez;
}

void Processor::onEvent(const std::string &/*source*/, const OrderEvent &evnt)
{
	assert(nullptr != evnt.order_);
	assert(events_.empty());
	
	//aux::ExchLogger::instance()->debug("Processor start onEvent(OrderEvent).");

	// prepare context
	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);
	auto_ptr<TransactionScope> scope(new TransactionScope());

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
	auto_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);//scope.executeTransaction(cntxt);

	// process defered events
	processDeferedEvent();

	//aux::ExchLogger::instance()->debug("Processor finished onEvent(OrderEvent).");
}

void Processor::onEvent(const std::string &/*source*/, const OrderCancelEvent &/*evnt*/)
{
	//aux::ExchLogger::instance()->debug("Processor finished onEvent(OrderCancelEvent).");
}

void Processor::onEvent(const std::string &/*source*/, const OrderReplaceEvent &/*evnt*/)
{
	//aux::ExchLogger::instance()->debug("Processor finished onEvent(OrderReplaceEvent).");
}

void Processor::onEvent(const std::string &/*source*/, const COP::Queues::OrderChangeStateEvent &/*evnt*/)
{
	//aux::ExchLogger::instance()->debug("Processor finished onEvent(OrderStateEvent).");
}

void Processor::onEvent(const std::string &/*source*/, const ProcessEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("Processor start onEvent(ProcessEvent).");
	///prepare context
	assert(events_.empty());
	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);
	auto_ptr<TransactionScope> scope(new TransactionScope());

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
	auto_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);//scope.executeTransaction(cntxt);

	processDeferedEvent();

	//aux::ExchLogger::instance()->debug("Processor finished onEvent(ProcessEvent).");
}

void Processor::onEvent(const std::string &/*source*/, const TimerEvent &/*evnt*/)
{
	//aux::ExchLogger::instance()->debug("Processor finished onEvent(TimerEvent).");
}

void Processor::addDeferedEvent(DeferedEventBase *evnt)
{
	assert(nullptr != evnt);
	events_.push_back(evnt);
	//aux::ExchLogger::instance()->debug("Processor: added defered event.");
}

void Processor::onEvent(DeferedEventBase *evnt)
{
	//aux::ExchLogger::instance()->debug("Processor: start onEvent(DeferedEventBase)");

	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);
	auto_ptr<TransactionScope> scope(new TransactionScope());

	evnt->execute(this, cntxt, scope.get());

	assert(nullptr != transactMgr_);
	auto_ptr<Transaction> tr(scope.release());
	transactMgr_->addTransaction(tr);//scope.executeTransaction(cntxt);

	//aux::ExchLogger::instance()->debug("Processor: finished onEvent(DeferedEventBase)");
}

void Processor::processDeferedEvent()
{
	//aux::ExchLogger::instance()->debug("Processor: start processDeferedEvent");
	/// Defered events may enqueue another chain of defered events - need to process them also.
	while(!events_.empty()){
		DeferedEventsT tmp;
		swap(tmp, events_);
		try{
			for(DeferedEventsT::iterator it = tmp.begin(); it != tmp.end(); ++it){
				auto_ptr<DeferedEventBase> evnt(*it);
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
	//aux::ExchLogger::instance()->debug("Processor: finished processDeferedEvent");
}

void Processor::process(onTradeExecution &evnt, OrderEntry *order, const ACID::Context &/*cnxt*/)
{
	//aux::ExchLogger::instance()->debug("Processor: start processing  onTradeExecution");

	evnt.generator_ = generator_;
	evnt.orderStorage_ = orderStorage_;

	stateMachine_->setPersistance(order->stateMachinePersistance());
	stateMachine_->process_event(evnt);

	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	//aux::ExchLogger::instance()->debug("Processor: finish processing onTradeExecution");
}

void Processor::process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context & /*cnxt*/)
{
	//aux::ExchLogger::instance()->debug("Processor: start processing  onCanceled");

	evnt.generator_ = generator_;
	evnt.orderStorage_ = orderStorage_;

	stateMachine_->setPersistance(order->stateMachinePersistance());
	stateMachine_->process_event(evnt);

	OrderStatePersistence smState = stateMachine_->getPersistence();
	assert(nullptr != smState.orderData_);
	smState.orderData_->setStateMachinePersistance(smState);

	//aux::ExchLogger::instance()->debug("Processor: finish processing onCanceled");
}

void Processor::process(const ACID::TransactionId &id, ACID::Transaction *tr)
{
	assert(nullptr != tr);
	assert(id.isValid());

	//aux::ExchLogger::instance()->debug("Processor: start processing transaction");

	Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_);

	tr->executeTransaction(cntxt);

	processDeferedEvent();

	//aux::ExchLogger::instance()->debug("Processor: finish processing transaction");
}