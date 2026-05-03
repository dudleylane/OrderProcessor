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
#include "DataModelDef.h"
#include "OrderStorage.h"

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
    bool rez = inQueue_->pop(this);
    return rez;
}

void Processor::onEvent(const std::string & /*source*/, const OrderEvent &evnt)
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

    // restore state of the state machine
    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(threadState().initialSMState);
    // process event
    threadState().stateMachine->process_event(evnt2Proc);
    // save state machine state into the order
    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);

    // enqueue transaction, prepared by state machine
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    // process defered events
    processDeferedEvent();
}

void Processor::onEvent(const std::string & /*source*/, const OrderCancelEvent &evnt)
{
    if (!evnt.id_.isValid()) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderCancelEvent): order id is invalid!");
    }

    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    // locate the order to cancel
    OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
    if (nullptr == ord) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderCancelEvent): unable to locate order!");
    }
    [[assume(ord != nullptr)]];

    // write lock on the order for state machine processing
    oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, true);

    // create cancel received event
    onCancelReceived evnt2Proc;
    evnt2Proc.generator_ = generator_;
    evnt2Proc.transaction_ = scope.get();
    evnt2Proc.orderStorage_ = orderStorage_;
    evnt2Proc.orderBook_ = orderBook_;

    // restore state machine from order and process event
    assert(nullptr != threadState().stateMachine);
    threadState().stateMachine->setPersistance(ord->stateMachinePersistance());
    threadState().stateMachine->process_event(evnt2Proc);

    // save updated state back to order
    OrderStatePersistence smState = threadState().stateMachine->getPersistence();
    assert(nullptr != smState.orderData_);
    smState.orderData_->setStateMachinePersistance(smState);

    ordLock.release();

    // enqueue transaction
    assert(nullptr != transactMgr_);
    std::unique_ptr<Transaction> tr(scope.release());
    transactMgr_->addTransaction(tr);

    processDeferedEvent();
}

void Processor::onEvent(const std::string & /*source*/, const OrderReplaceEvent &evnt)
{
    if (!evnt.id_.isValid()) [[unlikely]]
    {
        throw std::runtime_error("Processor::onEvent(OrderReplaceEvent): order id is invalid!");
    }

    PooledTransactionScope scope(scopePool_.get());
    ScopeArenaGuard arenaGuard(scope.get());

    if (nullptr != evnt.replacementOrder_)
    {
        // New replacement order submission - process via state machine
        onRplOrderReceived evnt2Proc(evnt.replacementOrder_);
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;
        evnt2Proc.orderBook_ = orderBook_;

        // use initial state for new replacement order
        assert(nullptr != threadState().stateMachine);
        threadState().stateMachine->setPersistance(threadState().initialSMState);
        threadState().stateMachine->process_event(evnt2Proc);

        // save state machine state into the replacement order
        OrderStatePersistence smState = threadState().stateMachine->getPersistence();
        assert(nullptr != smState.orderData_);
        smState.orderData_->setStateMachinePersistance(smState);
    }
    else
    {
        // Notify existing order about replace received (similar to ProcessEvent::ON_REPLACE_RECEVIED)
        OrderEntry *ord = orderStorage_->locateByOrderId(evnt.id_);
        if (nullptr == ord) [[unlikely]]
        {
            throw std::runtime_error("Processor::onEvent(OrderReplaceEvent): unable to locate order!");
        }

        // write lock on the order for state machine processing
        oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(ord->entryMutex_, true);

        onReplaceReceived evnt2Proc(evnt.id_);
        evnt2Proc.generator_ = generator_;
        evnt2Proc.transaction_ = scope.get();
        evnt2Proc.orderStorage_ = orderStorage_;

        assert(nullptr != threadState().stateMachine);
        threadState().stateMachine->setPersistance(ord->stateMachinePersistance());
        threadState().stateMachine->process_event(evnt2Proc);

        // save updated state back to order
        OrderStatePersistence smState = threadState().stateMachine->getPersistence();
        assert(nullptr != smState.orderData_);
        smState.orderData_->setStateMachinePersistance(smState);
    }

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
}

void Processor::process(const ACID::TransactionId &id, ACID::Transaction *tr)
{
    assert(nullptr != tr);
    assert(id.isValid());

    Context cntxt(orderStorage_, orderBook_, inQueues_, outQueues_, &matcher_, generator_, this);

    bool success = tr->executeTransaction(cntxt);

    if (success) [[likely]]
    {
        processDeferedEvent();
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