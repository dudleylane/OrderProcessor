/**
 Concurrent Order Processor library

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <cassert>
#include "DeferedEvents.h"
#include "DataModelDef.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::ACID;

StateChangeDeferedEvent::StateChangeDeferedEvent(OrderEntry *ord, OrderStateChange change)
    : order_(ord), change_(change)
{
    assert(nullptr != order_);
}

void StateChangeDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
    assert(nullptr != func);
    assert(nullptr != order_);

    func->process(change_, order_, scope, cnxt);
}
