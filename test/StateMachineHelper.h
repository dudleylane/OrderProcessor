/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "OrderStateEvents.h"

namespace test{
	struct OrderStateImpl;
	class OrderStateWrapper{
	public:
		OrderStateWrapper();
		~OrderStateWrapper();

		void start();
		void processEvent(const COP::OrdState::onOrderReceived &evnt);
		void processEvent(const COP::OrdState::onRplOrderReceived &evnt);
		void processEvent(const COP::OrdState::onNewOrder &evnt);
		void processEvent(const COP::OrdState::onExternalOrder &evnt);
		void processEvent(const COP::OrdState::onExternalOrderRejected &evnt);
		void processEvent(const COP::OrdState::onRecvRplOrderRejected &evnt);
		void processEvent(const COP::OrdState::onTradeExecution &evnt);
		void processEvent(const COP::OrdState::onRplOrderRejected &evnt);
		void processEvent(const COP::OrdState::onTradeCrctCncl &evnt);
		void processEvent(const COP::OrdState::onExpired &evnt);
		void processEvent(const COP::OrdState::onRplOrderExpired &evnt);
		void processEvent(const COP::OrdState::onCancelReceived &evnt);
		void processEvent(const COP::OrdState::onCanceled &evnt);
		void processEvent(const COP::OrdState::onRecvOrderRejected &evnt);
		void processEvent(const COP::OrdState::onOrderRejected &evnt);
		void processEvent(const COP::OrdState::onReplaceRejected &evnt);
		void processEvent(const COP::OrdState::onReplacedRejected &evnt);
		void processEvent(const COP::OrdState::onCancelRejected &evnt);
		void processEvent(const COP::OrdState::onReplaceReceived &evnt);
		void processEvent(const COP::OrdState::onReplace &evnt);
		void processEvent(const COP::OrdState::onFinished &evnt);
		void processEvent(const COP::OrdState::onExecCancel &evnt);
		void processEvent(const COP::OrdState::onNewDay &evnt);
		void processEvent(const COP::OrdState::onContinue &evnt);
		void processEvent(const COP::OrdState::onSuspended &evnt);
		void processEvent(const COP::OrdState::onExecReplace &evnt);

		void checkStates(const std::string &fst, const std::string &scnd)const;
	private:
		OrderStateImpl *stateImpl_;
	};
}