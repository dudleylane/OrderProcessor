/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <memory>
#include "TransactionDef.h"
#include "StateMachine.h"

namespace COP
{

struct OrderEntry;

namespace Proc
{

class DeferedEventContainer;

class OrderMatcher
{
public:
    OrderMatcher(void);
    ~OrderMatcher(void);

    void init(DeferedEventContainer *cont);
    void match(OrderEntry *order, const ACID::Context &ctxt);
    /// The first match of an order, as its booking or its release runs it (#240): a FOK order the book can't fill at
    /// once is canceled with nothing traded; any other is matched. A sweep continues with match().
    void matchNew(OrderEntry *order, const ACID::Context &ctxt);
    /// What follows one of order's trades, at lastPx, once the trade's event has decided its fills (#477), given what
    /// order had left before them. match() decides a trade from the orders' leaves before either fill runs, so what
    /// follows waits for order's fill. If it applied, the held stops lastPx reaches are released (#480), and the sweep
    /// goes on: with something left, match() again. A fill that didn't apply, which the state machine drops (#432), or
    /// a trade the event refused because order couldn't take it whole (#483), releases nothing and ends the sweep as a
    /// match that finds nothing to trade with ends it: a working order that never rests is canceled with what it has
    /// left, any other working order rests with it, and one that isn't working is left as it is.
    void afterTrade(OrderEntry *order, QuantityT leavesBefore, PriceT lastPx, const ACID::Context &ctxt);
    /// afterTrade() for a swap, once its trade's event has decided both sides' near legs (#477): if the swap's fill
    /// applied and left it something, its sweep goes on (matchSwap() again). A fill that didn't apply ends the sweep as
    /// finding no swap to trade with ends it (#485): an IOC or FOK swap is canceled with what it has left, and any
    /// other rests with it.
    void afterSwapTrade(OrderEntry *order, QuantityT leavesBefore);
    /// Whether the book holds enough, on the other side of order's instrument, of what order trades with to fill it at
    /// once: the orders findContra() would take, in the order it takes them, passing over what it passes over (#491).
    static bool canFillAtOnce(OrderEntry *order, const OrderBook &book, Store::OrderDataStorage *storage);

    /// The order the matcher trades order with first: the best working one (OrderParams::working(), #432) on the other
    /// side of its instrument's book that it trades with, or an invalid id when there is none. match() starts from it,
    /// and a restore uses it to find the orders that would have traded with a booked one (#207), so the two decide
    /// alike.
    static IdT findContra(OrderEntry *order, const OrderBook &book, Store::OrderDataStorage *storage);
    /// Whether order trades with contra, an order on the other side of its instrument, by their types and prices, and a
    /// swap's dates: the test findContra() applies to each working order it comes to. Neither quantity nor status is
    /// part of it: an order that isn't working trades with nothing, and the caller checks that.
    static bool tradesWith(const OrderEntry &order, const OrderEntry &contra);

private:
    void matchSwap(OrderEntry *order, const ACID::Context &ctxt);
    std::unique_ptr<OrdState::OrderState> stateMachine_;
    DeferedEventContainer *defered_;
};

} // namespace Proc
} // namespace COP