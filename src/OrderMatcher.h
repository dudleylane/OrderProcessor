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

    /// The order the matcher trades order with first: the best one on the other side of its instrument's book that it
    /// trades with, or an invalid id when there is none. match() starts from it, and a restore uses it to find the
    /// orders that would have traded with a booked one (#207), so the two decide alike.
    static IdT findContra(OrderEntry *order, const OrderBook &book, Store::OrderDataStorage *storage);
    /// Whether order trades with contra, an order on the other side of its instrument, by their types and prices, and a
    /// swap's dates: the test findContra() applies to each order it passes. Quantity isn't part of it: an order with
    /// nothing left trades with nothing, and the caller checks that.
    static bool tradesWith(const OrderEntry &order, const OrderEntry &contra);

private:
    void matchSwap(OrderEntry *order, const ACID::Context &ctxt);
    std::unique_ptr<OrdState::OrderState> stateMachine_;
    DeferedEventContainer *defered_;
};

} // namespace Proc
} // namespace COP