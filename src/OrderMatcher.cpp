/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#define NOMINMAX

#include <stdexcept>
#include <cassert>
#include <algorithm>
#include "StateMachine.h"
#include "OrderMatcher.h"
#include "TransactionDef.h"
#include "TransactionScope.h"
#include "DataModelDef.h"
#include "OrderStorage.h"
#include "DeferedEvents.h"
#include "Logger.h"

using namespace std;
using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

namespace
{
class FindOpositeOrder : public OrderFunctor
{
public:
    FindOpositeOrder(OrderEntry *order, OrderDataStorage *orderStorage) : order_(order), orderStorage_(orderStorage)
    {
        assert(nullptr != order_);
        assert(nullptr != orderStorage_);
        if (BUY_SIDE == order_->side_)
        {
            side_ = SELL_SIDE;
        }
        else
        {
            side_ = BUY_SIDE;
        }
    }
    virtual SourceIdT instrument() const;
    virtual bool match(const IdT &order, bool *stop) const;

private:
    OrderEntry *order_;
    OrderDataStorage *orderStorage_;
};

/// Walks the orders findContra() would take, in its order, adding up what they have left, and finds the one that makes
/// it enough to fill the order (#240)
class FindFillAtOnce : public OrderFunctor
{
public:
    FindFillAtOnce(OrderEntry *order, OrderDataStorage *orderStorage)
        : order_(order), orderStorage_(orderStorage), needed_(order->leavesQty_)
    {
        side_ = (BUY_SIDE == order_->side_) ? SELL_SIDE : BUY_SIDE;
    }
    SourceIdT instrument() const override
    {
        return order_->instrument_.getId();
    }
    bool match(const IdT &order, bool *stop) const override
    {
        OrderEntry *contrOrd = orderStorage_->locateByOrderId(order);
        if (nullptr == contrOrd)
        {
            return false;
        }
        oneapi::tbb::spin_rw_mutex::scoped_lock contrLock(contrOrd->entryMutex_, false);
        if (!contrOrd->working())
        {
            return false; // passed by, as findContra() passes it by: what it has left fills nothing (#432)
        }
        if (!OrderMatcher::tradesWith(*order_, *contrOrd))
        {
            *stop = true; // sorted by price: no order further on trades with it either
            return false;
        }
        found_ += contrOrd->leavesQty_;
        return found_ >= needed_;
    }

private:
    OrderEntry *order_;
    OrderDataStorage *orderStorage_;
    const u64 needed_;
    mutable u64 found_ = 0;
};

SourceIdT FindOpositeOrder::instrument() const
{
    return order_->instrument_.getId();
}

bool FindOpositeOrder::match(const IdT &order, bool *stop) const
{
    OrderEntry *contrOrd = orderStorage_->locateByOrderId(order);
    if (nullptr == contrOrd)
    {
        return false;
    }

    // read lock on contra order during field reads
    oneapi::tbb::spin_rw_mutex::scoped_lock contrLock(contrOrd->entryMutex_, false);

    // The book holds an order the state machine took out of trading until its removal, a transaction of its own, runs:
    // a closed one has no leaves (#386), and a suspended one keeps them. Either is passed by, and the search goes on
    // (#432): the state machine has no row for a trade with either, and dropped the trade without a word.
    if (!contrOrd->working())
    {
        return false;
    }
    if (OrderMatcher::tradesWith(*order_, *contrOrd))
    {
        return true;
    }
    // The side is sorted by price, so no order further on trades with it either
    assert(nullptr != stop);
    *stop = true;
    return false;
}
} // namespace

bool OrderMatcher::tradesWith(const OrderEntry &order, const OrderEntry &contra)
{
    // A stop order its stop released trades as a market order if it is a STOP, and at its limit if a STOPLIMIT (#223)
    if (order.marketLike() || contra.marketLike())
    {
        return true;
    }
    if (BUY_SIDE == order.side_)
    {
        return order.price_ >= contra.price_;
    }
    return order.price_ <= contra.price_;
}

IdT OrderMatcher::findContra(OrderEntry *order, const OrderBook &book, OrderDataStorage *storage)
{
    return book.find(FindOpositeOrder(order, storage));
}

bool OrderMatcher::canFillAtOnce(OrderEntry *order, const OrderBook &book, OrderDataStorage *storage)
{
    assert(nullptr != order);
    return (0 < order->leavesQty_) && book.find(FindFillAtOnce(order, storage)).isValid();
}

void OrderMatcher::matchNew(OrderEntry *order, const Context &ctxt)
{
    assert(nullptr != order);
    assert(nullptr != ctxt.orderBook_);
    // Fill or kill (#240): in full at once, or not at all. The book can't change between this and the sweep: they run in
    // one transaction, and every transaction that books, cancels or matches on the instrument relates to it.
    if ((FOK_TIF == order->tif_) && !canFillAtOnce(order, *ctxt.orderBook_, ctxt.orderStorage_))
    {
        std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
        defEvnt->cancelReason_ = "Fill or kill: the book can't fill the order at once";
        defEvnt->order_ = order;
        defered_->addDeferedEvent(defEvnt.release());
        return;
    }
    match(order, ctxt);
}

OrderMatcher::OrderMatcher(void) : defered_(nullptr)
{
    aux::ExchLogger::instance()->note("OrderMatcher created.");
}

OrderMatcher::~OrderMatcher(void)
{
    aux::ExchLogger::instance()->note("OrderMatcher destroyed.");
}

void OrderMatcher::init(DeferedEventContainer *cont)
{
    assert(nullptr != cont);
    defered_ = cont;

    stateMachine_ = std::make_unique<OrderState>();
    stateMachine_->start();
    aux::ExchLogger::instance()->note("OrderMatcher initialized.");
}

void OrderMatcher::match(OrderEntry *order, const Context &ctxt)
{
    assert(nullptr != order);
    assert(nullptr != ctxt.orderBook_);
    assert(nullptr != ctxt.orderStorage_);

    IdT id = findContra(order, *ctxt.orderBook_, ctxt.orderStorage_);
    if (!id.isValid())
    {
        // A STOP its stop released is cancelled with what is left, as a market order is (#223), and so is an IOC or FOK
        // order, which never rests (#240)
        if (order->marketLike())
        {
            /// if market not available - market order should be canceled
            aux::ExchLogger::instance()->error("Unable to find oposite order for Market order during order matching.");
            std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
            defEvnt->cancelReason_ = "Unable to find oposite order for Market order during order matching.";
            defEvnt->order_ = order;
            defered_->addDeferedEvent(defEvnt.release());
        }
        else if (order->cancelsWhatIsLeft())
        {
            std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
            defEvnt->cancelReason_ = "Immediate or cancel: what the order couldn't trade at once is canceled";
            defEvnt->order_ = order;
            defered_->addDeferedEvent(defEvnt.release());
        }
        else
        {
            if (aux::ExchLogger::instance()->isNoteOn())
            {
                aux::ExchLogger::instance()->note("Unable to find oposite order during order matching, stop matching.");
            }
        }
        return;
    }

    OrderEntry *contrOrd = ctxt.orderStorage_->locateByOrderId(id);
    if (nullptr == contrOrd)
    {
        throw std::runtime_error("Unable to retrive order from OrderStorage after search!");
    }

    /// read lock both orders with deadlock prevention (ascending orderId_)
    OrderEntry *first = order;
    OrderEntry *second = contrOrd;
    if (contrOrd->orderId_ < order->orderId_)
    {
        std::swap(first, second);
    }
    oneapi::tbb::spin_rw_mutex::scoped_lock firstLock(first->entryMutex_, false);
    oneapi::tbb::spin_rw_mutex::scoped_lock secondLock(second->entryMutex_, false);

    /// add trade event
    std::unique_ptr<ExecutionDeferedEvent> defEvnt(new ExecutionDeferedEvent(order));
    TradeParams trade;
    trade.order_ = contrOrd;
    trade.lastQty_ = std::min(order->leavesQty_, contrOrd->leavesQty_);
    trade.lastPx_ = contrOrd->price_;
    if (0 == trade.lastPx_ && 0 == order->price_)
    {
        throw std::runtime_error("OrderMatcher: cannot match two market orders - no reference price available!");
    }
    defEvnt->trades_.push_back(trade);

    QuantityT remainingQty = order->leavesQty_ - trade.lastQty_;

    secondLock.release();
    firstLock.release();

    defered_->addDeferedEvent(defEvnt.release());

    // The held stops this trade reaches are released after it (#223): each in a transaction of its own, which runs once
    // this one is done, since both relate to the instrument. Queued after the trade, so a trade whose fill throws,
    // which throws away the rest of the chain (Processor::processDeferedEvent), releases nothing, and a rolled-back
    // match drops them with its other events (MatchOrderTrOperation::rollback).
    const PriceT lastPx = trade.lastPx_;
    const SourceIdT instrument = order->instrument_.getId();
    if (ctxt.orderBook_->holdsStops(instrument)) [[unlikely]]
    {
        OrderBook::OrdersT reached;
        ctxt.orderBook_->triggeredStops(instrument, lastPx, &reached);
        for (const IdT &stopId : reached)
        {
            OrderEntry *stop = ctxt.orderStorage_->locateByOrderId(stopId);
            if ((nullptr != stop) && defered_->claimStopRelease(stopId))
            {
                defered_->addDeferedEvent(new ReleaseStopDeferedEvent(stop));
            }
        }
    }

    /// add another match event to continue matching
    if (remainingQty > 0)
    {
        std::unique_ptr<MatchOrderDeferedEvent> defEvnt(new MatchOrderDeferedEvent(order));
        defEvnt->order_ = order;
        defered_->addDeferedEvent(defEvnt.release());
    }
}
