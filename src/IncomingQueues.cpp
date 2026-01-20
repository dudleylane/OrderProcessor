/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/


#include <cassert>
#include "IncomingQueues.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace std;
using namespace COP;
using namespace COP::Queues;

IncomingQueues::IncomingQueues(void)
	: processor_(nullptr)
	, observer_(nullptr)
	, queueSize_(0)
{
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
	return processor_.exchange(obs);
}

InQueueProcessor *IncomingQueues::detachProcessor()
{
	aux::ExchLogger::instance()->note("IncomingQueues detaching InQueueProcessor.");
	return processor_.exchange(nullptr);
}

InQueuesObserver *IncomingQueues::attach(InQueuesObserver *obs)
{
	aux::ExchLogger::instance()->note("IncomingQueues attaching InQueuesObserver.");
	InQueuesObserver *obsOld = observer_.exchange(obs);
	InQueuesObserver *current = observer_.load();
	if ((nullptr != current) && (queueSize_.load(std::memory_order_acquire) > 0))
		current->onNewEvent();
	return obsOld;
}

InQueuesObserver *IncomingQueues::detach()
{
	aux::ExchLogger::instance()->note("IncomingQueues detaching InQueuesObserver.");
	return observer_.exchange(nullptr);
}

u32 IncomingQueues::size() const
{
	return queueSize_.load(std::memory_order_acquire);
}

bool IncomingQueues::top(InQueueProcessor *obs)
{
	assert(nullptr != obs);

	QueuedEvent event;

	// Check if we have a pending event from a previous top() call
	{
		oneapi::tbb::spin_mutex::scoped_lock lock(pendingLock_);
		if (pendingEvent_.has_value()) {
			event = pendingEvent_.value();
			dispatchEvent(obs, event.source_, event.event_);
			return true;
		}
	}

	// Try to get a new event from the queue
	if (!eventQueue_.try_pop(event)) {
		return false;
	}

	// Store as pending (will be consumed by pop())
	{
		oneapi::tbb::spin_mutex::scoped_lock lock(pendingLock_);
		pendingEvent_ = event;
	}

	dispatchEvent(obs, event.source_, event.event_);
	return true;
}

bool IncomingQueues::pop()
{
	// Check if we have a pending event from top()
	{
		oneapi::tbb::spin_mutex::scoped_lock lock(pendingLock_);
		if (pendingEvent_.has_value()) {
			// Handle OrderEvent memory cleanup
			if (auto *orderEvt = std::get_if<OrderEvent>(&pendingEvent_->event_)) {
				std::unique_ptr<OrderEntry> ord(orderEvt->order_);
			}
			pendingEvent_.reset();
			queueSize_.fetch_sub(1, std::memory_order_release);
			return true;
		}
	}

	// No pending event, try to pop directly from queue
	QueuedEvent event;
	if (!eventQueue_.try_pop(event)) {
		return false;
	}

	// Handle OrderEvent memory cleanup
	if (auto *orderEvt = std::get_if<OrderEvent>(&event.event_)) {
		std::unique_ptr<OrderEntry> ord(orderEvt->order_);
	}

	queueSize_.fetch_sub(1, std::memory_order_release);
	return true;
}

bool IncomingQueues::pop(InQueueProcessor *obs)
{
	assert(nullptr != obs);

	QueuedEvent event;
	bool hasPendingEvent = false;

	// Check if we have a pending event from top()
	{
		oneapi::tbb::spin_mutex::scoped_lock lock(pendingLock_);
		if (pendingEvent_.has_value()) {
			event = std::move(pendingEvent_.value());
			pendingEvent_.reset();
			hasPendingEvent = true;
		}
	}

	// If no pending event, try to get from queue
	if (!hasPendingEvent) {
		if (!eventQueue_.try_pop(event)) {
			return false;
		}
	}

	queueSize_.fetch_sub(1, std::memory_order_release);

	// Handle OrderEvent memory - ensure cleanup after dispatch
	std::unique_ptr<OrderEntry> ordCleanup;
	if (auto *orderEvt = std::get_if<OrderEvent>(&event.event_)) {
		ordCleanup.reset(orderEvt->order_);
	}

	dispatchEvent(obs, event.source_, event.event_);
	return true;
}

void IncomingQueues::dispatchEvent(InQueueProcessor *obs, const std::string &source, const EventVariant &event)
{
	std::visit([obs, &source](auto &&evt) {
		using T = std::decay_t<decltype(evt)>;
		if constexpr (std::is_same_v<T, OrderEvent>) {
			obs->onEvent(source, evt);
		} else if constexpr (std::is_same_v<T, OrderCancelEvent>) {
			obs->onEvent(source, evt);
		} else if constexpr (std::is_same_v<T, OrderReplaceEvent>) {
			obs->onEvent(source, evt);
		} else if constexpr (std::is_same_v<T, OrderChangeStateEvent>) {
			obs->onEvent(source, evt);
		} else if constexpr (std::is_same_v<T, ProcessEvent>) {
			obs->onEvent(source, evt);
		} else if constexpr (std::is_same_v<T, TimerEvent>) {
			obs->onEvent(source, evt);
		}
	}, event);
}

void IncomingQueues::push(const std::string &source, const OrderEvent &evnt)
{
	std::unique_ptr<OrderEntry> ord(evnt.order_);

	eventQueue_.push(QueuedEvent(source, evnt));
	ord.release();
	queueSize_.fetch_add(1, std::memory_order_release);

	InQueuesObserver *obs = observer_.load(std::memory_order_acquire);
	if (nullptr != obs)
		obs->onNewEvent();
}

void IncomingQueues::push(const std::string &source, const OrderCancelEvent &evnt)
{
	eventQueue_.push(QueuedEvent(source, evnt));
	queueSize_.fetch_add(1, std::memory_order_release);

	InQueuesObserver *obs = observer_.load(std::memory_order_acquire);
	if (nullptr != obs)
		obs->onNewEvent();
}

void IncomingQueues::push(const std::string &source, const OrderReplaceEvent &evnt)
{
	eventQueue_.push(QueuedEvent(source, evnt));
	queueSize_.fetch_add(1, std::memory_order_release);

	InQueuesObserver *obs = observer_.load(std::memory_order_acquire);
	if (nullptr != obs)
		obs->onNewEvent();
}

void IncomingQueues::push(const std::string &source, const OrderChangeStateEvent &evnt)
{
	eventQueue_.push(QueuedEvent(source, evnt));
	queueSize_.fetch_add(1, std::memory_order_release);

	InQueuesObserver *obs = observer_.load(std::memory_order_acquire);
	if (nullptr != obs)
		obs->onNewEvent();
}

void IncomingQueues::push(const std::string &source, const ProcessEvent &evnt)
{
	eventQueue_.push(QueuedEvent(source, evnt));
	queueSize_.fetch_add(1, std::memory_order_release);

	InQueuesObserver *obs = observer_.load(std::memory_order_acquire);
	if (nullptr != obs)
		obs->onNewEvent();
}

void IncomingQueues::push(const std::string &source, const TimerEvent &evnt)
{
	eventQueue_.push(QueuedEvent(source, evnt));
	queueSize_.fetch_add(1, std::memory_order_release);

	InQueuesObserver *obs = observer_.load(std::memory_order_acquire);
	if (nullptr != obs)
		obs->onNewEvent();
}

void IncomingQueues::clear()
{
	aux::ExchLogger::instance()->debug("IncomingQueues start clear");

	// Clear pending event
	{
		oneapi::tbb::spin_mutex::scoped_lock lock(pendingLock_);
		if (pendingEvent_.has_value()) {
			if (auto *orderEvt = std::get_if<OrderEvent>(&pendingEvent_->event_)) {
				std::unique_ptr<OrderEntry> ord(orderEvt->order_);
			}
			pendingEvent_.reset();
		}
	}

	// Drain the queue and cleanup OrderEntry pointers
	QueuedEvent event;
	while (eventQueue_.try_pop(event)) {
		if (auto *orderEvt = std::get_if<OrderEvent>(&event.event_)) {
			std::unique_ptr<OrderEntry> ord(orderEvt->order_);
		}
	}

	queueSize_.store(0, std::memory_order_release);

	aux::ExchLogger::instance()->debug("IncomingQueues finish clear");
}
