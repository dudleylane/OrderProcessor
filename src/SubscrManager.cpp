/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "SubscrManager.h"

#include "OrderFilter.h"

using namespace COP;
using namespace COP::SubscrMgr;
using namespace tbb;

bool SubscrInfoByID::operator()(const SubscriptionInfo& lft, const SubscriptionInfo& rght) const
{
	return lft.id_ < rght.id_;
}

bool SubscrInfoByName::operator()(const SubscriptionInfo& lft, const SubscriptionInfo& rght) const
{
	return lft.name_ < rght.name_;
}


SubscrManager::SubscrManager(void)
{
	subscrCounter_.store(1);
}

SubscrManager::~SubscrManager(void)
{
}

void SubscrManager::addSubscription(const std::string &name, OrderFilter *filter, 
									const SubscriberIdT &handlerId)
{
	SubscriptionInfo val;
	val.id_.date_ = 0;
	val.id_.id_ = subscrCounter_.fetch_add(1);
	val.name_ = name;
	val.subscription_.order_ = filter;
	val.type_ = ORDER_SUBSCRIPTION;
	val.handlerId_ = handlerId;

	InstrumentEntry instr;
	bool isGeneral = !filter->getInstrument(&instr);
	{
		tbb::mutex::scoped_lock lock(lock_);
		availableSubscriptions_.insert(val);
		SubscriptionsBySubscriberT::iterator sit = subscriptionsBySubscriber_.find(handlerId);
		if(subscriptionsBySubscriber_.end() == sit){
			SubscriptionsByTypeT tmp(TOTAL_SUBSCRIPTION);
			sit = subscriptionsBySubscriber_.insert(
				SubscriptionsBySubscriberT::value_type(handlerId, tmp)).first;
		}
		if(isGeneral)
			sit->second[ORDER_SUBSCRIPTION].generalSubscriptions_.insert(val);
		else{
			SubscriptionsByInstrumentGroupT::iterator git = 
						sit->second[ORDER_SUBSCRIPTION].groupedByInstruments_.find(instr);
			if(sit->second[ORDER_SUBSCRIPTION].groupedByInstruments_.end() == git){
				git = sit->second[ORDER_SUBSCRIPTION].groupedByInstruments_.insert(
							SubscriptionsByInstrumentGroupT::value_type(instr, SubscriptionsT())).first;
			}
			git->second.insert(val);
		}
		subscriptionsByHandler_[handlerId].push_back(val);
	}
}

void SubscrManager::removeSubscriptions(const SubscriberIdT &handlerId){
	SubscriptionsListT lst;
	{
		tbb::mutex::scoped_lock lock(lock_);
		SubscriptionsByHandlerT::iterator it = subscriptionsByHandler_.find(handlerId);
		if(subscriptionsByHandler_.end() == it)
			return;
		std::swap(it->second, lst);
		subscriptionsBySubscriber_.erase(handlerId);
	}
	for(SubscriptionsListT::const_iterator it = lst.begin(); it != lst.end(); ++it){
		switch (it->type_){
		case ORDER_SUBSCRIPTION:
			it->subscription_.order_->release();
			delete it->subscription_.order_;
			break;
		case EXEC_SUBSCRIPTION:
		case MARKETDATA_SUBSCRIPTION:
		case TIMER_SUBSCRIPTION:
		case HALT_SUBSCRIPTION:
		case ALERT_SUBSCRIPTION:
		case TRANSACTION_SUBSCRIPTION:
		case INVLDTRANSACT_SUBSCRIPTION:
		case FAILEDTRANSACT_SUBSCRIPTION:
		case CANCELTRANSACT_SUBSCRIPTION:
		case NOSUBSCRIPTION_SUBSCRIPTION:
		default:
			break;
		};
	}
}

void SubscrManager::getSubscribers(const OrderEntry &order, MatchedSubscribersT *subscribers)const
{
	{
		tbb::mutex::scoped_lock lock(lock_);
		for(SubscriptionsBySubscriberT::const_iterator hit = subscriptionsBySubscriber_.begin(); 
			hit != subscriptionsBySubscriber_.end(); ++hit){
			assert(TOTAL_SUBSCRIPTION == hit->second.size());

			bool found = false;
			SubscriptionsByInstrumentGroupT::const_iterator git = 
				hit->second[ORDER_SUBSCRIPTION].groupedByInstruments_.find(order.instrument_.get());
			if(hit->second[ORDER_SUBSCRIPTION].groupedByInstruments_.end() != git){
				SubscriptionsT::const_iterator sit = git->second.begin(),
					eit = git->second.end();
				for(SubscriptionsT::const_iterator it = sit; it != eit; ++it){
					if(ORDER_SUBSCRIPTION != it->type_)
						throw std::runtime_error("Subscription with invalid type enqueued at Orders subscriptions!");
					if((nullptr != it->subscription_.order_)&&(it->subscription_.order_->match(order))){
						subscribers->push_back(it->handlerId_);
						found = true;
						break;
					}
				}
			}

			if(!found){
				SubscriptionsT::const_iterator bit = hit->second[ORDER_SUBSCRIPTION].generalSubscriptions_.begin(),
					eit = hit->second[ORDER_SUBSCRIPTION].generalSubscriptions_.end();
				for(SubscriptionsT::const_iterator it = bit; it != eit; ++it){
					if(ORDER_SUBSCRIPTION != it->type_)
						throw std::runtime_error("Subscription with invalid type enqueued at Orders subscriptions!");
					if((nullptr != it->subscription_.order_)&&(it->subscription_.order_->match(order))){
						subscribers->push_back(it->handlerId_);
						break;
					}
				}
			}
		}
	}
}

