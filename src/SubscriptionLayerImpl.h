/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "SubscriptionLayerDef.h"

namespace COP{ namespace SL{

class SubscriptionLayerImpl: public SubscriptionLayer
{
public:
	SubscriptionLayerImpl(void);
	~SubscriptionLayerImpl(void);

public:
	virtual void attach(SubscrMgr::SubscriptionManager *subscMgr);
	virtual SubscrMgr::SubscriptionManager* dettach();

	virtual void process(const OrderEntry &order, const SubscrMgr::MatchedSubscribersT &subscr);
private:
	SubscrMgr::SubscriptionManager* subscrMgr_;
};

}}