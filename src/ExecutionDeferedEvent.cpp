/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include "DeferedEvents.h"
#include "Processor.h"
#include "TransactionDef.h"
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

ExecutionDeferedEvent::ExecutionDeferedEvent() : baseOrder_(nullptr) {}

ExecutionDeferedEvent::ExecutionDeferedEvent(OrderEntry *ord) : baseOrder_(ord)
{
    assert(nullptr != baseOrder_);
}

void ExecutionDeferedEvent::execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope,
                                    const TradeParams &param, DateTimeT dayStart)
{
    TradeExecEntry trade;
    trade.lastQty_ = param.lastQty_;
    trade.lastPx_ = param.lastPx_;
    trade.currency_ = param.order_->currency_;
    // The business day the match counts in, as YYYYMMDD (#258): every side of it gets the same
    trade.tradeDate_ = ExpiryScheduler::tradeDateOf(dayStart);
    trade.type_ = TRADE_EXECTYPE;
    trade.transactTime_ = 1;
    trade.orderId_ = param.order_->orderId_;
    onTradeExecution evnt4Proc(&trade);
    evnt4Proc.dayStart_ = dayStart;
    evnt4Proc.orderBook_ = cnxt.orderBook_;
    evnt4Proc.transaction_ = scope;

    func->process(evnt4Proc, param.order_, cnxt);
}

void ExecutionDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
    assert(nullptr != func);
    assert(nullptr != baseOrder_);
    assert(0 < trades_.size());

    TradeParams baseOrderParam;
    baseOrderParam.order_ = baseOrder_;
    baseOrderParam.lastQty_ = 0;
    baseOrderParam.lastPx_ = 0.0;
    PriceT lastPrint = 0.0;
    // What the base order has left before any fill: what follows the trade depends on whether its fill applied (#477)
    const QuantityT baseBefore = baseOrder_->leavesQty_;
    // Every side of the match trades in one business day (#244): the clock is read once, so a match at a day's end
    // can't count in one day for one side and in the next for the other
    const DateTimeT dayStart = ExpiryScheduler::dayStartOf(cnxt.now());
    for (TradesT::const_iterator it = trades_.begin(); it != trades_.end(); ++it)
    {
        assert(nullptr != it->order_);
        assert(0 < it->lastQty_);
        assert(0 < it->lastPx_);

        QuantityT quantBefore = it->order_->leavesQty_;
        execute(func, cnxt, scope, *it, dayStart);

        /// ToDo: should be redesigned: if execution applied - increase executed quantity
        if (quantBefore > it->order_->leavesQty_)
        {
            baseOrderParam.lastQty_ += it->lastQty_;
            baseOrderParam.lastPx_ += it->lastQty_ * it->lastPx_;
            lastPrint = it->lastPx_;
        }
    }
    if (0 < baseOrderParam.lastQty_)
    {
        baseOrderParam.lastPx_ /= baseOrderParam.lastQty_;

        execute(func, cnxt, scope, baseOrderParam, dayStart);

        // The instrument's last trade, durable with the fills (#237). The id is drawn now, from the generator every order
        // id comes from, so a held stop that arrived before this trade has a smaller one: a restart releases the stops
        // that this price reaches and that were held when it traded, if their releases were lost.
        assert(nullptr != cnxt.idGenerator_);
        std::unique_ptr<Operation> lastTrade(
            new PersistLastTradeTrOperation(baseOrder_->instrument_.getId(), lastPrint, cnxt.idGenerator_->getId()));
        scope->addOperationFirst(lastTrade);
    }

    // Then what follows the trade, if the base order's fill applied, which it runs only once a contra's has: the
    // releases of the held stops the trade's price reaches (#480), then the sweep's next match. If it didn't, the
    // sweep's end, with no release (#477). match() decided the trade before either fill ran, so it can't tell: it
    // queued the next match from the trade's quantity, so that a fill the state machine dropped was traded again
    // without end, and it released the stops the trade's price reached though the trade never happened. Queued now,
    // both land in the chain's next batch, the releases first, so the transactions run in the order they did: the
    // trade's, the releases', the next match's.
    assert(nullptr != cnxt.orderMatch_);
    cnxt.orderMatch_->afterTrade(baseOrder_, baseBefore, lastPrint, cnxt);
}
