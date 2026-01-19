/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include "DeferedEvents.h"
#include "Processor.h"
#include "TransactionDef.h"
#include "DataModelDef.h"
#include "OrderStorage.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

CancelOrderDeferedEvent::CancelOrderDeferedEvent(): order_(NULL)
{}

CancelOrderDeferedEvent::CancelOrderDeferedEvent(OrderEntry *ord): order_(ord)
{
	assert(NULL != order_);
}

void CancelOrderDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
	//aux::ExchLogger::instance()->debug("CancelOrderDeferedEvent: Start execution of trades.");

	assert(NULL != func);
	assert(NULL != order_);

	onInternalCancel evnt4Proc;
	evnt4Proc.transaction_ = scope;

	func->process(evnt4Proc, order_, cnxt);

	//aux::ExchLogger::instance()->debug("CancelOrderDeferedEvent: Finish execution of trades.");
}

