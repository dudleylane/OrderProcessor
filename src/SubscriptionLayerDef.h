/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "SubscriptionDef.h"
#include "DataModelDef.h"

namespace COP{ 

	namespace SubscrMgr{
		class SubscriptionManager;
	}

namespace SL{

	class SubscriptionLayer{
	public:
		virtual ~SubscriptionLayer(){}

		virtual void attach(SubscrMgr::SubscriptionManager *subscMgr) = 0;
		virtual SubscrMgr::SubscriptionManager* dettach() = 0;

		virtual void process(const OrderEntry &order, const SubscrMgr::MatchedSubscribersT &subscr) = 0;
	};

}}