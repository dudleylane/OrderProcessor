/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"

#include <oneapi/tbb/mutex.h>
#include <atomic>
#include <set>
#include <vector>
#include <map>

#include "SubscriptionDef.h"
#include "DataModelDef.h"


namespace COP{ namespace SubscrMgr{

	class OrderFilter;
	class EventHandler;

	enum SubscriptionType{
		INVALID_SUBSCRIPTION = 0,
		ORDER_SUBSCRIPTION,
		EXEC_SUBSCRIPTION,
		MARKETDATA_SUBSCRIPTION,
		TIMER_SUBSCRIPTION,
		HALT_SUBSCRIPTION,
		ALERT_SUBSCRIPTION,
		TRANSACTION_SUBSCRIPTION,
		INVLDTRANSACT_SUBSCRIPTION,
		FAILEDTRANSACT_SUBSCRIPTION,
		CANCELTRANSACT_SUBSCRIPTION,
		NOSUBSCRIPTION_SUBSCRIPTION,
		TOTAL_SUBSCRIPTION
	};

	struct SubscriptionInfo{
		IdT id_;
		std::string name_;
		SubscriptionType type_;
		union VALUE {
			OrderFilter *order_;
		} subscription_;
		IdT handlerId_;
	};

	class SubscrInfoByID: public std::less<SubscriptionInfo>{
	public:
		bool operator()(const SubscriptionInfo& lft, const SubscriptionInfo& rght) const;
	};

	class SubscrInfoByName: public std::less<SubscriptionInfo>{
	public:
		bool operator()(const SubscriptionInfo& lft, const SubscriptionInfo& rght) const;
	};

class SubscrManager: public SubscriptionManager
{
public:
	SubscrManager(void);
	~SubscrManager(void);

	void getSubscribers(const OrderEntry &order, MatchedSubscribersT *subscribers)const;
public:
	// Reimplemented from SubscriptionManager
	virtual void addSubscription(const std::string &name, OrderFilter *filter, const SubscriberIdT &handlerId);
	virtual void removeSubscriptions(const SubscriberIdT &handlerId);

private:
	std::atomic<u64> subscrCounter_;

	typedef std::set<SubscriptionInfo, SubscrInfoByID> SubscriptionsT;
	SubscriptionsT availableSubscriptions_;

	typedef std::set<SubscriptionInfo, SubscrInfoByName> SubscriptionsByNameT;
	SubscriptionsByNameT subscriptionsByName_;

	typedef std::deque<SubscriptionInfo> SubscriptionsListT;
	typedef std::map<SubscriberIdT, SubscriptionsListT> SubscriptionsByHandlerT;
	SubscriptionsByHandlerT subscriptionsByHandler_;

	typedef std::map<InstrumentEntry, SubscriptionsT, GroupInstrumentsBySymbol> SubscriptionsByInstrumentGroupT;

	struct SubscriptionsGroup{
		SubscriptionsByInstrumentGroupT groupedByInstruments_;
		SubscriptionsT generalSubscriptions_;
	};
	// subscrptions for each SubscriptionType
	typedef std::vector<SubscriptionsGroup> SubscriptionsByTypeT;
	typedef std::map<SubscriberIdT, SubscriptionsByTypeT> SubscriptionsBySubscriberT;
	SubscriptionsBySubscriberT subscriptionsBySubscriber_;

	mutable oneapi::tbb::mutex lock_;
};

typedef aux::Singleton<SubscrManager> SubscriptionMgr;

}}