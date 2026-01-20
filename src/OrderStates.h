/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "OrderStateEvents.h"
#include <boost/msm/front/states.hpp>
#include <boost/msm/front/functor_row.hpp>

#include <iostream>

namespace COP{
namespace OrdState{

struct Rcvd_New : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {}

    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {}

	template <class FSM> void on_exit(onOrderReceived const&, FSM& ){}
	template <class FSM> void on_exit(onRecvOrderRejected const&, FSM& ){}
	template <class FSM> void on_exit(onRplOrderReceived const&, FSM& ){}
	template <class FSM> void on_exit(onRecvRplOrderRejected const&, FSM& ){}
	template <class FSM> void on_exit(onExternalOrder const&, FSM& ){}
	template <class FSM> void on_exit(onExternalOrderRejected const&, FSM& ){}
};

struct NoCnlReplace : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: NoCnlReplace" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: NoCnlReplace" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onCancelRejected const&, FSM& );
	template <class FSM> void on_entry(onReplaceRejected const&, FSM& );
};

struct Pend_New : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: Pend_New" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Pend_New" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onOrderAccepted const&, FSM& );
};

struct Pend_Replace : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: Pend_Replace" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Pend_Replace" << std::endl;
#endif
	}
};

struct Rejected : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: Rejected" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Rejected" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onOrderAccepted const&, FSM& );
	template <class FSM> void on_entry(onOrderRejected const&, FSM& );
	template <class FSM> void on_entry(onRplOrderRejected const&, FSM& );
	template <class FSM> void on_entry(onExternalOrder const&, FSM& );
	template <class FSM> void on_entry(onReplace const&, FSM& );

	template <class FSM> void on_entry(onRecvOrderRejected const&, FSM& );
	template <class FSM> void on_entry(onRecvRplOrderRejected const&, FSM& );
	template <class FSM> void on_entry(onExternalOrderRejected const&, FSM& );
};

struct New : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: New" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: New" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onExternalOrder const&, FSM& );
	template <class FSM> void on_entry(onOrderReceived const&, FSM& );
	template <class FSM> void on_entry(onOrderAccepted const&, FSM& );
	template <class FSM> void on_entry(onReplace const&, FSM& );
	template <class FSM> void on_entry(onTradeCrctCncl const&, FSM& );
	template <class FSM> void on_entry(onNewDay const&, FSM& );
	template <class FSM> void on_entry(onContinue const&, FSM& );
};

struct PartFill : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: PartFill" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: PartFill" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onTradeExecution const&, FSM& );
	template <class FSM> void on_entry(onTradeCrctCncl const&, FSM& );
	template <class FSM> void on_entry(onNewDay const&, FSM& );
	template <class FSM> void on_entry(onContinue const&, FSM& );
};

struct Filled : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: Filled" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Filled" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onTradeExecution const&, FSM& );
};

struct Expired : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: Expired" << std::endl;
#endif
	}
    template <class Event, class FSM>
	void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Expired" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onExpired const&, FSM& );
	template <class FSM> void on_entry(onRplOrderExpired const&, FSM& );
	template <class FSM> void on_entry(onTradeCrctCncl const&, FSM& );
};

struct CnclReplaced : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: CnclReplaced" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: CnclReplaced" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onExecCancel const&, FSM& );
	template <class FSM> void on_entry(onInternalCancel const&, FSM& );
	template <class FSM> void on_entry(onExecReplace const&, FSM& );
	template <class FSM> void on_entry(onTradeCrctCncl const&, FSM& );
};

struct Suspended : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: Suspended" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Suspended" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onSuspended const&, FSM& );
	template <class FSM> void on_entry(onTradeCrctCncl const&, FSM& );
};

struct DoneForDay : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: DoneForDay" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: DoneForDay" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onFinished const&, FSM& );
	template <class FSM> void on_entry(onTradeCrctCncl const&, FSM& );
};

struct GoingCancel : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: GoingCancel" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: GoingCancel" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onCancelReceived const&, FSM& );
};

struct GoingReplace : public boost::msm::front::state<>
{
    template <class Event, class FSM>
    void on_entry(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "entering: GoingReplace" << std::endl;
#endif
	}
    template <class Event, class FSM>
    void on_exit(Event const& , FSM& ) {
#ifdef _DEBUG
		//std::cout << "leaving: GoingReplace" << std::endl;
#endif
	}

	template <class FSM> void on_entry(onReplaceReceived const&, FSM& );
};

}
}
