/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include <cassert>
#include "Processor.h"
#include "TransactionDef.h"
#include "StateMachine.h"
#include "TransactionScope.h"
#include "TrOperations.h"
#include "DataModelDef.h"
#include "OrderStorage.h"
#include "Logger.h"

using namespace std;
using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;
using COP::ACID::PooledTransactionScope;

namespace
{
/// RAII guard to set/clear TransactionScope::s_activeScope for arena allocation.
struct ScopeArenaGuard
{
    explicit ScopeArenaGuard(TransactionScope *s)
    {
        TransactionScope::s_activeScope = s;
    }
    ~ScopeArenaGuard()
    {
        TransactionScope::s_activeScope = nullptr;
    }
    ScopeArenaGuard(const ScopeArenaGuard &) = delete;
    ScopeArenaGuard &operator=(const ScopeArenaGuard &) = delete;
};

// Per-thread Processor workspace.  Multiple TBB workers concurrently call
// Processor::process() and dispatch into onEvent on the same Processor
// instance.  Sharing the state machine and deferred-event vector across
// workers races (TSan: StateMachine.cpp:62/65, vector ops).  Each thread
// gets its own MSM and event vector; the MSM is reset per event via
// setPersistance, so per-thread reuse is correct.
struct ProcessorThreadState
{
    std::unique_ptr<OrdState::OrderState> stateMachine;
    OrdState::OrderStatePersistence initialSMState;
    std::vector<DeferedEventBase *> events;

    ProcessorThreadState()
    {
        stateMachine = std::make_unique<OrdState::OrderState>();
        stateMachine->start();
        initialSMState = stateMachine->getPersistence();
    }
};

ProcessorThreadState &threadState()
{
    thread_local ProcessorThreadState s;
    return s;
}
} // namespace

Processor::Processor(void)
    : generator_(nullptr), orderStorage_(nullptr), orderBook_(nullptr), inQueues_(nullptr), outQueues_(nullptr),
      testStateMachine_(false), testStateMachineCheckResult_(false),
      scopePool_(std::make_unique<ACID::TransactionScopePool>())
{
}

Processor::~Processor(void) {}

void Processor::init(const ProcessorParams &params)
{
    generator_ = params.generator_;
    orderStorage_ = params.orderStorage_;
    orderBook_ = params.orderBook_;
    inQueues_ = params.inQueues_;
    outQueues_ = params.outQueues_;
    inQueue_ = params.inQueue_;
    transactMgr_ = params.transactMgr_;

    testStateMachine_ = params.testStateMachine_;
    testStateMachineCheckResult_ = params.testStateMachineCheckResult_;

    assert(nullptr != generator_);
    assert(nullptr != orderStorage_);
    assert(nullptr != orderBook_);
    assert(nullptr != inQueues_);
    assert(nullptr != outQueues_);
    assert(nullptr != inQueue_);
    assert(nullptr != transactMgr_);

    matcher_.init(this);
}

bool Processor::process()
{
    assert(nullptr != inQueue_);
    try
    {
        return inQueue_->pop(this);
    }
    catch (...)
    {
        // An event that throws part-way can leave deferred events on this worker, and the next new order here would
        // then fail on them. The caller, TaskManager, logs the exception and carries on (#57).
        clearDeferedEvents();
        throw;
    }
}

void Processor::onEvent(const std::string &source, const OrderEvent &evnt)
{
    if (nullptr == evnt.order_) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderEvent): order pointer is null!");
    }
    if (!threadState().events.empty()) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderEvent): events queue is not empty!");
    }
    [[assume(evnt.order_ != nullptr)]];

    // prepare scope
    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    // restore event to process
    onOrderReceived evnt2Proc(evnt.order_);
    evnt2Proc.generator_ = generator_;
    evnt2Proc.transaction_ = scope.get();
    evnt2Proc.orderStorage_ = orderStorage_;
    evnt2Proc.orderBook_ = orderBook_;
    // save() locks the new order before publishing it; released below once its state is written (#13)
    Store::PublishGuard publishGuard;
    evnt2Proc.publishGuard_ = &publishGuard;

    // restore state of the state machine
    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(threadState().initialSMState);
    // process event
    threadState().stateMachine->process_event(evnt2Proc);
    // save state machine state into the order
    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    // save() always stores the order as a clone and returns that, so the machine ends on the incoming object only when
    // storing failed: its ClOrdID is already in use. Then there is no stored order to persist or to report through a
    // transaction, so the scope goes back to its pool unused and the sender is told directly (#67).
    if (smState.orderData_ == evnt.order_) [[unlikely]]
    {
        publishGuard.release();
        rejectUnstoredOrder(source, *evnt.order_, false);
        return;
    }
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(scope.get(), *smState.orderData_);
    // The order is fully initialised: let other threads that located it proceed. Released before
    // addTransaction(), because the transaction and deferred events lock this entry themselves.
    publishGuard.release();

    // enqueue transaction, prepared by state machine
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    // process defered events
    processDeferedEvent();
}

void Processor::onEvent(const std::string &source, const OrderCancelEvent &evnt)
{
    // A cancel for an order that does not exist is the client's mistake: reject it rather than throw (#57)
    OrderEntry *ord = evnt.id_.isValid() ? orderStorage_->locateByOrderId(evnt.id_) : nullptr;
    if (nullptr == ord) [[unlikely]]
    {
        CancelRejectEvent reject;
        reject.id_ = evnt.id_;
        reject.reason_ = CancelRejectEvent::UNKNOWN_ORDER;
        outQueues_->push(reject, source);
        return;
    }
    [[assume(ord != nullptr)]];

    // The cancel is decided on the transaction worker, by process(onExecCancel) below, once every transaction before
    // this one on the order or its instrument has run, together with the fills it caused. Decided here, it could
    // overtake a trade already matched against the order and leave the other side filled alone (#73).
    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());
    std::unique_ptr<Operation> op(new CancelOrderTrOperation(ord, source));
    scope->addOperation(op);

    // enqueue transaction
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    processDeferedEvent();
}

void Processor::onEvent(const std::string &source, const OrderReplaceEvent &evnt)
{
    // The replace is decided on the transaction worker, by process(onReplace) below, once every transaction before this
    // one on the original or its instrument has run, together with the fills it caused, as a cancel is (#73, #74). Here
    // only what needs no decision is refused: a request with no replacement, or one for an order that doesn't exist.
    OrderEntry *original = evnt.id_.isValid() ? orderStorage_->locateByOrderId(evnt.id_) : nullptr;
    if ((nullptr == evnt.replacementOrder_) || (nullptr == original)) [[unlikely]]
    {
        OrderRejectEvent refusal;
        if (nullptr == evnt.replacementOrder_)
        {
            refusal.replacement_ = true;
            refusal.reason_ = "Replace refused: it carries no replacement order";
        }
        else
        {
            refusal = makeReplaceRefusal(*evnt.replacementOrder_, nullptr, CancelRejectEvent::UNKNOWN_ORDER,
                                         "Replace refused: unknown order");
        }
        assert(nullptr != outQueues_);
        outQueues_->push(refusal, source);
        return;
    }
    [[assume(original != nullptr)]];

    // The queue frees the event's replacement after dispatch, so the operation keeps a copy
    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());
    std::unique_ptr<Operation> op(
        new ReplaceOrderTrOperation(original, std::unique_ptr<OrderEntry>(evnt.replacementOrder_->clone()), source));
    scope->addOperation(op);

    // enqueue transaction
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    processDeferedEvent();
}

void Processor::onEvent(const std::string & /*source*/, const COP::Queues::OrderChangeStateEvent &evnt)
{
    if (!evnt.id_.isValid()) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): order id is invalid!");
    }
    if (evnt.changeType_ == OrderChangeStateEvent::INVALID_CHANGE) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): invalid change type!");
    }

    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    // locate the order
    OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
    if (nullptr == ord) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): unable to locate order!");
    }

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, true);

    // restore state machine from order
    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(ord->stateMachinePersistance());

    // process the appropriate state change event
    switch (evnt.changeType_)
    {
    case OrderChangeStateEvent::SUSPEND:
    {
        onSuspended evnt2Proc;
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    case OrderChangeStateEvent::RESUME:
    {
        onContinue evnt2Proc;
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    case OrderChangeStateEvent::FINISH:
    {
        onFinished evnt2Proc;
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    default:
        throw std::runtime_error("Processor::onEvent(OrderChangeStateEvent): unknown change type!");
    }

    // save updated state back to order
    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(scope.get(), *smState.orderData_);

    ordLock.release();

    // enqueue transaction
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    processDeferedEvent();
}

void Processor::onEvent(const std::string & /*source*/, const ProcessEvent &evnt)
{
    assert(threadState().events.empty());
    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    // Locate the order — all cases use the same id
    OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
    if (nullptr == ord) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(ProcessEvent): unable to locate order!");
    }

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, true);

    switch (evnt.type_)
    {
    case ProcessEvent::ON_REPLACE_RECEVIED:
    {
        // restore event to process
        onReplaceReceived evnt2Proc(evnt.id_);
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;

        assert(nullptr != threadState().stateMachine);
        threadState().stateMachine->setPersistance(ord->stateMachinePersistance());
        // process event
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    case ProcessEvent::ON_EXEC_REPLACE:
    {
        // restore event to process
        onExecReplace evnt2Proc(evnt.id_);
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;

        assert(nullptr != threadState().stateMachine);
        threadState().stateMachine->setPersistance(ord->stateMachinePersistance());
        // process event
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    case ProcessEvent::ON_REPLACE_REJECTED:
    {
        // restore event to process
        onReplaceRejected evnt2Proc(evnt.id_);
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;

        assert(nullptr != threadState().stateMachine);
        threadState().stateMachine->setPersistance(ord->stateMachinePersistance());
        // process event in state machine
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    default:
        throw std::runtime_error("Processor::onEvent() fails: unknown type of the ProcessEvent.");
    };

    // save state into the order
    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(scope.get(), *smState.orderData_);

    ordLock.release();

    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    processDeferedEvent();
}

void Processor::onEvent(const std::string & /*source*/, const TimerEvent &evnt)
{
    if (!evnt.id_.isValid()) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(TimerEvent): order id is invalid!");
    }
    if (evnt.timerType_ == TimerEvent::INVALID_TIMER) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(TimerEvent): invalid timer type!");
    }

    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    // locate the order
    OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
    if (nullptr == ord) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(TimerEvent): unable to locate order!");
    }

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, true);

    // restore state machine from order
    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(ord->stateMachinePersistance());

    // process the appropriate timer event
    switch (evnt.timerType_)
    {
    case TimerEvent::EXPIRATION:
    {
        onExpired evnt2Proc;
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    case TimerEvent::DAY_END:
    {
        onNewDay evnt2Proc;
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    case TimerEvent::DAY_START:
    {
        onContinue evnt2Proc;
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;
        threadState().stateMachine->process_event(evnt2Proc);
    }
    break;
    default:
        throw std::runtime_error("Processor::onEvent(TimerEvent): unknown timer type!");
    }

    // save updated state back to order
    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(scope.get(), *smState.orderData_);

    ordLock.release();

    // enqueue transaction
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    processDeferedEvent();
}

void Processor::addDeferedEvent(DeferedEventBase *evnt)
{
    assert(nullptr != evnt);
    threadState().events.push_back(evnt);
}

size_t Processor::deferedEventCount() const
{
    return threadState().events.size();
}

void Processor::removeDeferedEventsFrom(size_t startIndex)
{
    if (startIndex >= threadState().events.size())
    {
        return;
    }

    // Delete events from startIndex to end
    for (size_t i = startIndex; i < threadState().events.size(); ++i)
    {
        delete threadState().events[i];
    }
    threadState().events.erase(threadState().events.begin() + static_cast<std::ptrdiff_t>(startIndex),
                               threadState().events.end());
}

void Processor::persistOrder(ACID::Scope *transaction, const OrderEntry &order)
{
    assert(nullptr != transaction);
    std::unique_ptr<ACID::Operation> op(new ACID::PersistOrderTrOperation(order));
    // First, not last: CreateExecReportTrOperation publishes during execute and WsOutQueues
    // broadcasts straight away, so persisting afterwards let a client see a fill before it was
    // durable (#28). No operation mutates the fields the codec writes, so the record is the same.
    transaction->addOperationFirst(op);
}

void Processor::onEvent(DeferedEventBase *evnt)
{
    Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_, this);
    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    evnt->execute(this, cntxt, scope.get());

    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);
}

void Processor::processDeferedEvent()
{
    /// Defered events may enqueue another chain of defered events - need to process them also.
    while (!threadState().events.empty())
    {
        DeferedEventsT tmp;
        swap(tmp, threadState().events);
        try
        {
            for (DeferedEventsT::iterator it = tmp.begin(); it != tmp.end(); ++it)
            {
                std::unique_ptr<DeferedEventBase> evnt(*it);
                *it = nullptr;

                onEvent(evnt.get());
            }
        }
        catch (const std::exception &)
        {
            for (DeferedEventsT::iterator it = tmp.begin(); it != tmp.end(); ++it)
            {
                if (nullptr != *it)
                {
                    delete *it;
                }
            }
            // Clear any partial-chain events enqueued before the exception
            // to prevent processing inconsistent state
            DeferedEventsT partial;
            swap(partial, threadState().events);
            for (DeferedEventsT::iterator it = partial.begin(); it != partial.end(); ++it)
            {
                delete *it;
            }
            throw;
        }
    }
}

void Processor::process(onTradeExecution &evnt, OrderEntry *order, const ACID::Context & /*cnxt*/)
{
    evnt.generator_ = generator_;
    evnt.orderStorage_ = orderStorage_;

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(order->entryMutex_, true);

    threadState().stateMachine->setPersistance(order->stateMachinePersistance());
    threadState().stateMachine->process_event(evnt);

    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(evnt.transaction_, *smState.orderData_);
}

void Processor::process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context & /*cnxt*/)
{
    evnt.generator_ = generator_;
    evnt.orderStorage_ = orderStorage_;

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(order->entryMutex_, true);

    threadState().stateMachine->setPersistance(order->stateMachinePersistance());
    threadState().stateMachine->process_event(evnt);

    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(evnt.transaction_, *smState.orderData_);
}

void Processor::process(OrdState::onExecCancel &evnt, OrderEntry *order, const std::string &requester,
                        const ACID::Context & /*cnxt*/)
{
    evnt.generator_ = generator_;
    evnt.orderStorage_ = orderStorage_;
    evnt.orderBook_ = orderBook_;

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(order->entryMutex_, true);

    // Only a live order can be cancelled: one in the book, or suspended, or done for the day (#73)
    const OrderStatus status = order->status_;
    if ((NEW_ORDSTATUS != status) && (PARTFILL_ORDSTATUS != status) && (SUSPENDED_ORDSTATUS != status) &&
        (DFD_ORDSTATUS != status))
    {
        std::unique_ptr<Operation> op(new CancelRejectTrOperation(*order, CancelRejectEvent::TOO_LATE, requester));
        evnt.transaction_->addOperation(op);
        return;
    }

    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(order->stateMachinePersistance());
    threadState().stateMachine->process_event(evnt);
    if (CANCELED_ORDSTATUS != order->status_)
    {
        // No transition, and so nothing changed: a replace of the order is pending
        std::unique_ptr<Operation> op(new CancelRejectTrOperation(*order, CancelRejectEvent::PENDING, requester));
        evnt.transaction_->addOperation(op);
        return;
    }

    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);
    // Persist the order as part of this transaction (#20), ahead of the operations that publish
    // execution reports, so an acknowledged change is already durable (#28).
    persistOrder(evnt.transaction_, *smState.orderData_);
}

namespace
{
/// Whether the order's cancel/replace zone is at rest, read from its persisted state machine state
bool noCancelOrReplacePending(const OrderEntry &order)
{
    const int zone2 = order.stateMachinePersistance().stateZone2Id_;
    try
    {
        return (0 <= zone2) && ("NoCnlReplace" == OrderState::getStateName(zone2));
    }
    catch (const std::exception &)
    {
        return false; // a state the machine doesn't know: treat it as not at rest
    }
}
} // namespace

void Processor::process(OrdState::onReplace &evnt, OrderEntry *original, OrderEntry &replacement,
                        const std::string &requester, const ACID::Context & /*cnxt*/)
{
    evnt.generator_ = generator_;
    evnt.orderStorage_ = orderStorage_;
    evnt.orderBook_ = orderBook_;
    // A refusal goes out through this transaction, in order with the original's other reports
    auto refuse = [&](CancelRejectEvent::Reason refusal, const std::string &reason, bool duplicate)
    {
        OrderRejectEvent event = makeReplaceRefusal(replacement, original, refusal, reason);
        event.duplicateClOrderId_ = duplicate;
        std::unique_ptr<Operation> op(new RefuseReplaceTrOperation(*original, event, requester));
        evnt.transaction_->addOperation(op);
    };

    // A market replacement needs an order to trade with. Asked before the original's lock is taken: a matcher can hold
    // the book's lock while it waits on the original (#74).
    const Side opposite = (BUY_SIDE == replacement.side_) ? SELL_SIDE : BUY_SIDE;
    const bool noMarket = (MARKET_ORDERTYPE == replacement.ordType_) &&
                          !orderBook_->getTop(replacement.instrument_.getId(), opposite).isValid();

    // write lock on the original for the decision and its state machine
    oneapi::tbb::spin_rw_mutex::scoped_lock origLock(original->entryMutex_, true);

    // Only an order in the book can be replaced, with no cancel or replace of it pending
    const OrderStatus status = original->status_;
    if ((NEW_ORDSTATUS != status) && (PARTFILL_ORDSTATUS != status))
    {
        refuse(CancelRejectEvent::TOO_LATE, "Replace refused: too late, the order is no longer in the book", false);
        return;
    }
    if (!noCancelOrReplacePending(*original))
    {
        refuse(CancelRejectEvent::PENDING, "Replace refused: a cancel or replace of the order is pending", false);
        return;
    }

    // The replacement takes over what the original has done: its fills and its executions
    replacement.orderId_ = IdT();
    replacement.origOrderId_ = original->orderId_;
    replacement.origClOrderId_ = original->clOrderId_;
    replacement.executions_ = original->executions_;
    replacement.cumQty_ = original->cumQty_;
    replacement.avgPx_ = original->avgPx_;
    replacement.status_ = RECEIVEDNEW_ORDSTATUS;
    if ((replacement.instrument_.getId() != original->instrument_.getId()) || (replacement.side_ != original->side_))
    {
        refuse(CancelRejectEvent::OTHER, "Replace refused: a replace cannot change the instrument or the side", false);
        return;
    }
    if (replacement.orderQty_ <= original->cumQty_)
    {
        refuse(CancelRejectEvent::OTHER,
               "Replace refused: quantity " + std::to_string(replacement.orderQty_) + " is not above the " +
                   std::to_string(original->cumQty_) + " already filled",
               false);
        return;
    }
    replacement.leavesQty_ = replacement.orderQty_ - original->cumQty_;
    std::string invalid;
    if (!replacement.isValid(&invalid))
    {
        refuse(CancelRejectEvent::OTHER, "Replace refused: " + invalid, false);
        return;
    }
    if (noMarket)
    {
        refuse(CancelRejectEvent::OTHER, "Replace refused: there is no market for this instrument", false);
        return;
    }

    // Store it under a fresh id. A ClOrdID already in use is refused, as for a new order (#67).
    Store::PublishGuard publishGuard;
    OrderEntry *stored = nullptr;
    try
    {
        stored = orderStorage_->save(replacement, generator_, &publishGuard);
    }
    catch (const std::exception &)
    {
        bool duplicate = false;
        std::string clOrdId;
        try
        {
            const RawDataEntry &raw = replacement.clOrderId_.get();
            clOrdId.assign(raw.data_, raw.length_);
            duplicate = (nullptr != orderStorage_->locateByClOrderId(raw));
        }
        catch (const std::exception &)
        {
        }
        refuse(CancelRejectEvent::OTHER,
               duplicate ? "Replace refused: ClOrdID " + clOrdId + " is already in use"
                         : std::string("Replace refused: it could not be stored"),
               duplicate);
        return;
    }

    // The original is replaced: out of the book, with nothing left to fill
    onExecReplace replaced(stored->orderId_);
    replaced.generator_ = generator_;
    replaced.orderStorage_ = orderStorage_;
    replaced.orderBook_ = orderBook_;
    replaced.transaction_ = evnt.transaction_;
    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(original->stateMachinePersistance());
    threadState().stateMachine->process_event(replaced);
    OrderStatePersistence origState = threadState().stateMachine->getPersistence();
    assert(nullptr != origState.orderData_);
    origState.orderData_->setStateMachinePersistance(origState);

    // The replacement goes live: matched, and into the book, as a new order is
    OrderStatePersistence replState = threadState().initialSMState;
    replState.orderData_ = stored;
    threadState().stateMachine->setPersistance(replState);
    threadState().stateMachine->process_event(evnt);
    replState = threadState().stateMachine->getPersistence();
    assert(nullptr != replState.orderData_);
    replState.orderData_->setStateMachinePersistance(replState);
    // Everything either machine checks is checked above, so neither can refuse. If one did, the machine swallowed the
    // error and the status shows it.
    if ((REPLACED_ORDSTATUS != original->status_) ||
        ((NEW_ORDSTATUS != stored->status_) && (PARTFILL_ORDSTATUS != stored->status_))) [[unlikely]]
    {
        aux::ExchLogger::instance()->error("Processor: a replace that passed its checks did not complete");
    }

    // Persist both as part of this transaction (#20), ahead of the operations that publish their reports (#28)
    persistOrder(evnt.transaction_, *stored);
    persistOrder(evnt.transaction_, *original);
    publishGuard.release();
}

void Processor::process(const ACID::TransactionId &id, ACID::Transaction *tr)
{
    assert(nullptr != tr);
    assert(id.isValid());

    Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_, this);

    bool success = tr->executeTransaction(cntxt);

    if (success) [[likely]]
    {
        try
        {
            processDeferedEvent();
        }
        catch (...)
        {
            clearDeferedEvents(); // as in process(): leave nothing behind for this worker's next transaction (#57)
            throw;
        }
    }
    else
    {
        clearDeferedEvents();
    }
}

void Processor::clearDeferedEvents()
{
    for (DeferedEventsT::iterator it = threadState().events.begin(); it != threadState().events.end(); ++it)
    {
        delete *it;
    }
    threadState().events.clear();
}

void Processor::rejectUnstoredOrder(const std::string &source, const OrderEntry &order, bool replacement)
{
    clearDeferedEvents();
    auto text = [](const RawDataEntry &raw)
    {
        return (nullptr != raw.data_) ? std::string(raw.data_, raw.length_) : std::string();
    };
    OrderRejectEvent reject;
    reject.replacement_ = replacement;
    reject.side_ = order.side_;
    reject.orderQty_ = order.orderQty_;
    try
    {
        const RawDataEntry &clOrdId = order.clOrderId_.get();
        reject.clOrderId_ = text(clOrdId);
        // Only refused orders get here, so this second lookup costs nothing on the common path
        reject.duplicateClOrderId_ = (nullptr != orderStorage_->locateByClOrderId(clOrdId));
        if (replacement)
        {
            reject.origClOrderId_ = text(order.origClOrderId_.get());
        }
        reject.symbol_ = order.instrument_.get().symbol_;
    }
    catch (const std::exception &)
    {
        // a reference the order carries could not be resolved: report what is known
    }
    reject.reason_ = reject.duplicateClOrderId_ ? "Order refused: ClOrdID " + reject.clOrderId_ + " is already in use"
                                                : std::string("Order refused: it could not be stored");
    assert(nullptr != outQueues_);
    outQueues_->push(reject, source);
}

OrderRejectEvent Processor::makeReplaceRefusal(const OrderEntry &replacement, const OrderEntry *original,
                                               CancelRejectEvent::Reason refusal, const std::string &reason)
{
    auto text = [](const RawDataEntry &raw)
    {
        return (nullptr != raw.data_) ? std::string(raw.data_, raw.length_) : std::string();
    };
    OrderRejectEvent event;
    event.replacement_ = true;
    event.refusal_ = refusal;
    event.side_ = replacement.side_;
    event.orderQty_ = replacement.orderQty_;
    event.reason_ = reason;
    try
    {
        event.clOrderId_ = text(replacement.clOrderId_.get());
        event.symbol_ = replacement.instrument_.get().symbol_;
        if (nullptr != original)
        {
            event.origClOrderId_ = text(original->clOrderId_.get());
            event.origStatus_ = original->status_;
        }
        else if (SourceIdT() != replacement.origClOrderId_.getId())
        {
            event.origClOrderId_ = text(replacement.origClOrderId_.get());
        }
    }
    catch (const std::exception &)
    {
        // a reference the order carries could not be resolved: report what is known
    }
    return event;
}