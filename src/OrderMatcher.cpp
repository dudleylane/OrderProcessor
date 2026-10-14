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

    if (0 == contrOrd->leavesQty_)
    {
        return false;
    }
    // A swap trades only with a swap, on both legs (FindOppositeSwapOrder). An outright order passes over one, and keeps
    // looking: it used to take a swap by its near price alone, and filled the swap's near leg without its far leg (#212).
    if (FXSWAP_ORDERTYPE == contrOrd->ordType_)
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

/// Functor to find a matching contra FX Swap order.
/// Checks: same instrument, opposite side, both FXSWAP, matching settlement dates,
/// near-leg and far-leg price compatibility.
class FindOppositeSwapOrder : public OrderFunctor
{
public:
    FindOppositeSwapOrder(OrderEntry *order, OrderDataStorage *orderStorage)
        : order_(order), orderStorage_(orderStorage)
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
    virtual SourceIdT instrument() const
    {
        return order_->instrument_.getId();
    }
    virtual bool match(const IdT &order, bool *stop) const;

private:
    OrderEntry *order_;
    OrderDataStorage *orderStorage_;
};

bool FindOppositeSwapOrder::match(const IdT &order, bool *stop) const
{
    OrderEntry *contrOrd = orderStorage_->locateByOrderId(order);
    if (nullptr == contrOrd)
    {
        return false;
    }

    // read lock on contra order during field reads
    oneapi::tbb::spin_rw_mutex::scoped_lock contrLock(contrOrd->entryMutex_, false);

    // Must be a swap order with remaining quantity: the search passes over the rest of the side
    if ((FXSWAP_ORDERTYPE != contrOrd->ordType_) || (0 == contrOrd->leavesQty_))
    {
        return false;
    }
    if (OrderMatcher::tradesWith(*order_, *contrOrd))
    {
        return true;
    }
    // The side is sorted by near price: a swap of the same dates whose near price doesn't cross ends the search, as no
    // swap further on crosses either. One with other dates, or whose far price doesn't cross, doesn't end it.
    if ((contrOrd->settlDate_ == order_->settlDate_) && (contrOrd->farSettlDate_ == order_->farSettlDate_) &&
        !pricesCross(*order_, *contrOrd))
    {
        assert(nullptr != stop);
        *stop = true;
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
    if ((MARKET_ORDERTYPE == order.ordType_) || (MARKET_ORDERTYPE == contra.ordType_))
    {
        return true;
    }
    return pricesCross(order, contra);
}

IdT OrderMatcher::findContra(OrderEntry *order, const OrderBook &book, OrderDataStorage *storage)
{
    if (FXSWAP_ORDERTYPE == order->ordType_)
    {
        return book.find(FindOppositeSwapOrder(order, storage));
    }
    return book.find(FindOpositeOrder(order, storage));
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
        if (MARKET_ORDERTYPE == order->ordType_)
        {
            /// if market not available - market order should be canceled
            aux::ExchLogger::instance()->error("Unable to find oposite order for Market order during order matching.");
            std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
            defEvnt->cancelReason_ = "Unable to find oposite order for Market order during order matching.";
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

    /// add another match event to continue matching
    if (remainingQty > 0)
    {
        std::unique_ptr<MatchOrderDeferedEvent> defEvnt(new MatchOrderDeferedEvent(order));
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
        // No swap match — rest on book (swaps don't auto-cancel like MARKET orders)
        if (aux::ExchLogger::instance()->isNoteOn())
        {
            aux::ExchLogger::instance()->note(
                "Unable to find opposite swap order during matching, order rests on book.");
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

    QuantityT remainingQty = order->leavesQty_ - matchQty;

    secondLock.release();
    firstLock.release();

    defered_->addDeferedEvent(defEvnt.release());

    /// add another swap match event to continue matching remaining quantity
    if (remainingQty > 0)
    {
        std::unique_ptr<MatchSwapOrderDeferedEvent> defEvnt2(new MatchSwapOrderDeferedEvent(order));
        defEvnt2->order_ = order;
        defered_->addDeferedEvent(defEvnt2.release());
    }
}
