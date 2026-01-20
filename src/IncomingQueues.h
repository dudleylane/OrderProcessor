/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <optional>
#include <variant>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/spin_mutex.h>

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
		std::atomic<InQueueProcessor *> processor_;
		std::atomic<InQueuesObserver *> observer_;

		/// Unified event type using std::variant for lock-free queue
		using EventVariant = std::variant<
			OrderEvent,
			OrderCancelEvent,
			OrderReplaceEvent,
			OrderChangeStateEvent,
			ProcessEvent,
			TimerEvent
		>;

		/// Queued event containing source and the event data
		struct QueuedEvent {
			std::string source_;
			EventVariant event_;

			QueuedEvent() = default;
			QueuedEvent(const std::string &src, EventVariant evt)
				: source_(src), event_(std::move(evt)) {}
		};

		/// Lock-free concurrent queue (MPMC safe)
		oneapi::tbb::concurrent_queue<QueuedEvent> eventQueue_;

		/// Atomic size counter for O(1) size() queries
		std::atomic<u32> queueSize_{0};

		/// Pending event for top() without pop() - protected by pendingLock_
		mutable oneapi::tbb::spin_mutex pendingLock_;
		std::optional<QueuedEvent> pendingEvent_;

	private:
		void clear();
		void dispatchEvent(InQueueProcessor *obs, const std::string &source, const EventVariant &event);
	};

}
}