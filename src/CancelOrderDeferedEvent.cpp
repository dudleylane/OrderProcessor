/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include "DeferedEvents.h"
#include "Processor.h"
#include "TransactionDef.h"
#include "DataModelDef.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

CancelOrderDeferedEvent::CancelOrderDeferedEvent() : order_(nullptr) {}

CancelOrderDeferedEvent::CancelOrderDeferedEvent(OrderEntry *ord) : order_(ord)
{
    assert(nullptr != order_);
}

void CancelOrderDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
    assert(nullptr != func);
    assert(nullptr != order_);

    onInternalCancel evnt4Proc;
    evnt4Proc.transaction_ = scope;

    func->process(evnt4Proc, order_, cnxt);
}

CancelRequestDeferedEvent::CancelRequestDeferedEvent(OrderEntry *ord, const std::string &requester,
                                                     const std::string &requestClOrdId)
    : order_(ord), requester_(requester), requestClOrdId_(requestClOrdId)
{
    assert(nullptr != order_);
}

void CancelRequestDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
    assert(nullptr != func);
    assert(nullptr != order_);

    onExecCancel evnt4Proc;
    evnt4Proc.transaction_ = scope;
    evnt4Proc.requestClOrdId_ = requestClOrdId_;

    func->process(evnt4Proc, order_, requester_, cnxt);
}
