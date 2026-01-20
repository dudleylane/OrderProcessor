/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "OutgoingQueues.h"
#include "Logger.h"

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
	// Drain the lock-free queue
	QueuedOutEvent event;
	while (eventQueue_.try_pop(event)) {
		// Events don't own heap memory, no cleanup needed
	}
	aux::ExchLogger::instance()->note("OutgoingQueues cleared.");
}

void OutgoingQueues::push(const ExecReportEvent &evnt, const std::string &target)
{
	if (aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues push new ExecReportEvent.");

	// Lock-free push to concurrent queue
	eventQueue_.push(QueuedOutEvent(target, evnt));

	if (aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues pushed new ExecReportEvent.");
}

void OutgoingQueues::push(const CancelRejectEvent &evnt, const std::string &target)
{
	if (aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues push new CancelRejectEvent.");

	// Lock-free push to concurrent queue
	eventQueue_.push(QueuedOutEvent(target, evnt));

	if (aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues pushed new CancelRejectEvent.");
}

void OutgoingQueues::push(const BusinessRejectEvent &evnt, const std::string &target)
{
	if (aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues push new BusinessRejectEvent.");

	// Lock-free push to concurrent queue
	eventQueue_.push(QueuedOutEvent(target, evnt));

	if (aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OutgoingQueues pushed new BusinessRejectEvent.");
}
