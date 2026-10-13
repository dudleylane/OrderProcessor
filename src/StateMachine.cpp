/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include <stdexcept>
#include <boost/mpl/size.hpp>
#include "StateMachine.h"
#include "DataModelDef.h"
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

// Every stored order holds its states' ids (#150), so each id is pinned here to the index of its name above. MSM numbers
// the states by their first appearance in the transition table, sources first, then the states that are only targets.
// An edit of the table that moves a state fails to compile here, instead of reinterpreting every order already stored.
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
static_assert(stateId<Rejected> == 12, "Rejected's id is persisted");
static_assert(boost::mpl::size<boost::msm::back::generate_state_set<OrderState::stt>::type>::value == ORDER_STATE_COUNT,
              "a new state needs its id pinned above and a name in state_names");
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

void OrderState_::receive(onOrderReceived const &evnt)
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
        onRecvOrderRejected newEvnt(evnt, evnt.order_, ex.what());
        newEvnt.order4StateMachine_ = orderData_;
        static_cast<OrderState *>(this)->process_event(newEvnt);
        throw;
    }
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
        throw;
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
        throw;
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
            throw std::runtime_error("Test reject");
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
        throw;
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

    OrdStateImpl::processCorrected(orderData_, evnt);
    evnt.order4StateMachine_ = orderData_;
}

void OrderState_::correctedWithoutRestore(onTradeCrctCncl const &evnt)
{
    if (evnt.testStateMachine_)
    {
        return;
    }

    OrdStateImpl::processCorrectedWithoutRestore(orderData_, evnt);
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
