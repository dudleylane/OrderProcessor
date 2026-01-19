/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/


#include <cassert>
#include "IncomingQueues.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace std;
using namespace COP;
using namespace tbb;
using namespace COP::Queues;

IncomingQueues::IncomingQueues(void)
{
	processor_.fetch_and_store(NULL);
	observer_.fetch_and_store(NULL);

	aux::ExchLogger::instance()->note("IncomingQueues created.");	
}

IncomingQueues::~IncomingQueues(void)
{
	clear();
	aux::ExchLogger::instance()->note("IncomingQueues destroyed.");	
}

InQueueProcessor *IncomingQueues::attach(InQueueProcessor *obs)
{
	aux::ExchLogger::instance()->note("IncomingQueues attaching InQueueProcessor.");	
	return processor_.fetch_and_store(obs);}

InQueueProcessor *IncomingQueues::detachProcessor()
{
	aux::ExchLogger::instance()->note("IncomingQueues detaching InQueueProcessor.");	
	return processor_.fetch_and_store(NULL);
}

InQueuesObserver *IncomingQueues::attach(InQueuesObserver *obs)
{
	aux::ExchLogger::instance()->note("IncomingQueues attaching InQueuesObserver.");
	InQueuesObserver *obsOld = observer_.fetch_and_store(obs);
	if((NULL != observer_)&&(!processingQueue_.empty()))
		observer_->onNewEvent();
	return obsOld;

}

InQueuesObserver *IncomingQueues::detach()
{
	aux::ExchLogger::instance()->note("IncomingQueues detaching InQueuesObserver.");
	return observer_.fetch_and_store(NULL);
}

u32 IncomingQueues::size()const
{
	mutex::scoped_lock lock(lock_);
	return static_cast<u32>(processingQueue_.size());
}

bool IncomingQueues::top(InQueueProcessor *obs)
{
	assert(NULL != obs);

	//aux::ExchLogger::instance()->debug("IncomingQueues::top(InQueueProcessor ) start execution");

	Element elem;
	OrderEvent order;
	OrderCancelEvent cancel;
	OrderReplaceEvent replace;
	OrderChangeStateEvent state;
	ProcessEvent process;
	TimerEvent timer;
	auto_ptr<OrderEntry> ord;
	{
		mutex::scoped_lock lock(lock_);
		if(0 == processingQueue_.size())
			return false;
		elem = processingQueue_.front();
		switch(elem.type_){
		case ORDER_QUEUE_TYPE:
			{
				OrderQueuesT::const_iterator it = orders_.find(elem.source_);
				if((orders_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Order element!");
				order = it->second->front();
				ord.reset(order.order_);
			}
			break;
		case ORDER_CANCEL_QUEUE_TYPE:
			{
				OrderCancelQueuesT::const_iterator it = orderCancels_.find(elem.source_);
				if((orderCancels_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderCancel element!");
				cancel = it->second->front();
			}
			break;
		case ORDER_REPLACE_QUEUE_TYPE:
			{
				OrderReplaceQueuesT::const_iterator it = orderReplaces_.find(elem.source_);
				if((orderReplaces_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderReplace element!");
				replace = it->second->front();
			}
			break;
		case ORDER_STATE_QUEUE_TYPE:
			{
				OrderStateQueuesT::const_iterator it = orderStates_.find(elem.source_);
				if((orderStates_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderState element!");
				state = it->second->front();
			}
			break;
		case PROCESS_QUEUE_TYPE:
			{
				ProcessQueuesT::const_iterator it = processes_.find(elem.source_);
				if((processes_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Process element!");
				process = it->second->front();
			}
			break;
		case TIMER_QUEUE_TYPE:
			{
				TimerQueuesT::const_iterator it = timers_.find(elem.source_);
				if((timers_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Timer element!");
				timer = it->second->front();
			}
			break;
		default:
			throw std::exception("Invalid structure: Unknown type of the element!");
		};
	}

	switch(elem.type_){
	case ORDER_QUEUE_TYPE:
		obs->onEvent(elem.source_, order);
		break;
	case ORDER_CANCEL_QUEUE_TYPE:
		obs->onEvent(elem.source_, cancel);
		break;
	case ORDER_REPLACE_QUEUE_TYPE:
		obs->onEvent(elem.source_, replace);
		break;
	case ORDER_STATE_QUEUE_TYPE:
		obs->onEvent(elem.source_, state);
		break;
	case PROCESS_QUEUE_TYPE:
		obs->onEvent(elem.source_, process);
		break;
	case TIMER_QUEUE_TYPE:
		obs->onEvent(elem.source_, timer);
		break;
	};

	//aux::ExchLogger::instance()->debug("IncomingQueues::top(InQueueProcessor ) finish execution");
	return true;
}

bool IncomingQueues::pop()
{
	//aux::ExchLogger::instance()->debug("IncomingQueues::pop start execution");
	{
		mutex::scoped_lock lock(lock_);
		if(0 == processingQueue_.size())
			return false;
		Element elem = processingQueue_.front();
		processingQueue_.pop_front();
		switch(elem.type_){
		case ORDER_QUEUE_TYPE:
			{
				OrderQueuesT::iterator it = orders_.find(elem.source_);
				if((orders_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Order element!");
				it->second->pop_front();
			}
			break;
		case ORDER_CANCEL_QUEUE_TYPE:
			{
				OrderCancelQueuesT::iterator it = orderCancels_.find(elem.source_);
				if((orderCancels_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderCancel element!");
				it->second->pop_front();
			}
			break;
		case ORDER_REPLACE_QUEUE_TYPE:
			{
				OrderReplaceQueuesT::iterator it = orderReplaces_.find(elem.source_);
				if((orderReplaces_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderReplace element!");
				it->second->pop_front();
			}
			break;
		case ORDER_STATE_QUEUE_TYPE:
			{
				OrderStateQueuesT::iterator it = orderStates_.find(elem.source_);
				if((orderStates_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderState element!");
				it->second->pop_front();
			}
			break;
		case PROCESS_QUEUE_TYPE:
			{
				ProcessQueuesT::iterator it = processes_.find(elem.source_);
				if((processes_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Process element!");
				it->second->pop_front();
			}
			break;
		case TIMER_QUEUE_TYPE:
			{
				TimerQueuesT::iterator it = timers_.find(elem.source_);
				if((timers_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Timer element!");
				it->second->pop_front();
			}
			break;
		default:
			throw std::exception("Invalid structure: Unknown type of the element!");
		};
	}
	//aux::ExchLogger::instance()->debug("IncomingQueues::pop finish execution");
	return true;
}

bool IncomingQueues::pop(InQueueProcessor *obs)
{
	assert(NULL != obs);
	
	//aux::ExchLogger::instance()->debug("IncomingQueues::pop(InQueueProcessor) start execution");

	Element elem;
	OrderEvent order;
	OrderCancelEvent cancel;
	OrderReplaceEvent replace;
	OrderChangeStateEvent state;
	ProcessEvent process;
	TimerEvent timer;
	auto_ptr<OrderEntry> ord;
	{
		mutex::scoped_lock lock(lock_);
		if(0 == processingQueue_.size())
			return false;
		elem = processingQueue_.front();
		processingQueue_.pop_front();
		switch(elem.type_){
		case ORDER_QUEUE_TYPE:
			{
				OrderQueuesT::const_iterator it = orders_.find(elem.source_);
				if((orders_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Order element!");
				order = it->second->front();
				ord.reset(order.order_);
				it->second->pop_front();
			}
			break;
		case ORDER_CANCEL_QUEUE_TYPE:
			{
				OrderCancelQueuesT::const_iterator it = orderCancels_.find(elem.source_);
				if((orderCancels_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderCancel element!");
				cancel = it->second->front();
				it->second->pop_front();
			}
			break;
		case ORDER_REPLACE_QUEUE_TYPE:
			{
				OrderReplaceQueuesT::const_iterator it = orderReplaces_.find(elem.source_);
				if((orderReplaces_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderReplace element!");
				replace = it->second->front();
				it->second->pop_front();
			}
			break;
		case ORDER_STATE_QUEUE_TYPE:
			{
				OrderStateQueuesT::const_iterator it = orderStates_.find(elem.source_);
				if((orderStates_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate OrderState element!");
				state = it->second->front();
				it->second->pop_front();
			}
			break;
		case PROCESS_QUEUE_TYPE:
			{
				ProcessQueuesT::const_iterator it = processes_.find(elem.source_);
				if((processes_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Process element!");
				process = it->second->front();
				it->second->pop_front();
			}
			break;
		case TIMER_QUEUE_TYPE:
			{
				TimerQueuesT::const_iterator it = timers_.find(elem.source_);
				if((timers_.end() == it)||(NULL == it->second)||(0 == it->second->size()))
					throw std::exception("Invalid structure: Unable to locate Timer element!");
				timer = it->second->front();
				it->second->pop_front();
			}
			break;
		default:
			throw std::exception("Invalid structure: Unknown type of the element!");
		};
	}

	switch(elem.type_){
	case ORDER_QUEUE_TYPE:
		obs->onEvent(elem.source_, order);
		break;
	case ORDER_CANCEL_QUEUE_TYPE:
		obs->onEvent(elem.source_, cancel);
		break;
	case ORDER_REPLACE_QUEUE_TYPE:
		obs->onEvent(elem.source_, replace);
		break;
	case ORDER_STATE_QUEUE_TYPE:
		obs->onEvent(elem.source_, state);
		break;
	case PROCESS_QUEUE_TYPE:
		obs->onEvent(elem.source_, process);
		break;
	case TIMER_QUEUE_TYPE:
		obs->onEvent(elem.source_, timer);
		break;
	};
	//aux::ExchLogger::instance()->debug("IncomingQueues::pop(InQueueProcessor) finish execution");
	return true;
}

void IncomingQueues::push(const std::string &source, const OrderEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("IncomingQueues start push OrderEvent");
	auto_ptr<OrderEntry> ord(evnt.order_);
	{
		mutex::scoped_lock lock(lock_);
		OrderQueuesT::iterator it = orders_.find(source);
		if(orders_.end() == it){
			it = orders_.insert(OrderQueuesT::value_type(source, new OrderQueueT())).first;
		}
		assert(orders_.end() != it);
		assert(NULL != it->second);
		it->second->push_back(evnt);
		ord.release();
		processingQueue_.push_back(Element(source, ORDER_QUEUE_TYPE));
	}	
	{
		if(NULL != observer_)
			observer_->onNewEvent();
	}
	//aux::ExchLogger::instance()->debug("IncomingQueues finish push OrderEvent");
}

void IncomingQueues::push(const std::string &source, const OrderCancelEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("IncomingQueues start push OrderCancelEvent");

	{
		mutex::scoped_lock lock(lock_);
		OrderCancelQueuesT::iterator it = orderCancels_.find(source);
		if(orderCancels_.end() == it){
			it = orderCancels_.insert(OrderCancelQueuesT::value_type(source, new OrderCancelQueueT())).first;
		}
		assert(orderCancels_.end() != it);
		assert(NULL != it->second);
		it->second->push_back(evnt);

		processingQueue_.push_back(Element(source, ORDER_CANCEL_QUEUE_TYPE));
	}
	{
		if(NULL != observer_)
			observer_->onNewEvent();
	}

	//aux::ExchLogger::instance()->debug("IncomingQueues finish push OrderCancelEvent");
}

void IncomingQueues::push(const std::string &source, const OrderReplaceEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("IncomingQueues start push OrderReplaceEvent");

	{
		mutex::scoped_lock lock(lock_);
		OrderReplaceQueuesT::iterator it = orderReplaces_.find(source);
		if(orderReplaces_.end() == it){
			it = orderReplaces_.insert(OrderReplaceQueuesT::value_type(source, new OrderReplaceQueueT())).first;
		}
		assert(orderReplaces_.end() != it);
		assert(NULL != it->second);
		it->second->push_back(evnt);

		processingQueue_.push_back(Element(source, ORDER_REPLACE_QUEUE_TYPE));
	}
	{
		if(NULL != observer_)
			observer_->onNewEvent();
	}

	//aux::ExchLogger::instance()->debug("IncomingQueues finish push OrderReplaceEvent");
}

void IncomingQueues::push(const std::string &source, const OrderChangeStateEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("IncomingQueues start push OrderStateEvent");

	{
		mutex::scoped_lock lock(lock_);
		OrderStateQueuesT::iterator it = orderStates_.find(source);
		if(orderStates_.end() == it){
			it = orderStates_.insert(OrderStateQueuesT::value_type(source, new OrderStateQueueT())).first;
		}
		assert(orderStates_.end() != it);
		assert(NULL != it->second);
		it->second->push_back(evnt);

		processingQueue_.push_back(Element(source, ORDER_STATE_QUEUE_TYPE));
	}
	{
		if(NULL != observer_)
			observer_->onNewEvent();
	}

	//aux::ExchLogger::instance()->debug("IncomingQueues finish push OrderStateEvent");
}

void IncomingQueues::push(const std::string &source, const ProcessEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("IncomingQueues start push ProcessEvent");
	{
		mutex::scoped_lock lock(lock_);
		ProcessQueuesT::iterator it = processes_.find(source);
		if(processes_.end() == it){
			it = processes_.insert(ProcessQueuesT::value_type(source, new ProcessQueueT())).first;
		}
		assert(processes_.end() != it);
		assert(NULL != it->second);
		it->second->push_back(evnt);

		processingQueue_.push_back(Element(source, PROCESS_QUEUE_TYPE));
	}
	{
		if(NULL != observer_)
			observer_->onNewEvent();
	}

	//aux::ExchLogger::instance()->debug("IncomingQueues finish push ProcessEvent");
}

void IncomingQueues::push(const std::string &source, const TimerEvent &evnt)
{
	//aux::ExchLogger::instance()->debug("IncomingQueues start push TimerEvent");

	{
		mutex::scoped_lock lock(lock_);
		TimerQueuesT::iterator it = timers_.find(source);
		if(timers_.end() == it){
			it = timers_.insert(TimerQueuesT::value_type(source, new TimerQueueT())).first;
		}
		assert(timers_.end() != it);
		assert(NULL != it->second);
		it->second->push_back(evnt);

		processingQueue_.push_back(Element(source, TIMER_QUEUE_TYPE));
	}
	{
		if(NULL != observer_)
			observer_->onNewEvent();
	}

	//aux::ExchLogger::instance()->debug("IncomingQueues finish push TimerEvent");
}

void IncomingQueues::clear()
{
	aux::ExchLogger::instance()->debug("IncomingQueues start clear");

	OrderQueuesT ordersTmp;
	OrderCancelQueuesT orderCancelsTmp;
	OrderReplaceQueuesT orderReplacesTmp;
	OrderStateQueuesT orderStatesTmp;
	ProcessQueuesT processesTmp;
	TimerQueuesT timersTmp;
	{
		ProcessingQueueT tmp;
		mutex::scoped_lock lock(lock_);
		swap(tmp, processingQueue_);
		swap(ordersTmp, orders_);
		swap(orderCancelsTmp, orderCancels_);
		swap(orderReplacesTmp, orderReplaces_);
		swap(orderStatesTmp, orderStates_);
		swap(processesTmp, processes_);
		swap(timersTmp, timers_);
	}

	{
		for(OrderQueuesT::iterator it = ordersTmp.begin(); it != ordersTmp.end(); ++it){
			auto_ptr<OrderQueueT> ap(it->second);
			for(OrderQueueT::iterator oIt = ap->begin(); oIt != ap->begin(); ++oIt)
				auto_ptr<OrderEntry> ord(oIt->order_);
		}
	}
	{
		for(OrderCancelQueuesT::iterator it = orderCancelsTmp.begin(); it != orderCancelsTmp.end(); ++it){
			auto_ptr<OrderCancelQueueT> ap(it->second);
		}
	}
	{
		for(OrderReplaceQueuesT::iterator it = orderReplacesTmp.begin(); it != orderReplacesTmp.end(); ++it){
			auto_ptr<OrderReplaceQueueT> ap(it->second);
		}
	}
	{
		for(OrderStateQueuesT::iterator it = orderStatesTmp.begin(); it != orderStatesTmp.end(); ++it){
			auto_ptr<OrderStateQueueT> ap(it->second);
		}
	}
	{
		for(ProcessQueuesT::iterator it = processesTmp.begin(); it != processesTmp.end(); ++it){
			auto_ptr<ProcessQueueT> ap(it->second);
		}
	}
	{
		for(TimerQueuesT::iterator it = timersTmp.begin(); it != timersTmp.end(); ++it){
			auto_ptr<TimerQueueT> ap(it->second);
		}
	}
	aux::ExchLogger::instance()->debug("IncomingQueues finish clear");
}