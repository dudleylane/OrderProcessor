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
/// Whether order's price reaches contra's: a buy's at or above it, a sell's at or below. For swaps, their near prices.
bool pricesCross(const OrderEntry &order, const OrderEntry &contra)
{
    return (BUY_SIDE == order.side_) ? (order.price_ >= contra.price_) : (order.price_ <= contra.price_);
}

/// What the matcher's walk of the other side of order's instrument does at contra, read-locked: takes it, passes over
/// it and goes on, or ends there, as the side is sorted by price and no order further on trades with order either.
/// The sweep's search (findContra()) and the FOK check (canFillAtOnce()) both walk by it, so the check counts what the
/// sweep would take and stops only where the sweep would stop (#491).
enum class Walk
{
    PASS,
    TAKE,
    END
};

Walk walk(const OrderEntry &order, const OrderEntry &contra)
{
    // The book holds an order the state machine took out of trading until its removal, a transaction of its own, runs:
    // a closed one has no leaves (#386), and a suspended one keeps them. Either is passed by, and the search goes on
    // (#432): the state machine has no row for a trade with either, and dropped the trade without a word.
    if (!contra.working())
    {
        return Walk::PASS;
    }
    if (OrderMatcher::tradesWith(order, contra))
    {
        return Walk::TAKE;
    }
    const bool contraIsSwap = (FXSWAP_ORDERTYPE == contra.ordType_);
    if (FXSWAP_ORDERTYPE != order.ordType_)
    {
        // A swap trades only with a swap, on both legs: an outright order passes over one, and keeps looking. It used to
        // take a swap by its near price alone, and filled the swap's near leg without its far leg (#212). Any other
        // order it doesn't trade with ends the walk.
        return contraIsSwap ? Walk::PASS : Walk::END;
    }
    // A swap passes over an outright order, a swap of other dates, and one whose far price doesn't cross. A swap of the
    // same dates whose near price doesn't cross ends the walk: the side is sorted by near price, so no swap further on
    // crosses either.
    if (contraIsSwap && (contra.settlDate_ == order.settlDate_) && (contra.farSettlDate_ == order.farSettlDate_) &&
        !pricesCross(order, contra))
    {
        return Walk::END;
    }
    return Walk::PASS;
}

/// The sweep's search, an outright order's and a swap's: the first order on the other side of order's instrument that
/// walk() takes
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
/// it enough to fill the order (#240). It walks by walk(), as the sweep does: until #491 it ended at the first working
/// order the order didn't trade with, where on this branch the sweep passes over a swap, an outright order, or a swap
/// of other dates or a far price that doesn't cross, so a FOK order the book could fill was canceled.
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
        switch (walk(*order_, *contrOrd))
        {
        case Walk::TAKE:
            found_ += contrOrd->leavesQty_;
            return found_ >= needed_;
        case Walk::END:
            *stop = true;
            return false;
        case Walk::PASS:
            break;
        }
        return false;
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
    switch (walk(*order_, *contrOrd))
    {
    case Walk::TAKE:
        return true;
    case Walk::END:
        assert(nullptr != stop);
        *stop = true;
        return false;
    case Walk::PASS:
        break;
    }
    return false;
}
} // namespace

bool OrderMatcher::tradesWith(const OrderEntry &order, const OrderEntry &contra)
{
    // A swap trades only with a swap (#212), on both legs: the same near and far dates, the near prices crossing, and the
    // far prices crossing the other way, since the buyer of the near leg sells the far one
    const bool swap = (FXSWAP_ORDERTYPE == order.ordType_);
    if (swap != (FXSWAP_ORDERTYPE == contra.ordType_))
    {
        return false;
    }
    if (swap)
    {
        if ((contra.settlDate_ != order.settlDate_) || (contra.farSettlDate_ != order.farSettlDate_) ||
            !pricesCross(order, contra))
        {
            return false;
        }
        if (BUY_SIDE == order.side_)
        {
            return order.farPrice_ <= contra.farPrice_;
        }
        return order.farPrice_ >= contra.farPrice_;
    }
    // A stop order its stop released trades as a market order if it is a STOP, and at its limit if a STOPLIMIT (#223)
    if (order.marketLike() || contra.marketLike())
    {
        return true;
    }
    return pricesCross(order, contra);
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

    // Dispatch to swap-specific matching for FX Swap orders
    if (FXSWAP_ORDERTYPE == order->ordType_)
    {
        matchSwap(order, ctxt);
        return;
    }

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

    secondLock.release();
    firstLock.release();

    // What follows the trade, the held stops its price reaches and the sweep's next match or its end, waits for the
    // trade's fills: once they are decided, the trade's event hands the order to afterTrade() (#477, #480)
    defered_->addDeferedEvent(defEvnt.release());
}

void OrderMatcher::afterTrade(OrderEntry *order, QuantityT leavesBefore, PriceT lastPx, const Context &ctxt)
{
    assert(nullptr != order);
    assert(nullptr != ctxt.orderBook_);
    assert(nullptr != ctxt.orderStorage_);
    QuantityT leaves = 0;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
        leaves = order->leavesQty_;
    }
    if (leaves < leavesBefore) [[likely]]
    {
        // The fill applied, all of it (OrderParams::applyTrade() takes a trade whole or not at all), credited with what
        // the contra's applied: the trade happened, at lastPx. The held stops that price reaches are released after it
        // (#223), each in a transaction of its own, which relates to the instrument as the transaction whose chain
        // this is does, so it runs once that one is done, with every fill its chain decides. Each is claimed, so that a
        // sweep releases a stop once, whichever of its trades reaches it. Queued ahead of the next match, as they were
        // queued with the trade until #480, so the transactions run in the order they did.
        assert(0 < lastPx);
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
        // Then the sweep goes on with what the order has left
        if (0 < leaves)
        {
            std::unique_ptr<MatchOrderDeferedEvent> defEvnt(new MatchOrderDeferedEvent(order));
            defEvnt->order_ = order;
            defered_->addDeferedEvent(defEvnt.release());
        }
        return;
    }
    // The fill didn't apply: the state machine dropped it, and counted and logged why (#432), or the trade's event
    // refused the trade, filling neither side, as the order couldn't take it whole (#483). Matched again, the order
    // would trade the same quantity with the same order, without end, so the sweep ends here, as one that finds nothing
    // to trade with ends. A working order that never rests is canceled with what it has left: a FOK order that traded
    // part of itself before is canceled with the rest, which beats leaving it working and unbooked. Any other working
    // order rests with it, where it crosses the order it couldn't trade with. One that isn't working, which only a
    // refused trade hands here, is left as it is: a closed order has no row for the cancel, and a suspended or
    // done-for-day one is out of trading for now, not done with it. No stop is released: a trade's price reaches the
    // stops only once the trade has happened (#480).
    bool cancels = false;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
        cancels = order->working() && order->cancelsWhatIsLeft();
    }
    if (cancels)
    {
        std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
        defEvnt->cancelReason_ = "A trade's fill didn't apply: what the order has left is canceled";
        defEvnt->order_ = order;
        defered_->addDeferedEvent(defEvnt.release());
    }
}

void OrderMatcher::matchSwap(OrderEntry *order, const Context &ctxt)
{
    assert(nullptr != order);
    assert(nullptr != ctxt.orderBook_);
    assert(nullptr != ctxt.orderStorage_);
    assert(FXSWAP_ORDERTYPE == order->ordType_);

    IdT id = findContra(order, *ctxt.orderBook_, ctxt.orderStorage_);
    if (!id.isValid())
    {
        // An IOC or FOK swap never rests: what it couldn't trade at once is canceled, as match() cancels an outright
        // order's (#485). It was left open, out of the book, where no swap finds it. Any other swap rests: its booking
        // books it once this match is done (addBooking()).
        if (order->cancelsWhatIsLeft())
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
                aux::ExchLogger::instance()->note("Unable to find opposite swap order during matching, stop matching.");
            }
        }
        return;
    }

    OrderEntry *contrOrd = ctxt.orderStorage_->locateByOrderId(id);
    if (nullptr == contrOrd)
    {
        throw std::runtime_error("Unable to retrieve swap contra order from OrderStorage after search!");
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

    QuantityT matchQty = std::min(order->leavesQty_, contrOrd->leavesQty_);

    /// create swap execution deferred event with near and far trade params
    std::unique_ptr<SwapExecutionDeferedEvent> defEvnt(new SwapExecutionDeferedEvent(order));
    defEvnt->nearTrade_.order_ = contrOrd;
    defEvnt->nearTrade_.lastQty_ = matchQty;
    defEvnt->nearTrade_.lastPx_ = contrOrd->price_;

    defEvnt->farTrade_.order_ = contrOrd;
    defEvnt->farTrade_.lastQty_ = matchQty;
    defEvnt->farTrade_.lastPx_ = contrOrd->farPrice_;

    secondLock.release();
    firstLock.release();

    // What follows the trade, the sweep's next match or its end, waits for the trade's fills, as an outright order's
    // does: once they are decided, the trade's event hands the swap to afterSwapTrade() (#477)
    defered_->addDeferedEvent(defEvnt.release());
}

void OrderMatcher::afterSwapTrade(OrderEntry *order, QuantityT leavesBefore)
{
    assert(nullptr != order);
    assert(FXSWAP_ORDERTYPE == order->ordType_);
    QuantityT leaves = 0;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
        leaves = order->leavesQty_;
    }
    if (leaves < leavesBefore) [[likely]]
    {
        // The fill applied, all of it, so the sweep goes on with what the swap has left
        if (0 < leaves)
        {
            std::unique_ptr<MatchSwapOrderDeferedEvent> defEvnt(new MatchSwapOrderDeferedEvent(order));
            defEvnt->order_ = order;
            defered_->addDeferedEvent(defEvnt.release());
        }
        return;
    }
    // The fill didn't apply: the state machine dropped the contra's near fill, so nothing traded (#481), or the swap's
    // own, and counted and logged why (#432). Matched again, the swap would take the same swap without end, so the
    // sweep ends here, as one that finds no swap to trade with ends, and the same way (#485): an IOC or FOK swap is
    // canceled with what it has left, as afterTrade() cancels an outright order's, and any other rests with it,
    // crossing the swap it couldn't trade with. Until #485 every swap rested, whatever its time in force.
    if (order->cancelsWhatIsLeft())
    {
        std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
        defEvnt->cancelReason_ = "A trade's fill didn't apply: what the order has left is canceled";
        defEvnt->order_ = order;
        defered_->addDeferedEvent(defEvnt.release());
    }
}
