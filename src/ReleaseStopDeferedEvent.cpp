/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include <memory>
#include "DeferedEvents.h"
#include "TrOperations.h"
#include "DataModelDef.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::ACID;

ReleaseStopDeferedEvent::ReleaseStopDeferedEvent(OrderEntry *ord) : order_(ord)
{
    assert(nullptr != order_);
}

void ReleaseStopDeferedEvent::execute(DeferedEventFunctor * /*func*/, const Context & /*cnxt*/, Scope *scope)
{
    assert(nullptr != scope);
    // A stop is in the stop book once the transaction that books it has run, which can be before its creator marks that
    // transaction enqueued: the manager may run it on another worker at once. The release is a transaction about the
    // stop, so it waits for the mark, as any such transaction must, and draws a higher id (#83).
    order_->waitUntilBookingEnqueued();
    // The release decides when its transaction runs, after every earlier one on the order and its instrument (#223)
    std::unique_ptr<Operation> release(new ReleaseStopTrOperation(order_));
    scope->addOperation(release);
    // After the release, not first as Processor::persistOrder() puts it: the release marks the order released when it
    // executes, and the codec reads the order when this executes. The release publishes nothing itself, so nothing
    // reaches a client before the order is durable (#28).
    std::unique_ptr<Operation> persist(new PersistOrderTrOperation(*order_));
    scope->addOperation(persist);
}
