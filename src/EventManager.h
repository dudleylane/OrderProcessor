/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "EventDef.h"
#include "Singleton.h"

namespace COP{ 
	namespace SubscrMgr{
		class SubscrManager;
		class SubscriptionManager;
	}

	namespace SL{
		class SubscriptionLayer;
	}

namespace EventMgr{

class EventManager: public EventDispatcher
{
public:
	EventManager(void);
	~EventManager(void);

	SubscrMgr::SubscriptionManager *getSubscriptionManager()const;

	void attach(SL::SubscriptionLayer *sl);
	SL::SubscriptionLayer *dettach();

public:
	/// Reimplemented from EventDispatcher
	virtual void dispatch(const NewOrderEvent &evnt)const;

private:
	SubscrMgr::SubscrManager *subscrMgr_;
	SL::SubscriptionLayer *sl_;
};

aux::Singleton<EventManager> EventDispatcher;

}}