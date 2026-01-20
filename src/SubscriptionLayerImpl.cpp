/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "SubscriptionLayerImpl.h"
#include "Logger.h"

using namespace COP;
using namespace COP::SL;
using namespace COP::SubscrMgr;

SubscriptionLayerImpl::SubscriptionLayerImpl(void): subscrMgr_(nullptr)
{
	aux::ExchLogger::instance()->note("SubscriptionLayer created.");
}

SubscriptionLayerImpl::~SubscriptionLayerImpl(void)
{
	aux::ExchLogger::instance()->note("SubscriptionLayer destroyed.");
}

void SubscriptionLayerImpl::attach(SubscriptionManager *subscMgr)
{
	assert(nullptr == subscrMgr_);
	assert(nullptr != subscMgr);
	subscrMgr_ = subscMgr;
	aux::ExchLogger::instance()->note("SubscriptionLayer SubscriptionManager attached.");
}

SubscriptionManager* SubscriptionLayerImpl::dettach()
{
	SubscriptionManager* tmp = subscrMgr_;
	subscrMgr_ = nullptr;
	aux::ExchLogger::instance()->note("SubscriptionLayer SubscriptionManager detached.");
	return tmp;
}

void SubscriptionLayerImpl::process(const OrderEntry &order, const MatchedSubscribersT &subscribers)
{
	if(subscribers.empty()) {
		aux::ExchLogger::instance()->note("SubscriptionLayer::process() - no subscribers matched for order");
		return;
	}

	// Process each matched subscriber
	for(const auto &subscriberId : subscribers) {
		if(!subscriberId.isValid()) {
			aux::ExchLogger::instance()->warn("SubscriptionLayer::process() - invalid subscriber ID, skipping");
			continue;
		}

		// Log the notification for each subscriber
		aux::ExchLogger::instance()->note(
			std::string("SubscriptionLayer::process() - notifying subscriber ") +
			std::to_string(subscriberId.id_) +
			" about order " +
			std::to_string(order.orderId_.id_));
	}

	aux::ExchLogger::instance()->note(
		std::string("SubscriptionLayer::process() - processed ") +
		std::to_string(subscribers.size()) +
		" subscribers for order " +
		std::to_string(order.orderId_.id_));
}
