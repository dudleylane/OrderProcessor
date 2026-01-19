/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include "OutgoingQueues.h"
#include "Logger.h"

using namespace tbb;
using namespace std;
using namespace COP::Queues;

OutgoingQueues::OutgoingQueues(void)
{
	aux::ExchLogger::instance()->note("OutgoingQueues created.");
}

OutgoingQueues::~OutgoingQueues(void)
{
	clear();
	aux::ExchLogger::instance()->note("OutgoingQueues destroyed.");
}

void OutgoingQueues::clear()
{
	OutQueuesByTargetT tmp;
	{
		mutex::scoped_lock lock(lock_);
		swap(tmp, outQueues_);
	}
	for(OutQueuesByTargetT::iterator it = tmp.begin(); it != tmp.end(); ++it){
		auto_ptr<OutQueues> t(it->second);
	}
	aux::ExchLogger::instance()->note("OutgoingQueues cleared.");
}


void OutgoingQueues::push(const ExecReportEvent &evnt, const std::string &target)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues push new ExecReportEvent.");
	{
		mutex::scoped_lock lock(lock_);
		OutQueuesByTargetT::iterator it = outQueues_.find(target);
		if(outQueues_.end() == it){
			auto_ptr<OutQueues> q(new OutQueues);
			it = outQueues_.insert(OutQueuesByTargetT::value_type(target, q.get())).first;
			q.release();
		}
		assert(NULL != it->second);
		it->second->execReports_.push_back(evnt);
		it->second->order_.push_back(EXECREPORT_OUT_QUEUE_TYPE);
	}
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues pushed new ExecReportEvent.");
}

void OutgoingQueues::push(const CancelRejectEvent &evnt, const std::string &target)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues push new CancelRejectEvent.");
	{
		mutex::scoped_lock lock(lock_);
		OutQueuesByTargetT::iterator it = outQueues_.find(target);
		if(outQueues_.end() == it){
			auto_ptr<OutQueues> q(new OutQueues);
			it = outQueues_.insert(OutQueuesByTargetT::value_type(target, q.get())).first;
			q.release();
		}
		assert(NULL != it->second);
		it->second->cnclRejects_.push_back(evnt);
		it->second->order_.push_back(CANCELREJECT_OUT_QUEUE_TYPE);
	}
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues pushed new CancelRejectEvent.");
}

void OutgoingQueues::push(const BusinessRejectEvent &evnt, const std::string &target)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues push new BusinessRejectEvent.");
	{
		mutex::scoped_lock lock(lock_);
		OutQueuesByTargetT::iterator it = outQueues_.find(target);
		if(outQueues_.end() == it){
			auto_ptr<OutQueues> q(new OutQueues);
			it = outQueues_.insert(OutQueuesByTargetT::value_type(target, q.get())).first;
			q.release();
		}
		assert(NULL != it->second);
		it->second->bsnsRejects_.push_back(evnt);
		it->second->order_.push_back(BUSINESSREJECT_OUT_QUEUE_TYPE);
	}
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues pushed new BusinessRejectEvent.");
}
