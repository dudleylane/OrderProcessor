/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "OrderStates.h"
#include "TransactionScope.h"
#include "TrOperations.h"

using namespace std;
using namespace COP::ACID;
using namespace COP::OrdState;

template <class FSM>
void Rejected::on_entry(onReplace const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	//todo: add reject reason
	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation("", COP::REJECTED_ORDSTATUS,
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onRecvOrderRejected const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS,
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onRecvRplOrderRejected const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS,
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onExternalOrderRejected const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS,
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onOrderAccepted const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	///todo: add reject reason
	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation("", COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onOrderRejected const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onRplOrderRejected const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Rejected::on_entry(onExternalOrder const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	// unable to assign order status here - it's the first state
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	///todo: add reject reason
	std::unique_ptr<Operation> op(new CreateRejectExecReportTrOperation("", COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void New::on_entry(onExternalOrder const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;
	std::unique_ptr<Operation> op1(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS,
				COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op1);
}

template <class FSM>
void New::on_entry(onOrderReceived const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void New::on_entry(onOrderAccepted const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void New::on_entry(onReplace const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateReplaceExecReportTrOperation(evnt.origOrderId_, COP::NEW_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void New::on_entry(onNewDay const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void New::on_entry(onContinue const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Expired::on_entry(onExpired const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = EXPIRED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::EXPIRED_ORDSTATUS, COP::EXPIRED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Expired::on_entry(onRplOrderExpired const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = EXPIRED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::EXPIRED_ORDSTATUS, COP::EXPIRED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void CnclReplaced::on_entry(onExecCancel const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = CANCELED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::CANCELED_ORDSTATUS, COP::CANCEL_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void CnclReplaced::on_entry(onInternalCancel const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = CANCELED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::CANCELED_ORDSTATUS, COP::CANCEL_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void CnclReplaced::on_entry(onExecReplace const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REPLACED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateReplaceExecReportTrOperation(evnt.orderId_, COP::REPLACED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Suspended::on_entry(onSuspended const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = SUSPENDED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::SUSPENDED_ORDSTATUS, COP::SUSPENDED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void DoneForDay::on_entry(onFinished const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = DFD_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(COP::DFD_ORDSTATUS, COP::DFD_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void New::on_entry(onTradeCrctCncl const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), NEW_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void PartFill::on_entry(onTradeExecution const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateTradeExecReportTrOperation(evnt.trade(), PARTFILL_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void PartFill::on_entry(onTradeCrctCncl const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), PARTFILL_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void PartFill::on_entry(onNewDay const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(PARTFILL_ORDSTATUS, COP::RESTATED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void PartFill::on_entry(onContinue const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(PARTFILL_ORDSTATUS, COP::RESTATED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Filled::on_entry(onTradeExecution const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = FILLED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateTradeExecReportTrOperation(evnt.trade(), PARTFILL_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void Suspended::on_entry(onTradeCrctCncl const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = SUSPENDED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), SUSPENDED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void CnclReplaced::on_entry(onTradeCrctCncl const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	// Nothing to do, onTradeCrctCncl will be processed in seperate state's zone
}

template <class FSM>
void Expired::on_entry(onTradeCrctCncl const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = EXPIRED_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), EXPIRED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void DoneForDay::on_entry(onTradeCrctCncl const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = DFD_ORDSTATUS;

	std::unique_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), DFD_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void GoingCancel::on_entry(onCancelReceived const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(evnt.order4StateMachine_->status_, COP::PEND_CANCEL_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void GoingReplace::on_entry(onReplaceReceived const& evnt, FSM&){
	if(evnt.testStateMachine_)
		return;
	std::unique_ptr<Operation> op(new CreateExecReportTrOperation(evnt.order4StateMachine_->status_, COP::PEND_REPLACE_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void NoCnlReplace::on_entry(onCancelRejected const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	std::unique_ptr<Operation> op(new CancelRejectTrOperation(evnt.order4StateMachine_->status_, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

template <class FSM>
void NoCnlReplace::on_entry(onReplaceRejected const& evnt, FSM&)
{
	if(evnt.testStateMachine_)
		return;
	std::unique_ptr<Operation> op(new CancelRejectTrOperation(evnt.order4StateMachine_->status_, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

// Explicit template instantiations for OrderState (the back-end state machine type)
#include "StateMachine.h"

// Rejected state on_entry instantiations
template void Rejected::on_entry<OrderState>(onReplace const&, OrderState&);
template void Rejected::on_entry<OrderState>(onRecvOrderRejected const&, OrderState&);
template void Rejected::on_entry<OrderState>(onRecvRplOrderRejected const&, OrderState&);
template void Rejected::on_entry<OrderState>(onExternalOrderRejected const&, OrderState&);
template void Rejected::on_entry<OrderState>(onOrderAccepted const&, OrderState&);
template void Rejected::on_entry<OrderState>(onOrderRejected const&, OrderState&);
template void Rejected::on_entry<OrderState>(onRplOrderRejected const&, OrderState&);
template void Rejected::on_entry<OrderState>(onExternalOrder const&, OrderState&);

// New state on_entry instantiations
template void New::on_entry<OrderState>(onExternalOrder const&, OrderState&);
template void New::on_entry<OrderState>(onOrderReceived const&, OrderState&);
template void New::on_entry<OrderState>(onOrderAccepted const&, OrderState&);
template void New::on_entry<OrderState>(onReplace const&, OrderState&);
template void New::on_entry<OrderState>(onNewDay const&, OrderState&);
template void New::on_entry<OrderState>(onContinue const&, OrderState&);
template void New::on_entry<OrderState>(onTradeCrctCncl const&, OrderState&);

// Expired state on_entry instantiations
template void Expired::on_entry<OrderState>(onExpired const&, OrderState&);
template void Expired::on_entry<OrderState>(onRplOrderExpired const&, OrderState&);
template void Expired::on_entry<OrderState>(onTradeCrctCncl const&, OrderState&);

// CnclReplaced state on_entry instantiations
template void CnclReplaced::on_entry<OrderState>(onExecCancel const&, OrderState&);
template void CnclReplaced::on_entry<OrderState>(onInternalCancel const&, OrderState&);
template void CnclReplaced::on_entry<OrderState>(onExecReplace const&, OrderState&);
template void CnclReplaced::on_entry<OrderState>(onTradeCrctCncl const&, OrderState&);

// Suspended state on_entry instantiations
template void Suspended::on_entry<OrderState>(onSuspended const&, OrderState&);
template void Suspended::on_entry<OrderState>(onTradeCrctCncl const&, OrderState&);

// DoneForDay state on_entry instantiations
template void DoneForDay::on_entry<OrderState>(onFinished const&, OrderState&);
template void DoneForDay::on_entry<OrderState>(onTradeCrctCncl const&, OrderState&);

// PartFill state on_entry instantiations
template void PartFill::on_entry<OrderState>(onTradeExecution const&, OrderState&);
template void PartFill::on_entry<OrderState>(onTradeCrctCncl const&, OrderState&);
template void PartFill::on_entry<OrderState>(onNewDay const&, OrderState&);
template void PartFill::on_entry<OrderState>(onContinue const&, OrderState&);

// Filled state on_entry instantiations
template void Filled::on_entry<OrderState>(onTradeExecution const&, OrderState&);

// GoingCancel state on_entry instantiations
template void GoingCancel::on_entry<OrderState>(onCancelReceived const&, OrderState&);

// GoingReplace state on_entry instantiations
template void GoingReplace::on_entry<OrderState>(onReplaceReceived const&, OrderState&);

// NoCnlReplace state on_entry instantiations
template void NoCnlReplace::on_entry<OrderState>(onCancelRejected const&, OrderState&);
template void NoCnlReplace::on_entry<OrderState>(onReplaceRejected const&, OrderState&);


