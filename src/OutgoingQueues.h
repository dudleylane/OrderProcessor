/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <variant>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>

#include "QueuesDef.h"

namespace COP{
namespace Queues{

	class OutgoingQueues: public OutQueues
	{
	public:
		OutgoingQueues(void);
		~OutgoingQueues(void);
	public:
		/// reimplementeed from OutQueues
		virtual void push(const ExecReportEvent &evnt, const std::string &target);
		virtual void push(const CancelRejectEvent &evnt, const std::string &target);
		virtual void push(const BusinessRejectEvent &evnt, const std::string &target);

	private:
		/// Unified event type using std::variant for lock-free queue
		using OutEventVariant = std::variant<
			std::monostate,  // Default state for empty initialization
			ExecReportEvent,
			CancelRejectEvent,
			BusinessRejectEvent
		>;

		/// Queued outgoing event with target
		struct QueuedOutEvent {
			std::string target_;
			OutEventVariant event_;

			QueuedOutEvent() = default;
			QueuedOutEvent(const std::string &tgt, OutEventVariant evt)
				: target_(tgt), event_(std::move(evt)) {}
		};

		/// Lock-free concurrent queue for all outgoing events (MPSC pattern)
		oneapi::tbb::concurrent_queue<QueuedOutEvent> eventQueue_;

	private:
		void clear();
	};

}
}