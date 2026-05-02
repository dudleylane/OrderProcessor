/**
 Concurrent Order Processor library

 FX Swap Support: MatchSwapOrderDeferedEvent

 Distributed under the GNU Affero General Public License (AGPL).
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

MatchSwapOrderDeferedEvent::MatchSwapOrderDeferedEvent() : order_(nullptr) {}

MatchSwapOrderDeferedEvent::MatchSwapOrderDeferedEvent(OrderEntry *ord) : order_(ord)
{
    assert(nullptr != order_);
}

void MatchSwapOrderDeferedEvent::execute(DeferedEventFunctor *func, const Context &cnxt, ACID::Scope * /*scope*/)
{
    assert(nullptr != func);
    assert(nullptr != order_);

    // Delegates to OrderMatcher::match() which dispatches to matchSwap() for FXSWAP orders
    cnxt.orderMatch_->match(order_, cnxt);
}
