/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/


#include "StateMachineHelper.h"
#include "StateMachine.h"
#include "TestAux.h"

using namespace test;
using namespace COP;
using namespace COP::OrdState;

namespace test{

struct OrderStateImpl{
	OrderState state_;

	OrderStateImpl():state_(){}
};

}

OrderStateWrapper::OrderStateWrapper():stateImpl_(new test::OrderStateImpl())
{
}

OrderStateWrapper::~OrderStateWrapper()
{
	delete stateImpl_;
}

void OrderStateWrapper::start()
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.start();
}

void OrderStateWrapper::checkStates(const std::string &fst, const std::string &scnd)const
{
	assert(NULL != stateImpl_);
	check(fst == OrderState::getStateName(stateImpl_->state_.current_state()[0]));
	check(scnd == OrderState::getStateName(stateImpl_->state_.current_state()[1]));	
}

void OrderStateWrapper::processEvent(const onOrderReceived &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}

void OrderStateWrapper::processEvent(const onOrderAccepted &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}

void OrderStateWrapper::processEvent(const COP::OrdState::onRplOrderReceived &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onNewOrder &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExternalOrder &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExternalOrderRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRecvRplOrderRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onTradeExecution &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRplOrderRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onTradeCrctCncl &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExpired &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRplOrderExpired &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onCancelReceived &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onCanceled &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRecvOrderRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}

void OrderStateWrapper::processEvent(const COP::OrdState::onOrderRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplaceRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplacedRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onCancelRejected &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplaceReceived &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplace &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onFinished &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExecCancel &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onNewDay &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onContinue &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onSuspended &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExecReplace &evnt)
{
	assert(NULL != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
