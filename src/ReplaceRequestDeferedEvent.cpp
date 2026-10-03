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
using namespace COP::OrdState;

ReplaceRequestDeferedEvent::ReplaceRequestDeferedEvent(OrderEntry *original, std::unique_ptr<OrderEntry> replacement,
                                                       const std::string &requester)
    : original_(original), replacement_(std::move(replacement)), requester_(requester)
{
    assert(nullptr != original_);
    assert(nullptr != replacement_);
}

ReplaceRequestDeferedEvent::~ReplaceRequestDeferedEvent() {}

void ReplaceRequestDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope *scope)
{
    assert(nullptr != func);
    assert(nullptr != original_);
    assert(nullptr != replacement_);

    onReplace evnt4Proc;
    evnt4Proc.transaction_ = scope;
    evnt4Proc.origOrderId_ = original_->orderId_;

    func->process(evnt4Proc, original_, *replacement_, requester_, cnxt);
}
