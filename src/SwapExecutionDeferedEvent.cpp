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
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "TrOperations.h"
#include "ExpiryScheduler.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

SwapExecutionDeferedEvent::SwapExecutionDeferedEvent() : baseOrder_(nullptr) {}

SwapExecutionDeferedEvent::SwapExecutionDeferedEvent(OrderEntry *ord) : baseOrder_(ord)
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

    // What the swap has left before any fill: whether its sweep goes on depends on whether its fill applied (#477)
    const QuantityT baseBefore = baseOrder_->leavesQty_;

    // Stage the swap — if either leg fails, remove the stage and cancel
    size_t stageId = scope->startNewStage();

    // A link per side of this fill, which its near and its far report both carry, so a client can pair them (#199). The
    // FIX gateway made one from each report's own transact time, and two reports a millisecond apart didn't pair. Each
    // side has its own: one shared link would tell each client whose order it traded with.
    assert(nullptr != cnxt.idGenerator_);
    const u64 contraLink = cnxt.idGenerator_->getId().id_;
    const u64 baseLink = cnxt.idGenerator_->getId().id_;
    // Both sides' near legs trade in one business day (#244), as an outright match's sides do: the clock is read once
    const DateTimeT dayStart = ExpiryScheduler::dayStartOf(cnxt.now());
    // And on its date, as an outright trade does (#258): both legs, on both sides. The legs settle on different dates,
    // but they trade on one. Every swap leg's trade said 1 (#445).
    const DateTimeT tradeDate = ExpiryScheduler::tradeDateOf(dayStart);

    try
    {
        // === CONTRA ORDER ===

        // 1. Near-leg fill on contra order (invokes state machine → processFill → applyTrade)
        TradeExecEntry nearTradeEntry;
        nearTradeEntry.lastQty_ = nearTrade_.lastQty_;
        nearTradeEntry.lastPx_ = nearTrade_.lastPx_;
        nearTradeEntry.currency_ = nearTrade_.order_->currency_;
        nearTradeEntry.tradeDate_ = tradeDate;
        nearTradeEntry.type_ = TRADE_EXECTYPE;
        nearTradeEntry.transactTime_ = 1;
        nearTradeEntry.orderId_ = nearTrade_.order_->orderId_;
        nearTradeEntry.execLegType_ = NEAR_LEG;
        nearTradeEntry.tradeLink_ = contraLink;

        onTradeExecution contraEvnt(&nearTradeEntry);
        contraEvnt.dayStart_ = dayStart;
        contraEvnt.orderBook_ = cnxt.orderBook_;
        contraEvnt.transaction_ = scope;
        // What the contra has left before its fill: the swap trades only if that fill applied (#481)
        const QuantityT contraBefore = nearTrade_.order_->leavesQty_;
        func->process(contraEvnt, nearTrade_.order_, cnxt);

        // The rest only if the contra's fill applied, as an outright trade's base is filled only with what its contras'
        // fills applied (#481). One the state machine dropped (no row for it, or its action threw, counted and logged
        // since #432) left the contra as it was, so nothing traded: neither side's far leg is reported, and the swap's
        // near leg isn't filled. With its leaves as they were, afterSwapTrade() ends its sweep (#477). Until #481 the
        // swap was filled whatever the contra's fill did, and both far legs were reported: a one-sided trade.
        if (contraBefore > nearTrade_.order_->leavesQty_)
        {
            // 2. Far-leg execution REPORT on contra order (no state machine, no leavesQty_ change). It reports what the
            // near leg's fill left the order with (#411), taken here, under the order's lock: nothing else decides
            // about the order meanwhile, since every transaction that does names its instrument, as the one draining
            // this fill does. The report goes out when this fill's transaction runs, which can be after a later fill of
            // the order.
            std::unique_ptr<Operation> contraFarOp;
            {
                oneapi::tbb::spin_rw_mutex::scoped_lock lock(nearTrade_.order_->entryMutex_, false);
                OrderStatus contraStatus = (nearTrade_.order_->leavesQty_ == 0) ? FILLED_ORDSTATUS : PARTFILL_ORDSTATUS;
                contraFarOp.reset(new CreateTradeExecReportTrOperation(farTrade_.lastQty_, farTrade_.lastPx_, tradeDate,
                                                                       nearTrade_.order_->currency_, contraStatus,
                                                                       FAR_LEG, contraLink, *nearTrade_.order_));
            }
            scope->addOperation(contraFarOp);

            // === BASE (AGGRESSOR) ORDER ===

            // 3. Near-leg fill on base order (invokes state machine → processFill → applyTrade)
            TradeExecEntry baseNearTradeEntry;
            baseNearTradeEntry.lastQty_ = nearTrade_.lastQty_;
            baseNearTradeEntry.lastPx_ = nearTrade_.lastPx_;
            baseNearTradeEntry.currency_ = baseOrder_->currency_;
            baseNearTradeEntry.tradeDate_ = tradeDate;
            baseNearTradeEntry.type_ = TRADE_EXECTYPE;
            baseNearTradeEntry.transactTime_ = 1;
            baseNearTradeEntry.orderId_ = baseOrder_->orderId_;
            baseNearTradeEntry.execLegType_ = NEAR_LEG;
            baseNearTradeEntry.tradeLink_ = baseLink;

            onTradeExecution baseEvnt(&baseNearTradeEntry);
            baseEvnt.dayStart_ = dayStart;
            baseEvnt.orderBook_ = cnxt.orderBook_;
            baseEvnt.transaction_ = scope;
            func->process(baseEvnt, baseOrder_, cnxt);

            // 4. Far-leg execution REPORT on base order (no state machine, no leavesQty_ change), with what its near
            // leg's fill left it with, as the contra's (#411)
            std::unique_ptr<Operation> baseFarOp;
            {
                oneapi::tbb::spin_rw_mutex::scoped_lock lock(baseOrder_->entryMutex_, false);
                OrderStatus baseStatus = (baseOrder_->leavesQty_ == 0) ? FILLED_ORDSTATUS : PARTFILL_ORDSTATUS;
                baseFarOp.reset(new CreateTradeExecReportTrOperation(farTrade_.lastQty_, farTrade_.lastPx_, tradeDate,
                                                                     baseOrder_->currency_, baseStatus, FAR_LEG,
                                                                     baseLink, *baseOrder_));
            }
            scope->addOperation(baseFarOp);
        }
    }
    catch (const std::exception &)
    {
        // Atomic rollback: remove all operations added in this stage
        scope->removeStage(stageId);

        // Cancel the swap order since atomic execution failed
        std::unique_ptr<CancelOrderDeferedEvent> cancelEvnt(new CancelOrderDeferedEvent(baseOrder_));
        cancelEvnt->cancelReason_ = "FX Swap: atomic two-leg execution failed.";
        cancelEvnt->order_ = baseOrder_;
        cnxt.deferedEvents_->addDeferedEvent(cancelEvnt.release());
        return; // canceled: its sweep ends here
    }

    // Then what follows the trade, as an outright trade's event decides it (#477): the sweep's next match if the swap's
    // fill applied, or its end if it didn't. matchSwap() queued the next match from the trade's quantity, before either
    // fill ran. Queued now, it lands in the chain's next batch, so the transactions run in the order they did.
    assert(nullptr != cnxt.orderMatch_);
    cnxt.orderMatch_->afterSwapTrade(baseOrder_, baseBefore);
}
