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

MatchOrderDeferedEvent::MatchOrderDeferedEvent(): order_(nullptr)
{}

MatchOrderDeferedEvent::MatchOrderDeferedEvent(OrderEntry *ord): order_(ord)
{
	assert(nullptr != order_);
}

void MatchOrderDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope * /*scope*/)
{
	//aux::ExchLogger::instance()->debug("MatchOrderDeferedEvent: Start execution.");

	assert(nullptr != func);
	assert(nullptr != order_);

	// initiate new order matching iteration
	cnxt.orderMatch_->match(order_, cnxt);

	//aux::ExchLogger::instance()->debug("MatchOrderDeferedEvent: Finish execution.");
}

