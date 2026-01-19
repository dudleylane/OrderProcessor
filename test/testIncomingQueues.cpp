/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include "DataModelDef.h"
#include "IncomingQueues.h"
#include <iostream>

using namespace std;
using namespace COP;
using namespace COP::Queues;
using namespace test;

namespace {
	class TestInQueueObserver: public InQueueProcessor{
	public:
		virtual bool process(){ return false;}
		virtual void onEvent(const std::string &source, const OrderEvent &evnt){
			orders_.push_back(OrderQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const OrderCancelEvent &evnt){
			orderCancels_.push_back(OrderCancelQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const OrderReplaceEvent &evnt){
			orderReplaces_.push_back(OrderReplaceQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const OrderChangeStateEvent &evnt){
			orderStates_.push_back(OrderStateQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const ProcessEvent &evnt){
			processes_.push_back(ProcessQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const TimerEvent &evnt){
			timers_.push_back(TimerQueueT::value_type(source, evnt));
		}
	public:
		typedef std::deque<std::pair<std::string, OrderEvent> > OrderQueueT;
		typedef std::deque<std::pair<std::string, OrderCancelEvent> > OrderCancelQueueT;
		typedef std::deque<std::pair<std::string, OrderReplaceEvent> > OrderReplaceQueueT;
		typedef std::deque<std::pair<std::string, OrderChangeStateEvent> > OrderStateQueueT;
		typedef std::deque<std::pair<std::string, ProcessEvent> > ProcessQueueT;
		typedef std::deque<std::pair<std::string, TimerEvent> > TimerQueueT;

		OrderQueueT orders_;
		OrderCancelQueueT orderCancels_;
		OrderReplaceQueueT orderReplaces_;
		OrderStateQueueT orderStates_;
		ProcessQueueT processes_;
		TimerQueueT timers_;
	};


}

bool testIncomingQueues()
{
	{
		TestInQueueObserver obs;
		IncomingQueues queues;
		check(0 == queues.size());
		check(!queues.top(&obs));
		queues.pop();
	}
	{
		TestInQueueObserver obs;
		IncomingQueues queues;
		OrderEvent ord1; ord1.id_ = IdT(1, 1);
		OrderEvent ord2; ord2.id_ = IdT(2, 1);
		OrderCancelEvent cncl1; cncl1.id_ = IdT(1, 2);
		OrderCancelEvent cncl2; cncl2.id_ = IdT(2, 2);
		OrderReplaceEvent rpl1; rpl1.id_ = IdT(1, 3);
		OrderReplaceEvent rpl2; rpl2.id_ = IdT(2, 3);
		OrderChangeStateEvent st1; st1.id_ = IdT(1, 4);
		OrderChangeStateEvent st2; st2.id_ = IdT(2, 4);
		ProcessEvent pr1; pr1.id_ = IdT(1, 5);
		ProcessEvent pr2; pr2.id_ = IdT(2, 5);
		TimerEvent tr1; tr1.id_ = IdT(1, 6);
		TimerEvent tr2; tr2.id_ = IdT(2, 6);

		check(!queues.top(&obs));
		check(0 == queues.size());
		queues.push("1", ord1);
		check(1 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("2", cncl1);
		check(2 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("3", rpl1);
		check(3 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("4", st1);
		check(4 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("5", pr1);
		check(5 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("6", tr1);
		check(6 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("11", ord2);
		check(7 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("12", cncl2);
		check(8 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("13", rpl2);
		check(9 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("14", st2);
		check(10 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("15", pr2);
		check(11 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.push("16", tr2);
		check(12 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("1" == obs.orders_.front().first);
		check(IdT(1, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();

		queues.pop();

		check(11 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orderCancels_.size());
		check("2" == obs.orderCancels_.front().first);
		check(IdT(1, 2) == obs.orderCancels_.front().second.id_);
		obs.orderCancels_.clear();
		queues.pop();

		check(10 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orderReplaces_.size());
		check("3" == obs.orderReplaces_.front().first);
		check(IdT(1, 3) == obs.orderReplaces_.front().second.id_);
		obs.orderReplaces_.clear();
		queues.pop();

		check(9 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orderStates_.size());
		check("4" == obs.orderStates_.front().first);
		check(IdT(1, 4) == obs.orderStates_.front().second.id_);
		obs.orderStates_.clear();
		queues.pop();

		check(8 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.processes_.size());
		check("5" == obs.processes_.front().first);
		check(IdT(1, 5) == obs.processes_.front().second.id_);
		obs.processes_.clear();
		queues.pop();

		check(7 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.timers_.size());
		check("6" == obs.timers_.front().first);
		check(IdT(1, 6) == obs.timers_.front().second.id_);
		obs.timers_.clear();
		queues.pop();

		check(6 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orders_.size());
		check("11" == obs.orders_.front().first);
		check(IdT(2, 1) == obs.orders_.front().second.id_);
		obs.orders_.clear();
		queues.pop();

		check(5 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orderCancels_.size());
		check("12" == obs.orderCancels_.front().first);
		check(IdT(2, 2) == obs.orderCancels_.front().second.id_);
		obs.orderCancels_.clear();
		queues.pop();

		check(4 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orderReplaces_.size());
		check("13" == obs.orderReplaces_.front().first);
		check(IdT(2, 3) == obs.orderReplaces_.front().second.id_);
		obs.orderReplaces_.clear();
		queues.pop();

		check(3 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.orderStates_.size());
		check("14" == obs.orderStates_.front().first);
		check(IdT(2, 4) == obs.orderStates_.front().second.id_);
		obs.orderStates_.clear();
		queues.pop();

		check(2 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.processes_.size());
		check("15" == obs.processes_.front().first);
		check(IdT(2, 5) == obs.processes_.front().second.id_);
		obs.processes_.clear();
		queues.pop();

		check(1 == queues.size());
		check(queues.top(&obs));
		check(1 == obs.timers_.size());
		check("16" == obs.timers_.front().first);
		check(IdT(2, 6) == obs.timers_.front().second.id_);
		obs.timers_.clear();
		queues.pop();

		check(0 == queues.size());
	}
	return true;
}