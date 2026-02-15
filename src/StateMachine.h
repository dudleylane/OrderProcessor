/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#ifndef _STATE_MACHINE__H
#define _STATE_MACHINE__H

#include <stdexcept>
#include <vector>

//#define BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS
//#define BOOST_MPL_LIMIT_VECTOR_SIZE 50

#include "boost/mpl/vector/vector50.hpp"
#include <boost/msm/back/state_machine.hpp>
#include <boost/msm/front/state_machine_def.hpp>
#include "OrderStateEvents.h"
#include "OrderStates.h"
#include "StateMachineDef.h"

namespace mpl = boost::mpl;

namespace COP{

	struct OrderEntry;

namespace OrdState{

// Front-end state machine definition
struct OrderState_ : public boost::msm::front::state_machine_def<OrderState_>{
public:
	OrderState_();
	explicit OrderState_(OrderEntry *orderData);
	~OrderState_();

	static const std::string &getStateName(int idx);

	OrderStatePersistence getPersistence()const;
	void setPersistance(const OrderStatePersistence &state);
public:
	/// main zone

	/// New order event was received
	void receive(onOrderReceived const&);
	/// Order replace event was received
	void receive(onRplOrderReceived const&);

	void accept(onOrderAccepted const&);
	void accept(onExternalOrder const&);
	void reject(onExternalOrderRejected const&);
	void reject(onRecvOrderRejected const&);
	void reject(onOrderRejected const&);
	void rejectNew(onOrderRejected const&);
	void reject(onRecvRplOrderRejected const&);
	void reject(onRplOrderRejected const&);

	void accept(onReplace const&);

	void fill(onTradeExecution const&);
	bool notexecuted(onTradeExecution const &);
	bool notexecuted(onTradeCrctCncl const &);
	bool notexecuted(onNewDay const &);
	bool notexecuted(onContinue const &);
	bool complete(onTradeExecution const &);
	void corrected(onTradeCrctCncl const &);
	void correctedWithoutRestore(onTradeCrctCncl const &);
	void expire(onRplOrderExpired const&);
	void expire(onExpired const&);
	void cancel(onCanceled const&);
	void restored(onNewDay const &);
	void continued(onContinue const &);
	void finished(onFinished const&);
	void suspended(onSuspended const&);

	// cancel/replace zone
	void receive(onReplaceReceived const&);
	bool acceptable(onReplaceReceived const&);
	void receive(onCancelReceived const&);
	bool acceptable(onCancelReceived const&);
	void reject(onCancelRejected const &);
	void canceled(onExecCancel const&);
	void canceled(onInternalCancel const&);
	void reject(onReplaceRejected const &);
	void replaced(onExecReplace const&);



	typedef mpl::vector<Rcvd_New, NoCnlReplace> initial_state;
	typedef OrderState_ os;

public:
	// Transition table for Order
	// ToDo: add execCorrect, when qty not changed Fill->Fill for example
	// ToDo: add administrative switch to any state
	struct transition_table : mpl::vector42<
		//    Start                Event           Next           Action				Guard
		//	  +-------------+------------------+-------------+------------+----------------------+
		// create OrderEntry from received event and generate id
		//a_row< Rcvd_New,	 onOrderReceived,	Pend_New,	  &os::receive   			         >,
		a_row< Rcvd_New,	 onOrderReceived,	New,	  &os::receive   			         >,
		// generate id and create ExecReport Reject
		a_row< Rcvd_New,	 onRecvOrderRejected,Rejected,	  &os::reject				         >,
		// create OrderEntry from received event and generate id
		// apply onReplaceReceived event to the original order
		a_row< Rcvd_New,	 onRplOrderReceived,Pend_Replace, &os::receive    					 >,
		// generate id and create ExecReport Reject
		a_row< Rcvd_New,	 onRecvRplOrderRejected,Rejected, &os::reject				         >,
		// generate id and create ExecReport Reject
		a_row< Rcvd_New,	 onExternalOrderRejected,Rejected,&os::reject				         >,
		// create OrderEntry from received event, generate id, add to OrderBook, prepare ExecReport New
		a_row< Rcvd_New,	 onExternalOrder,	New,		  &os::accept						 >,

		// apply onExecReplace to the original order and add to OrderBook
		// Create ExecReport New
		a_row< Pend_Replace, onReplace,			New,		  &os::accept						 >,
		// apply onReplaceRejected to the original order
		// create ExecReport Rejected
		a_row< Pend_Replace, onRplOrderRejected,Rejected,	  &os::reject				         >,
		// apply onReplaceRejected to the original order
		// create ExecReport Expire
		a_row< Pend_Replace, onRplOrderExpired,	Expired,	  &os::expire						 >,

		// NB: called if complete(onTradeExecution) returns false
		// update orderentry according to trade
		// create ExecReport PartFill
		a_row< New,			 onTradeExecution,	PartFill,	  &os::fill				             >,
		/// should be after New->PartFill to switch state depend on complete
		// update orderentry according to trade, remove from OrderBook
		// create ExecReport Fill
		row  < New,			 onTradeExecution,	Filled,		  &os::fill,   &os::complete		 >,
		// remove from OrderBook
		// create ExecReport Rejected
		a_row< New,			 onOrderRejected,   Rejected,	  &os::rejectNew				         >,
		// remove from OrderBook
		// create ExecReport Expired
		a_row< New,			 onExpired,			Expired,	  &os::expire						 >,
		// remove from OrderBook
		// create ExecReport Finished
		a_row< New,			 onFinished,		DoneForDay,   &os::finished	                     >,
		// remove from OrderBook
		// create ExecReport Suspended
		a_row< New,			 onSuspended,		Suspended,    &os::suspended                     >,

		// NB: called if complete(onTradeExecution) returns false
		// update orderentry according to trade
		// create ExecReport PartFill
		a_row< PartFill,	 onTradeExecution,	PartFill,	  &os::fill			                 >,
		/// should be after PartFill->PartFill to switch state depend on complete
		// update orderentry according to trade, remove from OrderBook
		// create ExecReport PartFill
		row  < PartFill,	 onTradeExecution,	Filled,		  &os::fill,   &os::complete		 >,
		// NB: called if notexecuted(onTradeCrctCncl) returns false
		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		a_row< PartFill,	 onTradeCrctCncl,	PartFill,	  &os::corrected	                 >,
		/// should be after PartFill->PartFill to switch state depend on notexecuted
		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		row  < PartFill,	 onTradeCrctCncl,	New,		  &os::corrected, &os::notexecuted	 >,
		// Remove from OrderBook
		// create ExecReport Expired
		a_row< PartFill,	 onExpired,			Expired,	  &os::expire	                     >,
		// Remove from OrderBook
		// create ExecReport Finshed
		a_row< PartFill,	 onFinished,		DoneForDay,   &os::finished	                     >,
		// Remove from OrderBook
		// create ExecReport Suspended
		a_row< PartFill,	 onSuspended,		Suspended,    &os::suspended                     >,

		// NB: called if notexecuted(onTradeCrctCncl) returns false
		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		a_row< Filled,		 onTradeCrctCncl,	PartFill,	  &os::corrected	                 >,
		/// should be after Filled->PartFill to switch state depend on notexecuted
		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		row  < Filled,		 onTradeCrctCncl,	New,		  &os::corrected, &os::notexecuted   >,
		// Filled orders only allow trade corrections (not Expired/DoneForDay/Suspended
		// which could lead to re-activation via onNewDay/onContinue)

		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		a_row< Expired,		 onTradeCrctCncl,   Expired,	  &os::correctedWithoutRestore       >,

		// NB: called if notexecuted(onNewDay) returns false
		// add to OrderBook
		// create ExecReport PartFill
		a_row< DoneForDay,	 onNewDay,			PartFill,	  &os::restored					     >,
		/// should be after DoneForDay->PartFill to switch state depend on notexecuted
		// add to OrderBook
		// create ExecReport New
		row  < DoneForDay,	 onNewDay,			New,		  &os::restored,   &os::notexecuted	 >,
		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		a_row< DoneForDay,   onTradeCrctCncl,	DoneForDay,   &os::correctedWithoutRestore       >,
		// create ExecReport Suspended
		a_row< DoneForDay,   onSuspended,		Suspended,    &os::suspended	                 >,

		// NB: called if notexecuted(onNewDay) returns false
		// add to OrderBook
		// create ExecReport PartFill
		a_row< Suspended,	 onContinue,		PartFill,	  &os::continued					 >,
		/// should be after Suspended->PartFill to switch state depend on notexecuted
		// add to OrderBook
		// create ExecReport New
		row  < Suspended,	 onContinue,		New,		  &os::continued,  &os::notexecuted	 >,
		// create ExecReport Expired
		a_row< Suspended,    onExpired,			Expired,	  &os::expire						 >,
		// create ExecReport Finished
		a_row< Suspended,    onFinished,		DoneForDay,   &os::finished	                     >,
		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		a_row< Suspended,    onTradeCrctCncl,	Suspended,    &os::correctedWithoutRestore       >,

		/// Cancel/Replace zone
		// create ExecReport PendingCancel
		a_row< NoCnlReplace, onCancelReceived,  GoingCancel, &os::receive						 >,
		// create ExecReport PendingReplace
		a_row< NoCnlReplace, onReplaceReceived, GoingReplace, &os::receive						 >,
		// create ExecReport InternalCancel
		a_row< NoCnlReplace, onInternalCancel, CnclReplaced, &os::canceled						 >,

		// nothing to do?
		a_row< GoingCancel,  onCancelRejected,  NoCnlReplace, &os::reject						 >,
		// remove order from OrderBook
		// create ExecReport Canceled
		a_row< GoingCancel,  onExecCancel,		CnclReplaced, &os::canceled						 >,
		// nothing to do?
		a_row< GoingReplace, onReplaceRejected, NoCnlReplace, &os::reject						 >,
		// remove order from OrderBook
		// create ExecReport Replaced
		a_row< GoingReplace, onExecReplace,		CnclReplaced, &os::replaced						 >,

		// update orderentry according to tradecorrect
		// create ExecReport TradeCorrect
		a_row< CnclReplaced, onTradeCrctCncl,	CnclReplaced, &os::correctedWithoutRestore      >
		//    +-------------+------------------+-------------+----------------+------------------+
	> {};

    // Replaces the default no-transition response.
    template <class FSM, class Event>
    void no_transition(Event const&, FSM&, int)
    {
		throw std::runtime_error("no transition!");
        /*std::cout << "no transition from state " << state
            << " on event " << typeid(e).name() << std::endl;*/
        //return state;
    }

    template <class FSM, class Event>
    void exception_caught (Event const&, FSM&, std::exception&)
    {
    }

private:
	OrderEntry *orderData_;
};

// Back-end state machine (wraps the front-end definition)
typedef boost::msm::back::state_machine<OrderState_> OrderState;

}

}

#endif //_STATE_MACHINE__H
