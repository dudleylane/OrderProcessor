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

void Rejected::on_entry(onReplace const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	//todo: add reject reason
	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation("", COP::REJECTED_ORDSTATUS, 
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Rejected::on_entry(onRecvOrderRejected const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, 
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Rejected::on_entry(onRecvRplOrderRejected const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, 
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Rejected::on_entry(onExternalOrderRejected const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, 
				*evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Rejected::on_entry(onOrderAccepted const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	///todo: add reject reason
	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation("", COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Rejected::on_entry(onOrderRejected const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void Rejected::on_entry(onRplOrderRejected const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation(evnt.reason_, COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void Rejected::on_entry(onExternalOrder const& evnt){
	if(evnt.testStateMachine_)
		return;
	// unable to assign order status here - it's the first state
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REJECTED_ORDSTATUS;

	///todo: add reject reason
	auto_ptr<Operation> op(new CreateRejectExecReportTrOperation("", COP::REJECTED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void New::on_entry(onExternalOrder const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;
	auto_ptr<Operation> op1(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, 
				COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op1);
}

void New::on_entry(onOrderReceived const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void New::on_entry(onOrderAccepted const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void New::on_entry(onReplace const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	auto_ptr<Operation> op(new CreateReplaceExecReportTrOperation(evnt.origOrderId_, COP::NEW_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void New::on_entry(onNewDay const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void New::on_entry(onContinue const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::NEW_ORDSTATUS, COP::NEW_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Expired::on_entry(onExpired const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = EXPIRED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::EXPIRED_ORDSTATUS, COP::EXPIRED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Expired::on_entry(onRplOrderExpired const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = EXPIRED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::EXPIRED_ORDSTATUS, COP::EXPIRED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void CnclReplaced::on_entry(onExecCancel const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = CANCELED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::CANCELED_ORDSTATUS, COP::CANCEL_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void CnclReplaced::on_entry(onInternalCancel const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = CANCELED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::CANCELED_ORDSTATUS, COP::CANCEL_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void CnclReplaced::on_entry(onExecReplace const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = REPLACED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateReplaceExecReportTrOperation(evnt.orderId_, COP::REPLACED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Suspended::on_entry(onSuspended const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = SUSPENDED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::SUSPENDED_ORDSTATUS, COP::SUSPENDED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void DoneForDay::on_entry(onFinished const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = DFD_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(COP::DFD_ORDSTATUS, COP::DFD_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void New::on_entry(onTradeCrctCncl const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = NEW_ORDSTATUS;

	auto_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), NEW_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void PartFill::on_entry(onTradeExecution const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	auto_ptr<Operation> op(new CreateTradeExecReportTrOperation(evnt.trade(), PARTFILL_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void PartFill::on_entry(onTradeCrctCncl const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	auto_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), PARTFILL_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void PartFill::on_entry(onNewDay const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(PARTFILL_ORDSTATUS, COP::RESTATED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void PartFill::on_entry(onContinue const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = PARTFILL_ORDSTATUS;

	auto_ptr<Operation> op(new CreateExecReportTrOperation(PARTFILL_ORDSTATUS, COP::RESTATED_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Filled::on_entry(onTradeExecution const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = FILLED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateTradeExecReportTrOperation(evnt.trade(), PARTFILL_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void Suspended::on_entry(onTradeCrctCncl const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = SUSPENDED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), SUSPENDED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}
void CnclReplaced::on_entry(onTradeCrctCncl const& evnt){
	if(evnt.testStateMachine_)
		return;
	// Nothing to do, onTradeCrctCncl will be processed in seperate state's zone
}
void Expired::on_entry(onTradeCrctCncl const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = EXPIRED_ORDSTATUS;

	auto_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), EXPIRED_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void DoneForDay::on_entry(onTradeCrctCncl const& evnt){
	if(evnt.testStateMachine_)
		return;
	assert(nullptr != evnt.order4StateMachine_);
	evnt.order4StateMachine_->status_ = DFD_ORDSTATUS;

	auto_ptr<Operation> op(new CreateCorrectExecReportTrOperation(evnt.correct(), DFD_ORDSTATUS, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void GoingCancel::on_entry(onCancelReceived const& evnt){
	if(evnt.testStateMachine_)
		return;
	auto_ptr<Operation> op(new CreateExecReportTrOperation(evnt.order4StateMachine_->status_, COP::PEND_CANCEL_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void GoingReplace::on_entry(onReplaceReceived const& evnt){
	if(evnt.testStateMachine_)
		return;
	auto_ptr<Operation> op(new CreateExecReportTrOperation(evnt.order4StateMachine_->status_, COP::PEND_REPLACE_EXECTYPE, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void NoCnlReplace::on_entry(onCancelRejected const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	auto_ptr<Operation> op(new CancelRejectTrOperation(evnt.order4StateMachine_->status_, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}

void NoCnlReplace::on_entry(onReplaceRejected const& evnt)
{
	if(evnt.testStateMachine_)
		return;
	auto_ptr<Operation> op(new CancelRejectTrOperation(evnt.order4StateMachine_->status_, *evnt.order4StateMachine_));
	evnt.transaction_->addOperation(op);
}



