/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <deque>

namespace COP{ 
	struct OrderEntry;	

namespace EventMgr{

struct NewOrderEvent{
	OrderEntry *order_;
};

struct CancelOrderEvent{};
struct ReplaceOrderEvent{};
struct ChangedOrderEvent{};

struct ExecutionEvent{};
struct TradeCancelExecEvent{};
struct ReplaceExecEvent{};
struct RejectExecEvent{};
struct TradeCorrectExecEvent{};
struct TradeExecEvent{};
struct ChangedExecEvent{};

struct MarketDataEvent{};

struct TimerEvent{};

struct HaltEvent{};
struct AlertEvent{};

struct TransactionEvent{};
struct InvalidTransactEvent{};
struct FailedTransactEvent{};
struct CanceledTransactEvent{};

struct NoSubscribersEvent{};

class EventDispatcher{
public:
	virtual ~EventDispatcher(){}
	virtual void dispatch(const NewOrderEvent &evnt)const = 0;
};

}}