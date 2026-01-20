/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <deque>
#include "TypesDef.h"

namespace COP{ namespace SubscrMgr{

	class OrderFilter;

	typedef std::deque<IdT> MatchedSubscribersT;

class SubscriptionManager{
public:
	~SubscriptionManager(){};

	virtual void addSubscription(const std::string &name, OrderFilter *filter, 
								 const SubscriberIdT &subscriber) = 0;
	virtual void removeSubscriptions(const SubscriberIdT &subscriber) = 0;
};

}}