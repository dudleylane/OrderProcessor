/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <deque>
#include "TypesDef.h"
#include "TransactionDef.h"
#include "OrderStateEvents.h"

namespace COP
{
struct OrderEntry;

namespace Proc
{

class DeferedEventFunctor
{
public:
    virtual ~DeferedEventFunctor() {}

    virtual void process(OrdState::onTradeExecution &evnt, OrderEntry *order, const ACID::Context &cnxt) = 0;
    virtual void process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context &cnxt) = 0;
};

class DeferedEventBase
{
public:
    virtual ~DeferedEventBase() {}

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope) = 0;
};

class DeferedEventContainer
{
public:
    virtual ~DeferedEventContainer() {}
    virtual void addDeferedEvent(DeferedEventBase *evnt) = 0;
    virtual size_t deferedEventCount() const = 0;
    virtual void removeDeferedEventsFrom(size_t startIndex) = 0;
};

struct TradeParams
{
    // trade's quantity
    QuantityT lastQty_;
    // trade's price
    PriceT lastPx_;
    // order's id that changed according execution
    OrderEntry *order_;
};

typedef std::deque<TradeParams> TradesT;

struct ExecutionDeferedEvent : public DeferedEventBase
{
    TradesT trades_;
    OrderEntry *baseOrder_;

    ExecutionDeferedEvent();
    explicit ExecutionDeferedEvent(OrderEntry *ord);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);

private:
    void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope, const TradeParams &param);
};

struct MatchOrderDeferedEvent : public DeferedEventBase
{
    OrderEntry *order_;

    MatchOrderDeferedEvent();
    explicit MatchOrderDeferedEvent(OrderEntry *ord);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

struct CancelOrderDeferedEvent : public DeferedEventBase
{
    OrderEntry *order_;
    std::string cancelReason_;

    CancelOrderDeferedEvent();
    explicit CancelOrderDeferedEvent(OrderEntry *ord);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

struct SwapExecutionDeferedEvent : public DeferedEventBase
{
    TradeParams nearTrade_; // contra near-leg trade params
    TradeParams farTrade_;  // contra far-leg trade params
    OrderEntry *baseOrder_; // aggressor swap order

    SwapExecutionDeferedEvent();
    explicit SwapExecutionDeferedEvent(OrderEntry *ord);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

struct MatchSwapOrderDeferedEvent : public DeferedEventBase
{
    OrderEntry *order_;

    MatchSwapOrderDeferedEvent();
    explicit MatchSwapOrderDeferedEvent(OrderEntry *ord);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

} // namespace Proc
} // namespace COP