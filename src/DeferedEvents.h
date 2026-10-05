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
#include <memory>
#include "TypesDef.h"
#include "TransactionDef.h"
#include "OrderStateEvents.h"

namespace COP
{
struct OrderEntry;

namespace Proc
{

/// A change of an order's state that a request asks for: a suspend, resume or finish, or a timer's expiry, day end or
/// day start (#96)
enum class OrderStateChange
{
    SUSPEND,
    RESUME,
    FINISH,
    EXPIRE,
    DAY_END,
    DAY_START
};

class DeferedEventFunctor
{
public:
    virtual ~DeferedEventFunctor() {}

    virtual void process(OrdState::onTradeExecution &evnt, OrderEntry *order, const ACID::Context &cnxt) = 0;
    virtual void process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context &cnxt) = 0;
    /// A client's cancel of the order; a refusal goes to the requester (#73)
    virtual void process(OrdState::onExecCancel &evnt, OrderEntry *order, const std::string &requester,
                         const ACID::Context &cnxt) = 0;
    /// A client's replace of the original; the caller keeps the replacement, and a refusal goes to the requester (#74)
    virtual void process(OrdState::onReplace &evnt, OrderEntry *original, OrderEntry &replacement,
                         const std::string &requester, const ACID::Context &cnxt) = 0;
    /// A state change of the order, decided into scope (#96)
    virtual void process(OrderStateChange change, OrderEntry *order, ACID::Scope *scope, const ACID::Context &cnxt) = 0;
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

/// A client's replace, queued by ReplaceOrderTrOperation once the transactions before it on the original are done (#74).
/// It owns the replacement; the processor stores a copy if it accepts it.
struct ReplaceRequestDeferedEvent : public DeferedEventBase
{
    OrderEntry *original_;
    std::unique_ptr<OrderEntry> replacement_;
    std::string requester_;

    ReplaceRequestDeferedEvent(OrderEntry *original, std::unique_ptr<OrderEntry> replacement,
                               const std::string &requester);
    ~ReplaceRequestDeferedEvent();

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

/// A client's cancel, queued by CancelOrderTrOperation once the transactions before it on the order are done (#73)
struct CancelRequestDeferedEvent : public DeferedEventBase
{
    OrderEntry *order_;
    std::string requester_;

    CancelRequestDeferedEvent(OrderEntry *ord, const std::string &requester);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

/// A state change of an order, queued by ChangeOrderStateTrOperation once the transactions before it on the order are
/// done (#96)
struct StateChangeDeferedEvent : public DeferedEventBase
{
    OrderEntry *order_;
    OrderStateChange change_;

    StateChangeDeferedEvent(OrderEntry *ord, OrderStateChange change);

    virtual void execute(DeferedEventFunctor *func, const ACID::Context &cnxt, ACID::Scope *scope);
};

} // namespace Proc
} // namespace COP