/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <vector>
#include "TypesDef.h"
#include "DataModelDef.h"
#include "OrderStateEvents.h"

namespace COP{
	struct OrderEntry;

namespace Queues{

	struct EventBase{
		IdT id_;

		EventBase(){}	
	};

	struct OrderEvent: public EventBase{
		OrderEntry *order_;

		OrderEvent(): order_(nullptr){}
		explicit OrderEvent(OrderEntry *ord): order_(ord){}
	};

	struct OrderCancelEvent: public EventBase{
		
	};

	struct OrderReplaceEvent: public EventBase{
		
	};

	struct OrderChangeStateEvent: public EventBase{
		
	};

	struct ExecReportEvent: public EventBase{
		ExecReportEvent(ExecutionEntry *exec):exec_(exec){}

		ExecutionEntry *exec_;
	};

	struct CancelRejectEvent: public EventBase{
		
	};

	struct BusinessRejectEvent: public EventBase{
		
	};

	struct ProcessEvent: public EventBase{
		enum EventType{
			INVALID = 0,
			ON_REPLACE_RECEVIED,
			ON_ORDER_ACCEPTED,
			ON_EXEC_REPLACE,
			ON_REPLACE_REJECTED,
		};

		EventType type_;
		IdT id_;

		ProcessEvent(): type_(INVALID){}
		ProcessEvent(EventType type): 
			type_(type){}
		ProcessEvent(EventType type, const IdT &id): 
			type_(type), id_(id){}
	};

	struct TimerEvent: public EventBase{
	
	};

	class InQueues{
	public:
		virtual ~InQueues(){}
		virtual void push(const std::string &source, const OrderEvent &evnt) = 0;
		virtual void push(const std::string &source, const OrderCancelEvent &evnt) = 0;
		virtual void push(const std::string &source, const OrderReplaceEvent &evnt) = 0;
		virtual void push(const std::string &source, const OrderChangeStateEvent &evnt) = 0;
		virtual void push(const std::string &source, const ProcessEvent &evnt) = 0;
		virtual void push(const std::string &source, const TimerEvent &evnt) = 0;
	};

	class InQueueProcessor{
	public:
		virtual ~InQueueProcessor(){};
		virtual bool process() = 0;
		virtual void onEvent(const std::string &source, const OrderEvent &evnt) = 0;
		virtual void onEvent(const std::string &source, const OrderCancelEvent &evnt) = 0;
		virtual void onEvent(const std::string &source, const OrderReplaceEvent &evnt) = 0;
		virtual void onEvent(const std::string &source, const OrderChangeStateEvent &evnt) = 0;
		virtual void onEvent(const std::string &source, const ProcessEvent &evnt) = 0;
		virtual void onEvent(const std::string &source, const TimerEvent &evnt) = 0;
	};
	typedef std::vector<InQueueProcessor *> InQueueProcessorsPoolT;

	/// Push interface for the incoming queues
	class InQueuesPublisher{
	public:
		virtual ~InQueuesPublisher(){};
		virtual InQueueProcessor *attach(InQueueProcessor *obs) = 0;
		virtual InQueueProcessor *detachProcessor() = 0;
	};

	/// Push interface for the incoming queues
	class InQueuesObserver{
	public:
		virtual ~InQueuesObserver(){};
		virtual void onNewEvent() = 0;
	};

	/// Pop interface for the incoming queues
	class InQueuesContainer{
	public:
		virtual ~InQueuesContainer(){}
		virtual u32 size()const = 0;
		virtual bool top(InQueueProcessor *obs) = 0;
		virtual bool pop() = 0;
		/// pop front and process it by the observer
		virtual bool pop(InQueueProcessor *obs) = 0;

		virtual InQueuesObserver *attach(InQueuesObserver *obs) = 0;
		virtual InQueuesObserver *detach() = 0;
	};

	class OutQueues{
	public:
		virtual ~OutQueues(){}
		virtual void push(const ExecReportEvent &evnt, const std::string &target) = 0;
		virtual void push(const CancelRejectEvent &evnt, const std::string &target) = 0;
		virtual void push(const BusinessRejectEvent &evnt, const std::string &target) = 0;
	};

}
}