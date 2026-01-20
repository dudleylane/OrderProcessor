/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <memory>
#include "TransactionDef.h"
#include "StateMachine.h"

namespace COP{

	struct OrderEntry;

namespace Proc{

	class DeferedEventContainer;

	class OrderMatcher
	{
	public:
		OrderMatcher(void);
		~OrderMatcher(void);

		void init(DeferedEventContainer *cont);
		void match(OrderEntry *order, const ACID::Context &ctxt);
	private:
		std::unique_ptr<OrdState::OrderState> stateMachine_;
		DeferedEventContainer *defered_;
	};

}
}