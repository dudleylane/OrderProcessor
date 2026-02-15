/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "OrderStateEvents.h"

namespace COP{
	struct OrderEntry;

namespace OrdStateImpl{

	void processReceive(OrderEntry **orderData, OrdState::onOrderReceived const&evnt);
	void processReceive(OrderEntry **orderData, OrdState::onRplOrderReceived const&evnt);
	void processReject(OrderEntry **orderData, OrdState::onRecvOrderRejected const&evnt);
	void processReject(OrderEntry **orderData, OrdState::onRecvRplOrderRejected const&evnt);
	void processAccept(OrderEntry **orderData, OrdState::onExternalOrder const&evnt);
	void processReject(OrderEntry **orderData, OrdState::onExternalOrderRejected const&evnt);

	void processReject(OrderEntry *orderData, OrdState::onOrderRejected const&evnt);
	void processRejectNew(OrderEntry *orderData, OrdState::onOrderRejected const&evnt);
	void processReject(OrderEntry *orderData, OrdState::onRplOrderRejected const&evnt);
	void processExpire(OrderEntry *orderData, OrdState::onRplOrderExpired const&evnt);

	void processReject(OrderEntry *orderData, OrdState::onReplaceRejected const &evnt);
	void processReject(OrderEntry *orderData, OrdState::onCancelRejected const &evnt);

	void processAccept(OrderEntry *orderData, OrdState::onReplace const&evnt);

	void processFill(OrderEntry *orderData, OrdState::onTradeExecution const&evnt);
	bool processNotExecuted(OrderEntry *orderData, OrdState::onTradeExecution const &evnt);
	bool processNotExecuted(OrderEntry *orderData, OrdState::onTradeCrctCncl const &evnt);
	bool processNotExecuted(OrderEntry *orderData, OrdState::onNewDay const &evnt);
	bool processNotExecuted(OrderEntry *orderData, OrdState::onContinue const &evnt);
	bool processComplete(OrderEntry *orderData, OrdState::onTradeExecution const &evnt);
	void processCorrected(OrderEntry *orderData, OrdState::onTradeCrctCncl const &evnt);
	void processCorrectedWithoutRestore(OrderEntry *orderData, OrdState::onTradeCrctCncl const &evnt);
	void processExpire(OrderEntry *orderData, OrdState::onExpired const&evnt);
	void processCancel(OrderEntry *orderData, OrdState::onCanceled const&evnt);
	void processRestored(OrderEntry *orderData, OrdState::onNewDay const &evnt);
	void processContinued(OrderEntry *orderData, OrdState::onContinue const &evnt);
	void processFinished(OrderEntry *orderData, OrdState::onFinished const&evnt);
	void processSuspended(OrderEntry *orderData, OrdState::onSuspended const&evnt);
	void processCanceled(OrderEntry *orderData, OrdState::onExecCancel const&evnt);
	void processCanceled(OrderEntry *orderData, OrdState::onInternalCancel const&evnt);
	void processReplaced(OrderEntry *orderData, OrdState::onExecReplace const&evnt);

	// cancel/replace zone
	void processReceive(OrderEntry *orderData, OrdState::onReplaceReceived const&evnt);
	bool processAcceptable(OrdState::onReplaceReceived const&evnt);

	void processReceive(OrderEntry *orderData, OrdState::onCancelReceived const&evnt);
	bool processAcceptable(OrdState::onCancelReceived const&evnt);
	
}}