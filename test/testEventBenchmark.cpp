/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <iostream>
#include "SubscrManager.h"
#include "EventDef.h"
#include "OrderFilter.h"
#include "FilterImpl.h"
#include "TypesDef.h"
#include "DataModelDef.h"
#include <tbb/tick_count.h>

using namespace std;
using namespace tbb;
using namespace COP;
using namespace COP::Store;
using namespace COP::SubscrMgr;
using namespace COP::Impl;

namespace {
	SourceIdT g_srcId, g_destId, g_accountId, g_clearingId, g_execList;

	void simpleGoodScenario(const std::vector<std::string> &symbols, const std::vector<SourceIdT> &instrumentIds){
		SubscriberIdT handlerId(1, 0);

		size_t subscrCount = 0;
		cout<< "Start simpleGoodScenario: one subscription for each symbol (subscr has one filter - 'XYZ'<equal>Symbol)"<< endl;
		cout<< "\tPrepare subscriptions..."<< endl;
		tick_count bs = tick_count::now();
		{
			for(size_t i = 0; i < symbols.size(); ++i){
				auto_ptr<OrderFilter> filter(new OrderFilter());
				InstrumentFilter instrFlt;
				auto_ptr<StringEqualFilter> eqSymblFlt(new StringEqualFilter(symbols[i]));
				auto_ptr<InstrumentElementFilter> symblFlt(new InstrumentSymbolFilter(eqSymblFlt.get()));
				eqSymblFlt.release();
				instrFlt.addFilter(symblFlt.release());
				filter->addInstrumentFilter(&instrFlt);
				SubscriptionMgr::instance()->addSubscription("aaa", filter.release(), handlerId);
				++subscrCount;
			}
		}
		tick_count es = tick_count::now();
		double diff = (es - bs).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)subscrCount/diff<< " subscription/sec"<< endl;
		cout<< "\t"<< subscrCount<< " subscriptions created."<< endl;

		cout<< "\tLocate subscriptions..."<< endl;
		tick_count b = tick_count::now();
		{
			MatchedSubscribersT subscribers;

			for(size_t i = 0; i < instrumentIds.size(); ++i){//
				SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
				OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
				{
					subscribers.clear();
					SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
				}
			}
		}
		tick_count e = tick_count::now();
		diff = (e - b).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)instrumentIds.size()/diff<< " events/sec"<< endl;

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
			size_t i = 0;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (first) latency takes "<< diff<< " sec"<< endl;
		}

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
			size_t i = 4998;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (4998) latency takes "<< diff<< " sec"<< endl;
		}

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
			size_t i = 4999;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (4999) latency takes "<< diff<< " sec"<< endl;
		}
		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
			size_t i = 5000;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (5000) latency takes "<< diff<< " sec"<< endl;
		}
		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
			size_t i = 5001;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (5001) latency takes "<< diff<< " sec"<< endl;
		}

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
			size_t i = instrumentIds.size() - 1;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (last) latency takes "<< diff<< " sec"<< endl;
		}

		SubscriptionMgr::instance()->removeSubscriptions(handlerId);
	}

	void simpleWorseScenario(const std::vector<std::string> &symbols, const std::vector<SourceIdT> &instrumentIds){
		SubscriberIdT handlerId(1, 0);
		size_t subscrCount = 0;

		cout<< "Start simpleWorseScenario: one subscription for each symbol (subscr has one filter - 'XY.?'<match>Symbol)"<< endl;
		cout<< "\tPrepare subscriptions..."<< endl;
		tick_count bs = tick_count::now();
		{
			string pattern;
			for(size_t i = 0; i < symbols.size(); ++i){
				string part(symbols[i].c_str(), 2);
				if(pattern != part){
					auto_ptr<OrderFilter> filter(new OrderFilter());
					InstrumentFilter instrFlt;
					auto_ptr<StringFilter> eqSymblFlt(new StringMatchFilter(part + ".?"));//
					auto_ptr<InstrumentElementFilter> symblFlt(new InstrumentSymbolFilter(eqSymblFlt.get()));
					eqSymblFlt.release();
					instrFlt.addFilter(symblFlt.release());
					filter->addInstrumentFilter(&instrFlt);
					SubscriptionMgr::instance()->addSubscription("aaa", filter.release(), handlerId);
					++subscrCount;
					pattern = part;
				}
			}
		}
		tick_count es = tick_count::now();
		double diff = (es - bs).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)subscrCount/diff<< " subscription/sec"<< endl;
		cout<< "\t"<< subscrCount<< " subscriptions created."<< endl;


		cout<< "\tLocate subscriptions..."<< endl;
		tick_count b = tick_count::now();
		{
			MatchedSubscribersT subscribers;

			for(size_t i = 0; i < instrumentIds.size(); ++i){//
				SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;
				OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[i], accountId, clearingId, execList);
				{
					subscribers.clear();
					SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
				}
			}
		}
		tick_count e = tick_count::now();
		diff = (e - b).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)instrumentIds.size()/diff<< " event/sec"<< endl;

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[0], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (first) latency takes "<< diff<< " sec"<< endl;
		}

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;

			b = tick_count::now();
			OrderEntry order(src, dest, clOrderId, orifClOrderID, instrumentIds[instrumentIds.size() - 1], accountId, clearingId, execList);
			SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (last) latency takes "<< diff<< " sec"<< endl;
		}

		SubscriptionMgr::instance()->removeSubscriptions(handlerId);
	}

	void complexWorseScenario(const std::vector<std::string> &symbols, const std::vector<SourceIdT> &instrumentIds){
		SubscriberIdT handlerId(1, 0);
		size_t subscrCount = 0;

		cout<< "Start complexWorseScenario: one subscription for each symbol (subscr has many filters)"<< endl;
		cout<< "\tPrepare subscriptions..."<< endl;
		tick_count bs = tick_count::now();
		{
			string pattern;
			for(size_t i = 0; i < symbols.size(); ++i){
				string part(symbols[i].c_str(), 2);
				if(pattern != part){
					auto_ptr<OrderFilter> filter(new OrderFilter());
					filter->addFilter(new OrderStatusEqualFilter(RECEIVEDNEW_ORDSTATUS));
					filter->addFilter(new SideEqualFilter(BUY_SIDE));
					filter->addFilter(new OrderTypeEqualFilter(LIMIT_ORDERTYPE));
					TIFInFilter::ValuesT tif;
					tif.insert(GTD_TIF);
					tif.insert(GTC_TIF);
					filter->addFilter(new TIFInFilter(tif));
					filter->addFilter(new SettlTypeEqualFilter(_3_SETTLTYPE));
					filter->addFilter(new CapacityEqualFilter(PRINCIPAL_CAPACITY));
					filter->addFilter(new CurrencyEqualFilter(USD_CURRENCY));
					InstructionsInFilter::ValuesT instr;
					instr.insert(STAY_ON_OFFER_SIDE_INSTRUCTION);
					instr.insert(NOT_HELD_INSTRUCTION);
					filter->addFilter(new InstructionsInFilter(instr));
					filter->addFilter(new DestinationFilter(new StringEqualFilter("NASDAQ")));
					filter->addFilter(new SourceFilter(new StringEqualFilter("CLNT")));
					filter->addFilter(new PxFilter(new PriceGreaterFilter(10.50)));
					filter->addFilter(new StopPxFilter(new PriceGreaterFilter(11.50)));
					filter->addFilter(new AvgPxFilter(new PriceGreaterFilter(12.50)));
					filter->addFilter(new DayAvgPxFilter(new PriceGreaterFilter(13.50)));
					filter->addFilter(new MinQtyFilter(new QuantityGreaterFilter(100)));
					filter->addFilter(new OrderQtyFilter(new QuantityGreaterFilter(110)));
					filter->addFilter(new LeavesQtyFilter(new QuantityGreaterFilter(120)));
					filter->addFilter(new CumQtyFilter(new QuantityGreaterFilter(130)));
					filter->addFilter(new DayOrderQtyFilter(new QuantityGreaterFilter(140)));
					filter->addFilter(new DayCumQtyFilter(new QuantityGreaterFilter(150)));
					filter->addFilter(new ExpireTimeFilter(new DateTimeGreaterFilter(1)));
					filter->addFilter(new SettlDateFilter(new DateTimeGreaterFilter(2)));
					filter->addFilter(new CreationTimeFilter(new DateTimeGreaterFilter(3)));
					filter->addFilter(new LastUpdateTimeFilter(new DateTimeGreaterFilter(4)));
					
					{
						InstrumentFilter instrFlt;
						auto_ptr<StringFilter> eqSymblFlt(new StringMatchFilter(part + ".?"));//
						auto_ptr<InstrumentElementFilter> symblFlt(new InstrumentSymbolFilter(eqSymblFlt.get()));
						eqSymblFlt.release();
						instrFlt.addFilter(symblFlt.release());

						instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
						instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

						filter->addInstrumentFilter(&instrFlt);
					}

					{
						AccountFilter accFlt;
						accFlt.addFilter(new AccountAccountFilter(new StringEqualFilter("ACT")));
						accFlt.addFilter(new AccountFirmFilter(new StringEqualFilter("ACTFirm")));
						accFlt.addFilter(new AccountTypeEqualFilter(PRINCIPAL_ACCOUNTTYPE));
						filter->addAccountFilter(&accFlt);
					}

					{
						ClearingFilter clrFlt;
						clrFlt.addFilter(new ClearingFirmFilter(new StringEqualFilter("CLRFirm")));
						filter->addClearingFilter(&clrFlt);
					}

					SubscriptionMgr::instance()->addSubscription("aaa", filter.release(), handlerId);
					++subscrCount;
					pattern = part;
				}
			}
		}
		tick_count es = tick_count::now();
		double diff = (es - bs).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)subscrCount/diff<< " subscription/sec"<< endl;
		cout<< "\t"<< subscrCount<< " subscriptions created."<< endl;


		cout<< "\tLocate subscriptions..."<< endl;
		tick_count b = tick_count::now();
		{
			MatchedSubscribersT subscribers;

			for(size_t i = 0; i < instrumentIds.size(); ++i){//
				SourceIdT clOrderId, origClOrderID;
				OrderEntry order(g_srcId, g_destId, clOrderId, origClOrderID, instrumentIds[i], g_accountId, g_clearingId, g_execList);
				order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
				order.creationTime_ = 4; 
				order.lastUpdateTime_ = 5;
				order.expireTime_ = 2;
				order.settlDate_ = 3;

				order.price_ = 10.51; //8
				order.stopPx_ = 11.51;
				order.avgPx_ = 12.51;
				order.dayAvgPx_ = 13.51;

				order.status_ = RECEIVEDNEW_ORDSTATUS;
				order.side_ = BUY_SIDE;
				order.ordType_ = LIMIT_ORDERTYPE;
				order.tif_ = GTD_TIF;
				order.settlType_ = _3_SETTLTYPE;
				order.capacity_ = PRINCIPAL_CAPACITY;
				order.currency_ = USD_CURRENCY;
				order.minQty_ = 101; //4
				order.orderQty_ = 111;
				order.leavesQty_ = 121;
				order.cumQty_ = 131;
				order.dayOrderQty_ = 141;
				order.dayCumQty_ = 151;

				{
					subscribers.clear();
					SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
				}
			}
		}
		tick_count e = tick_count::now();
		diff = (e - b).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)instrumentIds.size()/diff<< " event/sec"<< endl;

		{
			MatchedSubscribersT subscribers;

			b = tick_count::now();
			{
				SourceIdT clOrderId, origClOrderID;
				OrderEntry order(g_srcId, g_destId, clOrderId, origClOrderID, instrumentIds[0], g_accountId, g_clearingId, g_execList);
				order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
				order.creationTime_ = 4; 
				order.lastUpdateTime_ = 5;
				order.expireTime_ = 2;
				order.settlDate_ = 3;

				order.price_ = 10.51; //8
				order.stopPx_ = 11.51;
				order.avgPx_ = 12.51;
				order.dayAvgPx_ = 13.51;

				order.status_ = RECEIVEDNEW_ORDSTATUS;
				order.side_ = BUY_SIDE;
				order.ordType_ = LIMIT_ORDERTYPE;
				order.tif_ = GTD_TIF;
				order.settlType_ = _3_SETTLTYPE;
				order.capacity_ = PRINCIPAL_CAPACITY;
				order.currency_ = USD_CURRENCY;
				order.minQty_ = 101; //4
				order.orderQty_ = 111;
				order.leavesQty_ = 121;
				order.cumQty_ = 131;
				order.dayOrderQty_ = 141;
				order.dayCumQty_ = 151;

				SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			}
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (first) latency takes "<< diff<< " sec"<< endl;
		}

		{
			MatchedSubscribersT subscribers;

			b = tick_count::now();
			{
				SourceIdT clOrderId, origClOrderID;
				OrderEntry order(g_srcId, g_destId, clOrderId, origClOrderID, instrumentIds[instrumentIds.size() - 1], g_accountId, g_clearingId, g_execList);
				order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
				order.creationTime_ = 4; 
				order.lastUpdateTime_ = 5;
				order.expireTime_ = 2;
				order.settlDate_ = 3;

				order.price_ = 10.51; //8
				order.stopPx_ = 11.51;
				order.avgPx_ = 12.51;
				order.dayAvgPx_ = 13.51;

				order.status_ = RECEIVEDNEW_ORDSTATUS;
				order.side_ = BUY_SIDE;
				order.ordType_ = LIMIT_ORDERTYPE;
				order.tif_ = GTD_TIF;
				order.settlType_ = _3_SETTLTYPE;
				order.capacity_ = PRINCIPAL_CAPACITY;
				order.currency_ = USD_CURRENCY;
				order.minQty_ = 101; //4
				order.orderQty_ = 111;
				order.leavesQty_ = 121;
				order.cumQty_ = 131;
				order.dayOrderQty_ = 141;
				order.dayCumQty_ = 151;

				SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			}
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (last) latency takes "<< diff<< " sec"<< endl;
		}

		SubscriptionMgr::instance()->removeSubscriptions(handlerId);
	}

	void complexGoodScenario(const std::vector<std::string> &symbols, const std::vector<SourceIdT> &instrumentIds){
		SubscriberIdT handlerId(1, 0);
		size_t subscrCount = 0;

		cout<< "Start complexGoodScenario: one subscription for each symbol (subscr has many filters)"<< endl;
		cout<< "\tPrepare subscriptions..."<< endl;
		tick_count bs = tick_count::now();
		{
			for(size_t i = 0; i < symbols.size(); ++i){
					auto_ptr<OrderFilter> filter(new OrderFilter());
					filter->addFilter(new OrderStatusEqualFilter(RECEIVEDNEW_ORDSTATUS));
					filter->addFilter(new SideEqualFilter(BUY_SIDE));
					filter->addFilter(new OrderTypeEqualFilter(LIMIT_ORDERTYPE));
					TIFInFilter::ValuesT tif;
					tif.insert(GTD_TIF);
					tif.insert(GTC_TIF);
					filter->addFilter(new TIFInFilter(tif));
					filter->addFilter(new SettlTypeEqualFilter(_3_SETTLTYPE));
					filter->addFilter(new CapacityEqualFilter(PRINCIPAL_CAPACITY));
					filter->addFilter(new CurrencyEqualFilter(USD_CURRENCY));
					InstructionsInFilter::ValuesT instr;
					instr.insert(STAY_ON_OFFER_SIDE_INSTRUCTION);
					instr.insert(NOT_HELD_INSTRUCTION);
					filter->addFilter(new InstructionsInFilter(instr));
					filter->addFilter(new DestinationFilter(new StringEqualFilter("NASDAQ")));
					filter->addFilter(new SourceFilter(new StringEqualFilter("CLNT")));
					filter->addFilter(new PxFilter(new PriceGreaterFilter(10.50)));
					filter->addFilter(new StopPxFilter(new PriceGreaterFilter(11.50)));
					filter->addFilter(new AvgPxFilter(new PriceGreaterFilter(12.50)));
					filter->addFilter(new DayAvgPxFilter(new PriceGreaterFilter(13.50)));
					filter->addFilter(new MinQtyFilter(new QuantityGreaterFilter(100)));
					filter->addFilter(new OrderQtyFilter(new QuantityGreaterFilter(110)));
					filter->addFilter(new LeavesQtyFilter(new QuantityGreaterFilter(120)));
					filter->addFilter(new CumQtyFilter(new QuantityGreaterFilter(130)));
					filter->addFilter(new DayOrderQtyFilter(new QuantityGreaterFilter(140)));
					filter->addFilter(new DayCumQtyFilter(new QuantityGreaterFilter(150)));
					filter->addFilter(new ExpireTimeFilter(new DateTimeGreaterFilter(1)));
					filter->addFilter(new SettlDateFilter(new DateTimeGreaterFilter(2)));
					filter->addFilter(new CreationTimeFilter(new DateTimeGreaterFilter(3)));
					filter->addFilter(new LastUpdateTimeFilter(new DateTimeGreaterFilter(4)));
					
					{
						InstrumentFilter instrFlt;
						auto_ptr<StringFilter> eqSymblFlt(new StringEqualFilter(symbols[i]));//
						auto_ptr<InstrumentElementFilter> symblFlt(new InstrumentSymbolFilter(eqSymblFlt.get()));
						eqSymblFlt.release();
						instrFlt.addFilter(symblFlt.release());

						instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
						instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

						filter->addInstrumentFilter(&instrFlt);
					}

					{
						AccountFilter accFlt;
						accFlt.addFilter(new AccountAccountFilter(new StringEqualFilter("ACT")));
						accFlt.addFilter(new AccountFirmFilter(new StringEqualFilter("ACTFirm")));
						accFlt.addFilter(new AccountTypeEqualFilter(PRINCIPAL_ACCOUNTTYPE));
						filter->addAccountFilter(&accFlt);
					}

					{
						ClearingFilter clrFlt;
						clrFlt.addFilter(new ClearingFirmFilter(new StringEqualFilter("CLRFirm")));
						filter->addClearingFilter(&clrFlt);
					}

					SubscriptionMgr::instance()->addSubscription("aaa", filter.release(), handlerId);
					++subscrCount;
				}
		}
		tick_count es = tick_count::now();
		double diff = (es - bs).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)subscrCount/diff<< " subscription/sec"<< endl;
		cout<< "\t"<< subscrCount<< " subscriptions created."<< endl;


		cout<< "\tLocate subscriptions..."<< endl;
		tick_count b = tick_count::now();
		{
			MatchedSubscribersT subscribers;

			for(size_t i = 0; i < instrumentIds.size(); ++i){//
				SourceIdT clOrderId, origClOrderID;
				OrderEntry order(g_srcId, g_destId, clOrderId, origClOrderID, instrumentIds[i], g_accountId, g_clearingId, g_execList);
				order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
				order.creationTime_ = 4; 
				order.lastUpdateTime_ = 5;
				order.expireTime_ = 2;
				order.settlDate_ = 3;

				order.price_ = 10.51; //8
				order.stopPx_ = 11.51;
				order.avgPx_ = 12.51;
				order.dayAvgPx_ = 13.51;

				order.status_ = RECEIVEDNEW_ORDSTATUS;
				order.side_ = BUY_SIDE;
				order.ordType_ = LIMIT_ORDERTYPE;
				order.tif_ = GTD_TIF;
				order.settlType_ = _3_SETTLTYPE;
				order.capacity_ = PRINCIPAL_CAPACITY;
				order.currency_ = USD_CURRENCY;
				order.minQty_ = 101; //4
				order.orderQty_ = 111;
				order.leavesQty_ = 121;
				order.cumQty_ = 131;
				order.dayOrderQty_ = 141;
				order.dayCumQty_ = 151;

				{
					subscribers.clear();
					SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
				}
			}
		}
		tick_count e = tick_count::now();
		diff = (e - b).seconds();
		cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)instrumentIds.size()/diff<< " event/sec"<< endl;

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId, execList;

			b = tick_count::now();
			{
				SourceIdT clOrderId, origClOrderID;
				OrderEntry order(g_srcId, g_destId, clOrderId, origClOrderID, instrumentIds[0], g_accountId, g_clearingId, execList);
				order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
				order.creationTime_ = 4; 
				order.lastUpdateTime_ = 5;
				order.expireTime_ = 2;
				order.settlDate_ = 3;

				order.price_ = 10.51; //8
				order.stopPx_ = 11.51;
				order.avgPx_ = 12.51;
				order.dayAvgPx_ = 13.51;

				order.status_ = RECEIVEDNEW_ORDSTATUS;
				order.side_ = BUY_SIDE;
				order.ordType_ = LIMIT_ORDERTYPE;
				order.tif_ = GTD_TIF;
				order.settlType_ = _3_SETTLTYPE;
				order.capacity_ = PRINCIPAL_CAPACITY;
				order.currency_ = USD_CURRENCY;
				order.minQty_ = 101; //4
				order.orderQty_ = 111;
				order.leavesQty_ = 121;
				order.cumQty_ = 131;
				order.dayOrderQty_ = 141;
				order.dayCumQty_ = 151;
				SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			}
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (first) latency takes "<< diff<< " sec"<< endl;
		}

		{
			MatchedSubscribersT subscribers;
			SourceIdT src, dest, clOrderId, orifClOrderID, accountId, clearingId;

			b = tick_count::now();
			{
				SourceIdT clOrderId, origClOrderID;
				OrderEntry order(g_srcId, g_destId, clOrderId, origClOrderID, instrumentIds[instrumentIds.size() - 1], g_accountId, g_clearingId, g_execList);
				order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
				order.creationTime_ = 4; 
				order.lastUpdateTime_ = 5;
				order.expireTime_ = 2;
				order.settlDate_ = 3;

				order.price_ = 10.51; //8
				order.stopPx_ = 11.51;
				order.avgPx_ = 12.51;
				order.dayAvgPx_ = 13.51;

				order.status_ = RECEIVEDNEW_ORDSTATUS;
				order.side_ = BUY_SIDE;
				order.ordType_ = LIMIT_ORDERTYPE;
				order.tif_ = GTD_TIF;
				order.settlType_ = _3_SETTLTYPE;
				order.capacity_ = PRINCIPAL_CAPACITY;
				order.currency_ = USD_CURRENCY;
				order.minQty_ = 101; //4
				order.orderQty_ = 111;
				order.leavesQty_ = 121;
				order.cumQty_ = 131;
				order.dayOrderQty_ = 141;
				order.dayCumQty_ = 151;

				SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
			}
			e = tick_count::now();
			diff = (e - b).seconds();
			cout<< "\tLocate subscription (last) latency takes "<< diff<< " sec"<< endl;
		}

		SubscriptionMgr::instance()->removeSubscriptions(handlerId);
	}

}

bool testEventBenchmark()
{
	std::vector<std::string> symbols;
	symbols.resize(10000);
	std::vector<SourceIdT> instrumentIds;
	instrumentIds.resize(10000);
	WideDataStorage::create();
	SubscriptionMgr::create();
	{
		g_srcId = WideDataStorage::instance()->add(new StringT("CLNT"));
		g_destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));

		auto_ptr<AccountEntry> account(new AccountEntry());
		account->type_ = PRINCIPAL_ACCOUNTTYPE;
		account->firm_ = "ACTFirm";
		account->account_ = "ACT";
		account->id_ = IdT();
		g_accountId = WideDataStorage::instance()->add(account.release());

		auto_ptr<ClearingEntry> clearing(new ClearingEntry());
		clearing->firm_ = "CLRFirm";
		g_clearingId = WideDataStorage::instance()->add(clearing.release());

		auto_ptr<ExecutionsT> execLst(new ExecutionsT());
		g_execList = WideDataStorage::instance()->add(execLst.release());
	}

	cout<< "\tPrepare instruments..."<< endl;
	{
		size_t pos = 0;
		string symb;
		for(char a = 'A'; a < 'Z'; ++a){
			for(char b = 'A'; b < 'Z'; ++b){
				for(char c = 'A'; c < 'Z'; ++c){
					auto_ptr<InstrumentEntry> instr(new InstrumentEntry());
					symb.clear();
					symb += a;
					symb += b;
					symb += c;
					symbols[pos] = symb;
					instr->symbol_ = symb;
					instr->securityId_ = "AAA";
					instr->securityIdSource_ = "AAASrc";
					instrumentIds[pos] = WideDataStorage::instance()->add(instr.release());
					++pos;
					if(10000 == pos)
						break;
				}			
				if(10000 == pos)
					break;
			}
			if(10000 == pos)
				break;
		}
	}
	simpleGoodScenario(symbols, instrumentIds);
	simpleWorseScenario(symbols, instrumentIds);
	complexGoodScenario(symbols, instrumentIds);
	complexWorseScenario(symbols, instrumentIds);
	SubscriptionMgr::destroy();
	WideDataStorage::destroy();
    return true;
}
