/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "OrderStateEvents.h"
#include <boost/msm/state_machine.hpp>

#include <iostream>

namespace COP{
namespace OrdState{

struct Rcvd_New : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    /*template <class Event>
    void on_entry(Event const& evnt) {
		//std::cout << "entering: Rcvd_New" << std::endl;
	}*/
    /*template <class Event>
    void on_exit(Event const& evnt) {//std::cout << "leaving: Rcvd_New" << std::endl;}*/

	void on_exit(onOrderReceived const& ){}
	void on_exit(onRecvOrderRejected const& ){}
	void on_exit(onRplOrderReceived const& ){}
	void on_exit(onRecvRplOrderRejected const& ){}
	void on_exit(onExternalOrder const& ){}
	void on_exit(onExternalOrderRejected const& ){}
};

struct NoCnlReplace : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: NoCnlReplace" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG		
		//std::cout << "leaving: NoCnlReplace" << std::endl;
#endif	
	}

	void on_entry(onCancelRejected const& );
	void on_entry(onReplaceRejected const& );
};

struct Pend_New : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& /*evnt*/) {
#ifdef _DEBUG
		//std::cout << "entering: Pend_New" << std::endl;
#endif
	}
    template <class Event>
    void on_exit(Event const& /*evnt*/) {
#ifdef _DEBUG
		//std::cout << "leaving: Pend_New" << std::endl;
#endif	
	}

	void on_entry(onOrderAccepted const& );
};

struct Pend_Replace : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: Pend_Replace" << std::endl;
#endif
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Pend_Replace" << std::endl;
#endif
	}
};

struct Rejected : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: Rejected" << std::endl;
#endif
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Rejected" << std::endl;
#endif
	}


	void on_entry(onOrderAccepted const& );
	void on_entry(onOrderRejected const& );
	void on_entry(onRplOrderRejected const& );
	void on_entry(onExternalOrder const& );
	void on_entry(onReplace const& );
	
	void on_entry(onRecvOrderRejected const& );
	void on_entry(onRecvRplOrderRejected const& );
	void on_entry(onExternalOrderRejected const& );	
};

struct New : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: New" << std::endl;
#endif
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: New" << std::endl;
#endif	
	}

	void on_entry(onExternalOrder const& );
	void on_entry(onOrderReceived const& );
	void on_entry(onOrderAccepted const& );
	void on_entry(onReplace const& );
	void on_entry(onTradeCrctCncl const& );
	void on_entry(onNewDay const& );
	void on_entry(onContinue const& );
};

struct PartFill : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: PartFill" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: PartFill" << std::endl;
#endif	
	}


	void on_entry(onTradeExecution const& );
	void on_entry(onTradeCrctCncl const& );
	void on_entry(onNewDay const& );
	void on_entry(onContinue const& );

};

struct Filled : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: Filled" << std::endl;
#endif
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Filled" << std::endl;
#endif
	}

	
	void on_entry(onTradeExecution const& );
};

struct Expired : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: Expired" << std::endl;
#endif	
	}
    template <class Event>
	void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Expired" << std::endl;
#endif	
	}


	void on_entry(onExpired const& );
	void on_entry(onRplOrderExpired const& );
	void on_entry(onTradeCrctCncl const& );
};

struct CnclReplaced : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: CnclReplaced" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: CnclReplaced" << std::endl;
#endif	
	}

	void on_entry(onExecCancel const& );
	void on_entry(onInternalCancel const& );
	void on_entry(onExecReplace const& );
	void on_entry(onTradeCrctCncl const& );
};

struct Suspended : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: Suspended" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: Suspended" << std::endl;
#endif	
	}


	void on_entry(onSuspended const& );
	void on_entry(onTradeCrctCncl const& );
};

struct DoneForDay : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: DoneForDay" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: DoneForDay" << std::endl;
#endif	
	}


	void on_entry(onFinished const& );
	void on_entry(onTradeCrctCncl const& );
};

struct GoingCancel : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: GoingCancel" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: GoingCancel" << std::endl;
#endif	
	}


	void on_entry(onCancelReceived const& );
};

struct GoingReplace : public boost::msm::state<> 
{
    // every (optional) entry/exit methods get the event passed.
    template <class Event>
    void on_entry(Event const& ) {
#ifdef _DEBUG
		//std::cout << "entering: GoingReplace" << std::endl;
#endif	
	}
    template <class Event>
    void on_exit(Event const& ) {
#ifdef _DEBUG
		//std::cout << "leaving: GoingReplace" << std::endl;
#endif	
	}


	void on_entry(onReplaceReceived const& );
};



}
}