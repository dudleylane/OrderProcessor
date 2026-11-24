/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <atomic>
#include <cassert>
#include <stdexcept>
#include <boost/core/demangle.hpp>
#include <boost/mpl/at.hpp>
#include <boost/mpl/size.hpp>
#include <boost/msm/row_tags.hpp>
#include <type_traits>
#include <utility>
#include "StateMachine.h"
#include "DataModelDef.h"
#include "Logger.h"
#include "OrderStateMachineImpl.h"

using namespace COP;
using namespace COP::OrdState;
using namespace boost::msm;

namespace
{
const std::string state_names[ORDER_STATE_COUNT] = { "Rcvd_New",     "Pend_Replace", "New",          "PartFill",
                                                     "Filled",       "Expired",      "DoneForDay",   "Suspended",
                                                     "NoCnlReplace", "GoingCancel",  "GoingReplace", "CnclReplaced",
                                                     "Rejected" };

// Every stored order holds its states' ids (#150), so each id is pinned here to the index of its name above. MSM
// numbers the states by their first appearance in the transition table, sources first, then the states that are only
// targets. An edit of the table that moves a state fails to compile here, instead of reinterpreting every order already
// stored.
template <typename State> constexpr int stateId = boost::msm::back::get_state_id<OrderState::stt, State>::value;
static_assert(stateId<Rcvd_New> == 0, "Rcvd_New's id is persisted");
static_assert(stateId<Pend_Replace> == 1, "Pend_Replace's id is persisted");
static_assert(stateId<New> == 2, "New's id is persisted");
static_assert(stateId<PartFill> == 3, "PartFill's id is persisted");
static_assert(stateId<Filled> == 4, "Filled's id is persisted");
static_assert(stateId<Expired> == 5, "Expired's id is persisted");
static_assert(stateId<DoneForDay> == 6, "DoneForDay's id is persisted");
static_assert(stateId<Suspended> == 7, "Suspended's id is persisted");
static_assert(stateId<NoCnlReplace> == 8, "NoCnlReplace's id is persisted");
static_assert(stateId<GoingCancel> == 9, "GoingCancel's id is persisted");
static_assert(stateId<GoingReplace> == 10, "GoingReplace's id is persisted");
static_assert(stateId<CnclReplaced> == 11, "CnclReplaced's id is persisted");
static_assert(stateId<CnclReplaced> == CNCL_REPLACED_STATE_ID, "closedInZone2() reads CnclReplaced's id");
static_assert(stateId<Rejected> == 12, "Rejected's id is persisted");
// Each region's states, as restore checks a record's pair against them (#600): the first region is the one
// initial_state starts at Rcvd_New, the second the one it starts at NoCnlReplace
static_assert(isZone1StateId(stateId<Rcvd_New>) && isZone1StateId(stateId<Pend_Replace>) &&
                  isZone1StateId(stateId<New>) && isZone1StateId(stateId<PartFill>) &&
                  isZone1StateId(stateId<Filled>) && isZone1StateId(stateId<Expired>) &&
                  isZone1StateId(stateId<DoneForDay>) && isZone1StateId(stateId<Suspended>) &&
                  isZone1StateId(stateId<Rejected>),
              "the first region's states");
static_assert(isZone2StateId(stateId<NoCnlReplace>) && isZone2StateId(stateId<GoingCancel>) &&
                  isZone2StateId(stateId<GoingReplace>) && isZone2StateId(stateId<CnclReplaced>),
              "the second region's states");
static_assert(boost::mpl::size<boost::msm::back::generate_state_set<OrderState::stt>::type>::value == ORDER_STATE_COUNT,
              "a new state needs its id pinned above and a name in state_names");

// A guard that refuses throws nothing: MSM tries the state's other rows for the event, and an event every row's guard
// refused returns unhandled, with no call to no_transition(), so exception_caught() never counts or logs it, and the
// request it carried goes unanswered (#432). MSM tries a state's rows last-declared first, so each guarded row needs an
// unguarded row for its source state and event declared before it, which MSM tries last. The table is walked here, and
// a guarded row added without one fails to compile (#634).
template <std::size_t I> using RowAt = typename boost::mpl::at_c<OrderState_::transition_table, I>::type;
template <typename Row>
constexpr bool guardedRow =
    std::is_same_v<typename Row::row_type_tag, row_tag> || std::is_same_v<typename Row::row_type_tag, g_row_tag> ||
    std::is_same_v<typename Row::row_type_tag, irow_tag> || std::is_same_v<typename Row::row_type_tag, g_irow_tag>;
template <std::size_t I, std::size_t J> constexpr bool unguardedBefore()
{
    if constexpr (J >= I)
    {
        return false;
    }
    else
    {
        return !guardedRow<RowAt<J>> && std::is_same_v<typename RowAt<J>::Source, typename RowAt<I>::Source> &&
               std::is_same_v<typename RowAt<J>::Evt, typename RowAt<I>::Evt>;
    }
}
template <std::size_t I, std::size_t... J> constexpr bool hasUnguardedBefore(std::index_sequence<J...>)
{
    return (unguardedBefore<I, J>() || ...);
}
template <std::size_t... I> constexpr bool everyGuardedRowHasOne(std::index_sequence<I...> rows)
{
    return ((!guardedRow<RowAt<I>> || hasUnguardedBefore<I>(rows)) && ...);
}
static_assert(everyGuardedRowHasOne(std::make_index_sequence<boost::mpl::size<OrderState_::transition_table>::value>{}),
              "a guarded row needs an unguarded row for its source state and event declared before it (#432, #634)");

// What exception_caught() has swallowed (#432): refused by design, and failed
std::atomic<uint64_t> swallowedRefused{ 0 };
std::atomic<uint64_t> swallowedFailed{ 0 };
} // namespace

const std::string &OrderState_::getStateName(int idx)
{
    if ((idx < 0) || (ORDER_STATE_COUNT <= idx))
    {
        throw std::runtime_error("Invalid index of the state's name!");
    }
    return state_names[idx];
}

OrderState_::OrderState_() : orderData_(nullptr) {}

OrderState_::OrderState_(OrderEntry *orderData) : orderData_(orderData) {}

OrderState_::~OrderState_() {}

OrderStatePersistence OrderState_::getPersistence() const
{
    // Cast to back-end to access state machine internals
    // This is safe because getPersistence is only called on the back-end object
    const auto *backend = static_cast<const OrderState *>(this);
    OrderStatePersistence st;
    st.orderData_ = orderData_;
    st.stateZone1Id_ = backend->current_state()[0];
    st.stateZone2Id_ = backend->current_state()[1];
    return st;
}

void OrderState_::setPersistance(const OrderStatePersistence &st)
{
    // Cast to back-end to access state machine internals
    auto *backend = static_cast<OrderState *>(this);
    orderData_ = st.orderData_;
    // Direct write to state array (const_cast is needed as current_state() returns const)
    auto *states = const_cast<int *>(backend->current_state());
    // Restore refuses an id that isn't a state (#150); every other id comes from the machine itself
    assert((0 <= st.stateZone1Id_) && (st.stateZone1Id_ < ORDER_STATE_COUNT));
    assert((0 <= st.stateZone2Id_) && (st.stateZone2Id_ < ORDER_STATE_COUNT));
    states[0] = st.stateZone1Id_;
    states[1] = st.stateZone2Id_;
}

uint64_t OrderState_::refusedCount() noexcept
{
    return swallowedRefused.load(std::memory_order_relaxed);
}

uint64_t OrderState_::failedCount() noexcept
{
    return swallowedFailed.load(std::memory_order_relaxed);
}

std::string OrderState_::describeSwallowed(bool refused, const std::type_info &event, const IdT &order,
                                           const int *states, const char *what)
{
    std::string name = boost::core::demangle(event.name());
    const std::string ns = "COP::OrdState::"; // every event is one of COP::OrdState's
    if (0 == name.compare(0, ns.size(), ns))
    {
        name.erase(0, ns.size());
    }
    const auto state = [](int idx) -> std::string
    {
        return ((0 <= idx) && (idx < ORDER_STATE_COUNT)) ? state_names[idx] : std::to_string(idx);
    };
    return std::string("State machine ") + (refused ? "refused " : "failed ") + name + " for " +
           (order.isValid() ? "order " + std::to_string(order.id_) : std::string("a new order")) + " in " +
           state(states[0]) + "/" + state(states[1]) + ": " + what;
}

void OrderState_::swallowed(const std::type_info &event, const OrderEntry *order, const IdT &eventOrder,
                            const int *states, const std::exception &ex) noexcept
{
    const bool refused = (nullptr != dynamic_cast<const TransitionRefused *>(&ex));
    (refused ? swallowedRefused : swallowedFailed).fetch_add(1, std::memory_order_relaxed);
    // As in TaskManager::taskFailed(): nothing may leave here (exception_caught()), so a line lost, e.g. to bad_alloc,
    // is the lesser harm, and the count above still records it
    try
    {
        aux::Logger *log = aux::ExchLogger::instance();
        const IdT &id = (nullptr != order) ? order->orderId_ : eventOrder;
        if (!refused)
        {
            log->error(describeSwallowed(false, event, id, states, ex.what()));
        }
        else if (log->isNoteOn())
        {
            log->note(describeSwallowed(true, event, id, states, ex.what()));
        }
    }
    catch (...)
    {
    }
}

void OrderState_::receive(onOrderReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }
    assert(nullptr == orderData_);
    assert(nullptr != evnt.order_);
    // What storing the order refuses, a ClOrdID already in use, which refused() can't decide before the store does. The
    // reject is queued, and the throw stops the transition to New, so that the reject leaves Rcvd_New (#436): a
    // TransitionRefused, which exception_caught() notes rather than logs as a failure (#432). An action can't veto its
    // row otherwise, so that throw stays; the store says the ClOrdID is in use without one, and the reject doesn't
    // store the order again, where each threw too (#605).
    auto refuse = [this, &evnt](const char *reason, bool clOrdIdInUse)
    {
        onRecvOrderRejected newEvnt(evnt, evnt.order_, reason);
        newEvnt.clOrdIdInUse_ = clOrdIdInUse;
        newEvnt.order4StateMachine_ = orderData_;
        static_cast<OrderState *>(this)->process_event(newEvnt);
        throw TransitionRefused(reason);
    };
    const char *inUse = nullptr;
    bool stored = false;
    try
    {
        stored = OrdStateImpl::processReceive(&orderData_, evnt, &inUse);
    }
    catch (const std::exception &ex)
    {
        refuse(ex.what(), false);
    }
    if (!stored) [[unlikely]]
    {
        refuse(inUse, true);
    }
    evnt.order4StateMachine_ = orderData_;
}

/// Whether the order is refused before it is stored: isValid() refuses it, or it is a market order with nothing to
/// trade with. A refused order takes its own row to Rejected, where processReceive() used to throw and receive() to
/// rethrow: two exceptions raised, about 45% of a reject's time (#436). Test mode refuses what its check fails.
bool OrderState_::refused(onOrderReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return !evnt.testStateMachineCheckResult_;
    }
    return OrdStateImpl::processRefused(evnt);
}

void OrderState_::reject(onOrderReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }
    assert(nullptr == orderData_);
    OrdStateImpl::processReject(&orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::receive(onRplOrderReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }
    assert(nullptr == orderData_);
    assert(nullptr != evnt.order_);
    try
    {
        OrdStateImpl::processReceive(&orderData_, evnt);
        evnt.order4StateMachine_ = orderData_;
    }
    catch (const std::exception &ex)
    {
        onRecvRplOrderRejected newEvnt(evnt, evnt.order_, ex.what());
        newEvnt.order4StateMachine_ = orderData_;
        static_cast<OrderState *>(this)->process_event(newEvnt);
        throw TransitionRefused(ex.what());
    }
}

void OrderState_::accept(onExternalOrder const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    try
    {
        OrdStateImpl::processAccept(&orderData_, evnt);
        evnt.order4StateMachine_ = orderData_;
    }
    catch (const std::exception &ex)
    {
        onExternalOrderRejected newEvnt(evnt, evnt.order_, ex.what());
        newEvnt.order4StateMachine_ = orderData_;
        static_cast<OrderState *>(this)->process_event(newEvnt);
        throw TransitionRefused(ex.what());
    }
}

void OrderState_::reject(onExternalOrderRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(&orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::reject(onReplaceRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::reject(onCancelRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::accept(onReplace const &evnt)
{
    if (evnt.testStateMachine_)
    {
        if (!evnt.testStateMachineCheckResult_)
        {
            onRplOrderRejected newEvnt(evnt, "Test reject");
            static_cast<OrderState *>(this)->process_event(newEvnt);
            throw TransitionRefused("Test reject");
        }
        return;
    }

    try
    {
        OrdStateImpl::processAccept(orderData_, evnt);
        evnt.order4StateMachine_ = orderData_;
    }
    catch (const std::exception &ex)
    {
        onRplOrderRejected newEvnt(evnt, ex.what());
        newEvnt.order4StateMachine_ = orderData_;
        static_cast<OrderState *>(this)->process_event(newEvnt);
        throw TransitionRefused(ex.what());
    }
}

bool OrderState_::hasFills(onReplace const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }
    assert(nullptr != orderData_);
    return 0 < orderData_->cumQty_;
}

/// A replacement with fills and nothing left to trade, which Processor::process(onReplace) amended to what its original
/// had filled (#381). Test mode has one answer to give, and hasFills gives it, so it never takes this row.
bool OrderState_::isFilled(onReplace const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return false;
    }
    assert(nullptr != orderData_);
    return (0 < orderData_->cumQty_) && (0 == orderData_->leavesQty_);
}

bool OrderState_::notexecuted(onTradeExecution const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processNotExecuted(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

bool OrderState_::notexecuted(onTradeCrctCncl const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processNotExecuted(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

bool OrderState_::notexecuted(onNewDay const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processNotExecuted(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

bool OrderState_::notexecuted(onContinue const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processNotExecuted(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

bool OrderState_::complete(onTradeExecution const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processComplete(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

void OrderState_::corrected(onTradeCrctCncl const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    // Zone 2 owns a correction of an order it holds canceled or replaced (#777): here, zone 1 still New or PartFill,
    // it books nothing and gives no leaves
    if (closedInZone2(static_cast<const OrderState &>(*this).current_state()))
    {
        OrdStateImpl::processCorrectedClosed(orderData_, evnt);
    }
    else
    {
        OrdStateImpl::processCorrected(orderData_, evnt);
    }
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::correctedWithoutRestore(onTradeCrctCncl const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    // A suspended order keeps its leaves by design, unless zone 2 holds it canceled or replaced (#777)
    if (closedInZone2(static_cast<const OrderState &>(*this).current_state()))
    {
        OrdStateImpl::processCorrectedClosed(orderData_, evnt);
    }
    else
    {
        OrdStateImpl::processCorrectedWithoutRestore(orderData_, evnt);
    }
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::correctedClosed(onTradeCrctCncl const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processCorrectedClosed(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::expire(onExpired const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processExpire(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::cancel(onCanceled const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processCancel(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::restored(onNewDay const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processRestored(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::continued(onContinue const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processContinued(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::finished(onFinished const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processFinished(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::suspended(onSuspended const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processSuspended(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::canceled(onExecCancel const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processCanceled(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::canceled(onInternalCancel const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processCanceled(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::replaced(onExecReplace const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReplaced(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::receive(onReplaceReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReceive(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

bool OrderState_::acceptable(onReplaceReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processAcceptable(evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

void OrderState_::receive(onCancelReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReceive(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

bool OrderState_::acceptable(onCancelReceived const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return evnt.testStateMachineCheckResult_;
    }

    bool res = OrdStateImpl::processAcceptable(evnt);
    evnt.order4StateMachine_ = orderData_;
    return res;
}

void OrderState_::reject(onRplOrderRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::reject(onOrderRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::rejectNew(onOrderRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processRejectNew(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::reject(onRecvOrderRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(&orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::fill(onTradeExecution const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processFill(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::reject(onRecvRplOrderRejected const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processReject(&orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::expire(onRplOrderExpired const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processExpire(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}
