/**
 Concurrent Order Processor library

 FX Swap Support: SwapExecutionDeferedEvent — atomic two-leg execution

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <cassert>
#include <memory>
#include "DeferedEvents.h"
#include "Processor.h"
#include "TransactionDef.h"
#include "TransactionScope.h"
#include "DataModelDef.h"
#include "OrderStorage.h"
#include "TrOperations.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

SwapExecutionDeferedEvent::SwapExecutionDeferedEvent(): baseOrder_(nullptr)
{}

SwapExecutionDeferedEvent::SwapExecutionDeferedEvent(OrderEntry *ord): baseOrder_(ord)
{
	assert(nullptr != baseOrder_);
}

void SwapExecutionDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
	assert(nullptr != func);
	assert(nullptr != baseOrder_);
	assert(nullptr != scope);
	assert(nullptr != nearTrade_.order_);
	assert(nullptr != farTrade_.order_);
	assert(nearTrade_.lastQty_ > 0);

	// Stage the swap — if either leg fails, remove the stage and cancel
	size_t stageId = scope->startNewStage();

	try{
		// === CONTRA ORDER ===

		// 1. Near-leg fill on contra order (invokes state machine → processFill → applyTrade)
		TradeExecEntry nearTradeEntry;
		nearTradeEntry.lastQty_ = nearTrade_.lastQty_;
		nearTradeEntry.lastPx_ = nearTrade_.lastPx_;
		nearTradeEntry.currency_ = nearTrade_.order_->currency_;
		nearTradeEntry.tradeDate_ = 1;
		nearTradeEntry.type_ = TRADE_EXECTYPE;
		nearTradeEntry.transactTime_ = 1;
		nearTradeEntry.orderId_ = nearTrade_.order_->orderId_;
		nearTradeEntry.execLegType_ = NEAR_LEG;

		onTradeExecution contraEvnt(&nearTradeEntry);
		contraEvnt.orderBook_ = cnxt.orderBook_;
		contraEvnt.transaction_ = scope;
		func->process(contraEvnt, nearTrade_.order_, cnxt);

		// 2. Far-leg execution REPORT on contra order (no state machine, no leavesQty_ change)
		OrderStatus contraStatus = (nearTrade_.order_->leavesQty_ == 0)
			? FILLED_ORDSTATUS : PARTFILL_ORDSTATUS;
		std::unique_ptr<Operation> contraFarOp(
			new CreateTradeExecReportTrOperation(
				farTrade_.lastQty_, farTrade_.lastPx_, nearTrade_.order_->currency_,
				contraStatus, FAR_LEG, *nearTrade_.order_));
		scope->addOperation(contraFarOp);

		// === BASE (AGGRESSOR) ORDER ===

		// 3. Near-leg fill on base order (invokes state machine → processFill → applyTrade)
		TradeExecEntry baseNearTradeEntry;
		baseNearTradeEntry.lastQty_ = nearTrade_.lastQty_;
		baseNearTradeEntry.lastPx_ = nearTrade_.lastPx_;
		baseNearTradeEntry.currency_ = baseOrder_->currency_;
		baseNearTradeEntry.tradeDate_ = 1;
		baseNearTradeEntry.type_ = TRADE_EXECTYPE;
		baseNearTradeEntry.transactTime_ = 1;
		baseNearTradeEntry.orderId_ = baseOrder_->orderId_;
		baseNearTradeEntry.execLegType_ = NEAR_LEG;

		onTradeExecution baseEvnt(&baseNearTradeEntry);
		baseEvnt.orderBook_ = cnxt.orderBook_;
		baseEvnt.transaction_ = scope;
		func->process(baseEvnt, baseOrder_, cnxt);

		// 4. Far-leg execution REPORT on base order (no state machine, no leavesQty_ change)
		OrderStatus baseStatus = (baseOrder_->leavesQty_ == 0)
			? FILLED_ORDSTATUS : PARTFILL_ORDSTATUS;
		std::unique_ptr<Operation> baseFarOp(
			new CreateTradeExecReportTrOperation(
				farTrade_.lastQty_, farTrade_.lastPx_, baseOrder_->currency_,
				baseStatus, FAR_LEG, *baseOrder_));
		scope->addOperation(baseFarOp);

	}catch(const std::exception &){
		// Atomic rollback: remove all operations added in this stage
		scope->removeStage(stageId);

		// Cancel the swap order since atomic execution failed
		std::unique_ptr<CancelOrderDeferedEvent> cancelEvnt(new CancelOrderDeferedEvent(baseOrder_));
		cancelEvnt->cancelReason_ = "FX Swap: atomic two-leg execution failed.";
		cancelEvnt->order_ = baseOrder_;
		cnxt.deferedEvents_->addDeferedEvent(cancelEvnt.release());
	}
}
