/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "EventManager.h"
#include "DataModelDef.h"
#include "SubscrManager.h"
#include "SubscriptionLayerDef.h"
#include "Logger.h"

using namespace COP::EventMgr;
using namespace COP::SubscrMgr;
using namespace COP::SL;

EventManager::EventManager(void): subscrMgr_(nullptr), sl_(nullptr)
{
	subscrMgr_ = SubscriptionMgr::instance();
	aux::ExchLogger::instance()->note("EventManager created.");
}

EventManager::~EventManager(void)
{
	aux::ExchLogger::instance()->note("EventManager destroyed.");
}

SubscriptionManager *EventManager::getSubscriptionManager()const
{
	return subscrMgr_;
}

void EventManager::attach(SubscriptionLayer *sl)
{
	assert(nullptr == sl_);
	assert(nullptr != sl);
	sl_ = sl;
	aux::ExchLogger::instance()->note("EventManager attached with SubscriptionLayer.");
}

SubscriptionLayer *EventManager::dettach()
{
	SubscriptionLayer *sl = sl_;
	sl_ = nullptr;
	aux::ExchLogger::instance()->note("EventManager dettached from SubscriptionLayer.");
	return sl;
}

void EventManager::dispatch(const NewOrderEvent &evnt)const
{
	MatchedSubscribersT subscribers;
	subscrMgr_->getSubscribers(*evnt.order_, &subscribers);
	assert(nullptr != sl_);
	sl_->process(*evnt.order_, subscribers);
}
