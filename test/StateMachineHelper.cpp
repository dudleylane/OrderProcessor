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
	assert(nullptr != stateImpl_);
	stateImpl_->state_.start();
}

void OrderStateWrapper::checkStates(const std::string &fst, const std::string &scnd)const
{
	assert(nullptr != stateImpl_);
	check(fst == OrderState::getStateName(stateImpl_->state_.current_state()[0]));
	check(scnd == OrderState::getStateName(stateImpl_->state_.current_state()[1]));	
}

void OrderStateWrapper::processEvent(const onOrderReceived &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}

void OrderStateWrapper::processEvent(const COP::OrdState::onRplOrderReceived &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onNewOrder &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExternalOrder &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExternalOrderRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRecvRplOrderRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onTradeExecution &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRplOrderRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onTradeCrctCncl &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExpired &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRplOrderExpired &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onCancelReceived &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onCanceled &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onRecvOrderRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}

void OrderStateWrapper::processEvent(const COP::OrdState::onOrderRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplaceRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplacedRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onCancelRejected &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplaceReceived &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onReplace &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onFinished &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExecCancel &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onNewDay &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onContinue &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onSuspended &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
void OrderStateWrapper::processEvent(const COP::OrdState::onExecReplace &evnt)
{
	assert(nullptr != stateImpl_);
	stateImpl_->state_.process_event(evnt);	
}
