/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <tbb/atomic.h>
#include <tbb/mutex.h>
#include <map>
#include <deque>

#include "QueuesDef.h"

namespace COP{
namespace Queues{

	class IncomingQueues: public InQueues, public InQueuesPublisher, public InQueuesContainer
	{
	public:
		IncomingQueues(void);
		~IncomingQueues(void);
	public:
		/// reimplemented from InQueues
		virtual void push(const std::string &source, const OrderEvent &evnt);
		virtual void push(const std::string &source, const OrderCancelEvent &evnt);
		virtual void push(const std::string &source, const OrderReplaceEvent &evnt);
		virtual void push(const std::string &source, const OrderChangeStateEvent &evnt);
		virtual void push(const std::string &source, const ProcessEvent &evnt);
		virtual void push(const std::string &source, const TimerEvent &evnt);
		virtual InQueuesObserver *attach(InQueuesObserver *obs);
		virtual InQueuesObserver *detach();

	public:
		/// reimplemented from InQueuesPublisher
		virtual InQueueProcessor *attach(InQueueProcessor *obs);
		virtual InQueueProcessor *detachProcessor();

	public:
		/// reimplemented from InQueuesContainer
		virtual u32 size()const;
		virtual bool top(InQueueProcessor *obs);
		virtual bool pop();
		virtual bool pop(InQueueProcessor *obs);

	private:
		tbb::atomic<InQueueProcessor *> processor_;
		tbb::atomic<InQueuesObserver *> observer_;
		

		mutable tbb::mutex lock_;

		typedef std::deque<OrderEvent> OrderQueueT;
		typedef std::deque<OrderCancelEvent> OrderCancelQueueT;
		typedef std::deque<OrderReplaceEvent> OrderReplaceQueueT;
		typedef std::deque<OrderChangeStateEvent> OrderStateQueueT;
		typedef std::deque<ProcessEvent> ProcessQueueT;
		typedef std::deque<TimerEvent> TimerQueueT;

		typedef std::map<std::string, OrderQueueT*> OrderQueuesT;
		typedef std::map<std::string, OrderCancelQueueT*> OrderCancelQueuesT;
		typedef std::map<std::string, OrderReplaceQueueT*> OrderReplaceQueuesT;
		typedef std::map<std::string, OrderStateQueueT*> OrderStateQueuesT;
		typedef std::map<std::string, ProcessQueueT*> ProcessQueuesT;
		typedef std::map<std::string, TimerQueueT*> TimerQueuesT;

		enum QueueType{
			INVALID_QUEUE_TYPE = 0,
			ORDER_QUEUE_TYPE,
			ORDER_CANCEL_QUEUE_TYPE,
			ORDER_REPLACE_QUEUE_TYPE,
			ORDER_STATE_QUEUE_TYPE,
			PROCESS_QUEUE_TYPE,
			TIMER_QUEUE_TYPE,
			TOTAL_QUEUE_TYPE
		};
		struct Element{
			std::string source_;
			QueueType type_;

			Element():source_(), type_(INVALID_QUEUE_TYPE){}
			Element(const std::string &src, QueueType type): source_(src), type_(type){}
		};
		typedef std::deque<Element> ProcessingQueueT;

		OrderQueuesT orders_;
		OrderCancelQueuesT orderCancels_;
		OrderReplaceQueuesT orderReplaces_;
		OrderStateQueuesT orderStates_;
		ProcessQueuesT processes_;
		TimerQueuesT timers_;

		ProcessingQueueT processingQueue_;

	private:
		void clear();
	};

}
}