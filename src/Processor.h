/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <vector>
#include <memory>
#include "QueuesDef.h"
#include "StateMachineDef.h"
#include "TransactionDef.h"
#include "TransactionScopePool.h"
#include "OrderMatcher.h"
#include "DeferedEvents.h"

namespace COP
{

class IdTValueGenerator;
class OrderBook;

namespace Store
{
class OrderDataStorage;
}

namespace ACID
{
class TransactionManager;
}

namespace Proc
{
class ExpiryScheduler;

struct ProcessorParams
{
    IdTValueGenerator *generator_;
    Store::OrderDataStorage *orderStorage_;
    OrderBook *orderBook_;
    Queues::InQueues *inQueues_;
    Queues::OutQueues *outQueues_;
    Queues::InQueuesContainer *inQueue_;
    ACID::TransactionManager *transactMgr_;
    /// Expires DAY and GTD orders at their time (#240), and restates the orders that outlive a business day (#244);
    /// none in most tests
    ExpiryScheduler *expiry_ = nullptr;
    /// Milliseconds since the epoch, UTC: the business day a trade or a restatement is in (#244). The system's clock
    /// when empty; tests give one of their own.
    std::function<DateTimeT()> clock_;

    /// for the test purpose
    bool testStateMachine_;
    bool testStateMachineCheckResult_;

    ProcessorParams()
        : generator_(nullptr), orderStorage_(nullptr), orderBook_(nullptr), inQueues_(nullptr), outQueues_(nullptr),
          inQueue_(nullptr), transactMgr_(nullptr), testStateMachine_(false), testStateMachineCheckResult_(false)
    {
    }
    ProcessorParams(IdTValueGenerator *generator, Store::OrderDataStorage *orderStorage, OrderBook *orderBook,
                    Queues::InQueues *inQueues, Queues::OutQueues *outQueues, Queues::InQueuesContainer *inQueue,
                    ACID::TransactionManager *transactMgr, bool testStateMachine = false,
                    bool testStateMachineCheckResult = false)
        : generator_(generator), orderStorage_(orderStorage), orderBook_(orderBook), inQueues_(inQueues),
          outQueues_(outQueues), inQueue_(inQueue), transactMgr_(transactMgr), testStateMachine_(testStateMachine),
          testStateMachineCheckResult_(testStateMachineCheckResult)
    {
    }
};

class Processor : public Queues::InQueueProcessor,
                  public DeferedEventContainer,
                  public DeferedEventFunctor,
                  public ACID::TransactionProcessor
{
public:
    Processor(void);
    ~Processor(void);

    void init(const ProcessorParams &params);

public:
    /// reimplemented from DeferedEventContainer
    virtual void addDeferedEvent(DeferedEventBase *evnt);
    virtual size_t deferedEventCount() const;
    virtual void removeDeferedEventsFrom(size_t startIndex);
    virtual bool claimStopRelease(const IdT &order);

    const ACID::TransactionScopePool *scopePool() const noexcept
    {
        return scopePool_.get();
    }

public:
    /// reimplemented from TransactionProcessor
    virtual void process(const ACID::TransactionId &id, ACID::Transaction *tr);

    /// Appends the operation that persists the order when the transaction commits (#20), once per transaction (#148),
    /// with the figures the decision just made left it with (#411). Call it under the order's write lock, once the
    /// decision is done.
    static void persistOrder(ACID::Scope *transaction, const OrderEntry &order);
    /// Sets the timers of an order just accepted: its expiry, if its time in force ends, a DAY or GTD order (#240), and
    /// its restatement, if it outlives the business day (#244)
    void scheduleExpiry(const OrderEntry &order) const;

    /// Matches a restored order that lost its fills, or the cancel of what it couldn't trade, as the transaction that
    /// booked it did when it ran (#207, #489), and books it if it rests in the price book: a market, IOC or FOK order
    /// never does, and what it can't trade is canceled. The fills and the cancel go to the transaction manager, each in
    /// a transaction of its own, but they are applied before this returns, so the next order matched sees them. Call it
    /// before the server takes orders, for each of StorageRecordDispatcher::ordersToMatch() in turn. Throws if the
    /// order can't be matched or booked.
    void matchRestored(OrderEntry *order);
    /// Releases a restored held stop that its instrument's last trade reached (#237), as that trade's release would have:
    /// in a transaction of its own, on a transaction worker. Call it for each of StorageRecordDispatcher::stopsReached()
    /// in turn, once the orders matchRestored() matches are done, and wait for the manager between them.
    void releaseRestored(OrderEntry *stop);

private:
    /// Enqueues a state change of the order with id, to be decided on the transaction worker (#96). caller names the
    /// handler in the error when the order doesn't exist.
    void enqueueStateChange(const IdT &id, OrderStateChange change, const char *caller);

public:
public:
    /// reimplemented from DeferedEventFunctor
    virtual void process(OrdState::onTradeExecution &evnt, OrderEntry *order, const ACID::Context &cnxt);
    virtual void process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context &cnxt);
    virtual void process(OrdState::onExecCancel &evnt, OrderEntry *order, const std::string &requester,
                         const ACID::Context &cnxt);
    virtual void process(OrdState::onReplace &evnt, OrderEntry *original, OrderEntry &replacement,
                         const std::string &requester, const ACID::Context &cnxt);
    virtual void process(OrderStateChange change, OrderEntry *order, ACID::Scope *scope, const ACID::Context &cnxt);

public:
    /// reimplemented from InQueueProcessor
    /// retrives new event from inQueues, process it in transaction
    virtual bool process();
    virtual void onEvent(const std::string &source, const Queues::OrderEvent &evnt);
    virtual void onEvent(const std::string &source, const Queues::OrderCancelEvent &evnt);
    virtual void onEvent(const std::string &source, const Queues::OrderReplaceEvent &evnt);
    virtual void onEvent(const std::string &source, const Queues::OrderChangeStateEvent &evnt);
    virtual void onEvent(const std::string &source, const Queues::ProcessEvent &evnt);
    virtual void onEvent(const std::string &source, const Queues::TimerEvent &evnt);

    void onEvent(DeferedEventBase *evnt);

private:
    void processDeferedEvent();
    void clearDeferedEvents();
    /// Tells the sender that its new order or replacement was refused without being stored (#67)
    void rejectUnstoredOrder(const std::string &source, const OrderEntry &order, bool replacement);
    /// What tells a client its replace was refused, and why (#74); original is null when it is unknown
    static Queues::OrderRejectEvent makeReplaceRefusal(const OrderEntry &replacement, const OrderEntry *original,
                                                       Queues::CancelRejectEvent::Reason refusal,
                                                       const std::string &reason);

private:
    IdTValueGenerator *generator_;
    Store::OrderDataStorage *orderStorage_;
    OrderBook *orderBook_;
    Queues::InQueues *inQueues_;
    Queues::OutQueues *outQueues_;
    Queues::InQueuesContainer *inQueue_;
    ACID::TransactionManager *transactMgr_;
    /// Expires DAY and GTD orders at their time (#240), and restates the orders that outlive a day (#244); none in
    /// most tests
    ExpiryScheduler *expiry_ = nullptr;
    /// The business day's clock (#244), given to every transaction's context; the system's when empty
    std::function<DateTimeT()> clock_;

    /// for the test purpose
    bool testStateMachine_;
    bool testStateMachineCheckResult_;

    OrderMatcher matcher_;

    // stateMachine_, initialSMState_, and events_ moved to anonymous-
    // namespace ProcessorThreadState in Processor.cpp.  They are
    // per-TBB-worker state, not Processor state — multiple workers
    // dispatch into Processor::process() concurrently and would race
    // on shared MSM/event-vector members.
public:
    typedef std::vector<DeferedEventBase *> DeferedEventsT;

private:
    /// Pool for TransactionScope objects to eliminate heap allocations
    std::unique_ptr<ACID::TransactionScopePool> scopePool_;
};

} // namespace Proc
} // namespace COP