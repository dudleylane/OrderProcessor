/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "StateMachine.h"
#include "DataModelDef.h"
#include "OrderStateMachineImpl.h"

using namespace COP;
using namespace COP::OrdState;
using namespace boost::msm;

namespace{
	const int STATE_SIZE = 13;
	const std::string state_names[STATE_SIZE] = { "Rcvd_New", "Pend_Replace", "New", "PartFill",
			"Filled", "Expired", "DoneForDay", "Suspended", "NoCnlReplace", "GoingCancel", "GoingReplace",
			"CnclReplaced", "Rejected"};
}

const std::string &OrderState::getStateName(int idx)
{
	if(STATE_SIZE <= idx)
		throw std::runtime_error("Invalid index of the state's name!");
	return state_names[idx];
}



OrderState::OrderState():orderData_(NULL)
{}

OrderState::OrderState(OrderEntry *orderData): orderData_(orderData)
{}

OrderState::~OrderState()
{}

OrderStatePersistence OrderState::getPersistence()const
{
	OrderStatePersistence st;
	st.orderData_ = orderData_;
	st.stateZone1Id_ = current_state()[0];
	st.stateZone2Id_ = current_state()[1];
	return st;
}

void OrderState::setPersistance(const OrderStatePersistence &st)
{
	orderData_ = st.orderData_;
	m_states[0] = st.stateZone1Id_;
	m_states[1] = st.stateZone2Id_;
}

void OrderState::receive(onOrderReceived const &evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(NULL == orderData_);
	assert(NULL != evnt.order_);
	try{
		OrdStateImpl::processReceive(&orderData_, evnt);
		evnt.order4StateMachine_ = orderData_;
	}catch(const std::exception &ex){
		onRecvOrderRejected newEvnt(evnt, evnt.order_, ex.what());
		newEvnt.order4StateMachine_ = orderData_;
		this->process_event(newEvnt);
		throw;
	}
}

void OrderState::receive(onRplOrderReceived const &evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(NULL == orderData_);
	assert(NULL != evnt.order_);
	try{
		OrdStateImpl::processReceive(&orderData_, evnt);
		evnt.order4StateMachine_ = orderData_;
	}catch(const std::exception &ex){
		onRecvRplOrderRejected newEvnt(evnt, evnt.order_, ex.what());
		newEvnt.order4StateMachine_ = orderData_;
		this->process_event(newEvnt);
		throw;
	}
}


void OrderState::accept(onExternalOrder const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	try{
		OrdStateImpl::processAccept(&orderData_, evnt);
		evnt.order4StateMachine_ = orderData_;
	}catch(const std::exception &ex){
		onExternalOrderRejected newEvnt(evnt, evnt.order_, ex.what());
		newEvnt.order4StateMachine_ = orderData_;
		this->process_event(newEvnt);
		throw;
	}

}

void OrderState::reject(onExternalOrderRejected const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(&orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::reject(onReplaceRejected const &evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::reject(onCancelRejected const &evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::accept(onReplace const&evnt){
	if(evnt.testStateMachine_){
		if(!evnt.testStateMachineCheckResult_){
			onRplOrderRejected newEvnt(evnt, "Test reject");
			this->process_event(newEvnt);
			throw std::runtime_error("Test reject");
		}
		return;
	}

	try{
		OrdStateImpl::processAccept(orderData_, evnt);
		evnt.order4StateMachine_ = orderData_;
	}catch(const std::exception &ex){
		onRplOrderRejected newEvnt(evnt, ex.what());
		newEvnt.order4StateMachine_ = orderData_;
		this->process_event(newEvnt);
		throw;
	}

}

bool OrderState::notexecuted(onTradeExecution const &evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processNotexecuted(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

bool OrderState::notexecuted(onTradeCrctCncl const &evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processNotexecuted(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

bool OrderState::notexecuted(onNewDay const &evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processNotexecuted(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

bool OrderState::notexecuted(onContinue const &evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processNotexecuted(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

bool OrderState::complete(onTradeExecution const &evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processComplete(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

void OrderState::corrected(onTradeCrctCncl const &evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processCorrected(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}


void OrderState::correctedWithoutRestore(onTradeCrctCncl const &evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processCorrectedWithoutRestore(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::expire(onExpired const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processExpire(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::cancel(onCanceled const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processCancel(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::restored(onNewDay const &evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processRestored(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::continued(onContinue const &evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processContinued(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::finished(onFinished const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processFinished(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::suspended(onSuspended const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processSuspended(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::canceled(onExecCancel const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processCanceled(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::canceled(onInternalCancel const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processCanceled(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::replaced(onExecReplace const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReplaced(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::receive(onReplaceReceived const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReceive(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

bool OrderState::acceptable(onReplaceReceived const&evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processAcceptable(evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

void OrderState::receive(onCancelReceived const&evnt){
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReceive(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

bool OrderState::acceptable(onCancelReceived const&evnt){
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processAcceptable(evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}

void OrderState::accept(onOrderAccepted const &evnt)
{
	if(evnt.testStateMachine_){
		if(!evnt.testStateMachineCheckResult_){
			onOrderRejected newEvnt(evnt, "Test reject");
			this->process_event(newEvnt);
			throw std::runtime_error("Test reject");
		}
		return;
	}

	try{
		OrdStateImpl::processAccept(orderData_, evnt);
		evnt.order4StateMachine_ = orderData_;
	}catch(const std::exception &ex){
		onOrderRejected newEvnt(evnt, ex.what());
		newEvnt.order4StateMachine_ = orderData_;
		this->process_event(newEvnt);
		throw;
	}
}

/*bool OrderState::acceptable(onOrderAccepted const &evnt)
{
	if(evnt.testStateMachine_)
		return evnt.testStateMachineCheckResult_;

	bool res = OrdStateImpl::processAcceptable(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
	return res;
}*/

void OrderState::reject(onRplOrderRejected const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}


void OrderState::reject(onOrderRejected const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::rejectNew(onOrderRejected const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processRejectNew(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::reject(onRecvOrderRejected const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(&orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::fill(onTradeExecution const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processFill(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::reject(onRecvRplOrderRejected const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processReject(&orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}

void OrderState::expire(onRplOrderExpired const&evnt)
{
	if(evnt.testStateMachine_)
		return;

	OrdStateImpl::processExpire(orderData_, evnt);
	evnt.order4StateMachine_ = orderData_;
}
