/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <map>
#include <deque>
#include <tbb/mutex.h>

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
		mutable tbb::mutex lock_;

		typedef std::deque<ExecReportEvent> ExecRepQueueT;
		typedef std::deque<CancelRejectEvent> CancelRejectQueueT;
		typedef std::deque<BusinessRejectEvent> BusinessRejectQueueT;
		
		enum OutQueueType{
			INVALID_OUT_QUEUE_TYPE = 0,
			EXECREPORT_OUT_QUEUE_TYPE,
			CANCELREJECT_OUT_QUEUE_TYPE,
			BUSINESSREJECT_OUT_QUEUE_TYPE,
			TOTAL_OUT_QUEUE_TYPE
		};
		typedef std::deque<OutQueueType> EventOrderT;

		struct OutQueues{
			EventOrderT order_;
			ExecRepQueueT execReports_;
			CancelRejectQueueT cnclRejects_;
			BusinessRejectQueueT bsnsRejects_;
		};
		typedef std::map<std::string, OutQueues *> OutQueuesByTargetT;
		OutQueuesByTargetT outQueues_;

	private:
		void clear();
	};

}
}