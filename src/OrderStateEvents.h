/****************************************************************************************
*
*                             Exchange v.2
*                   Copyright(c) 2009 Sergey Mikhailik
*
****************************************************************************************/

#pragma once

#include <string>
#include "TypesDef.h"

namespace COP{
	struct OrderEntry;
	class IdTValueGenerator;
	struct TradeExecEntry;
	struct ExecCorrectExecEntry;
	class OrderBook;

	namespace Store{
		class OrderDataStorage;
	}

	namespace ACID{
		class Scope;
	}
namespace OrdState{

	struct OrderStateEvent{
		IdTValueGenerator *generator_;
		Store::OrderDataStorage *orderStorage_;
		ACID::Scope *transaction_;
		IdT orderId_;

		// for the statemachine purpose only 
		mutable OrderEntry *order4StateMachine_;
		bool testStateMachine_;
		bool testStateMachineCheckResult_;

		OrderBook *orderBook_;

		OrderStateEvent():
			generator_(nullptr), orderStorage_(nullptr), transaction_(nullptr), testStateMachine_(false), 
			testStateMachineCheckResult_(true), orderId_(), order4StateMachine_(nullptr), orderBook_(nullptr){}

		OrderStateEvent(IdTValueGenerator *generator, 
						Store::OrderDataStorage *orderStorage,
						const IdT &orderId,
						OrderBook *orderBook,
						bool testStateMachine,
						bool testStateMachineCheckResult):
			generator_(generator), orderStorage_(orderStorage), orderId_(orderId), 
			testStateMachine_(testStateMachine), testStateMachineCheckResult_(testStateMachineCheckResult),
			order4StateMachine_(nullptr), transaction_(nullptr), orderBook_(orderBook){}

		OrderStateEvent(const OrderStateEvent &evnt):
			generator_(evnt.generator_), orderStorage_(evnt.orderStorage_), orderId_(evnt.orderId_), 
			testStateMachine_(evnt.testStateMachine_), testStateMachineCheckResult_(evnt.testStateMachineCheckResult_),
			order4StateMachine_(nullptr), transaction_(evnt.transaction_), orderBook_(evnt.orderBook_){}
	};

	struct onOrderReceived: public OrderStateEvent
	{
		OrderEntry *order_;	

		onOrderReceived(): order_(nullptr)
		{}
		explicit onOrderReceived(OrderEntry *order): order_(order)
		{}
	};
	struct onRplOrderReceived: public OrderStateEvent
	{
		OrderEntry *order_;	

		onRplOrderReceived(): order_(nullptr)
		{}
		explicit onRplOrderReceived(OrderEntry *order): order_(order)
		{}	
	};

	struct onNewOrder: public OrderStateEvent
	{};
	struct onExternalOrder: public OrderStateEvent
	{
		onExternalOrder(): order_(nullptr)
		{}
		explicit onExternalOrder(OrderEntry *order): order_(order)
		{}

		OrderEntry *order_;	
	};
	struct onExternalOrderRejected: public OrderStateEvent
	{
		onExternalOrderRejected(): order_(nullptr)
		{}
		explicit onExternalOrderRejected(OrderEntry *order): order_(order)
		{}
		onExternalOrderRejected(const OrderStateEvent &evnt, OrderEntry *order, const std::string &reason): 
			OrderStateEvent(evnt), order_(order), reason_(reason)
		{}			

		OrderEntry *order_;	
		std::string reason_;
	};

	struct onRecvRplOrderRejected: public OrderStateEvent
	{
		onRecvRplOrderRejected(): order_(nullptr)
		{}
		explicit onRecvRplOrderRejected(OrderEntry *order): order_(order)
		{}			
		onRecvRplOrderRejected(const OrderStateEvent &evnt, OrderEntry *order, const std::string &reason): 
			OrderStateEvent(evnt), order_(order), reason_(reason)
		{}			

		OrderEntry *order_;	

		std::string reason_;
	};
	struct onRplOrderRejected: public OrderStateEvent
	{
		onRplOrderRejected()
		{}
		onRplOrderRejected(const OrderStateEvent &evnt, const std::string &reason): 
			OrderStateEvent(evnt), reason_(reason)
		{}			

		std::string reason_;	
	};
	struct onTradeExecution: public OrderStateEvent
	{
	public:
		explicit onTradeExecution(const TradeExecEntry *trade):trade_(trade)
		{}
		const TradeExecEntry *trade()const{return trade_;}
	private:
		const TradeExecEntry *trade_;
	};
	struct onTradeCrctCncl: public OrderStateEvent
	{
	public:
		explicit onTradeCrctCncl(const ExecCorrectExecEntry *correct):correct_(correct)
		{}
		const ExecCorrectExecEntry *correct()const{return correct_;}
	private:
		const ExecCorrectExecEntry *correct_;	
	};
	struct onExpired: public OrderStateEvent
	{};
	struct onRplOrderExpired: public OrderStateEvent
	{};
	struct onCancelReceived: public OrderStateEvent
	{};
	struct onCanceled: public OrderStateEvent
	{};
	struct onOrderRejected: public OrderStateEvent
	{
		onOrderRejected(){}
		explicit onOrderRejected(const OrderStateEvent &evnt): 
			OrderStateEvent(evnt)
		{}
		onOrderRejected(const OrderStateEvent &evnt, const std::string &reason): 
			OrderStateEvent(evnt), reason_(reason)
		{}

		std::string reason_;
	};

	struct onRecvOrderRejected: public OrderStateEvent
	{
		onRecvOrderRejected(): order_(nullptr)
		{}
		explicit onRecvOrderRejected(OrderEntry *order): order_(order)
		{}		
		onRecvOrderRejected(const OrderStateEvent &evnt, OrderEntry *order, const std::string &reason): 
			OrderStateEvent(evnt), order_(order), reason_(reason)
		{}

		OrderEntry *order_;	
		std::string reason_;
	};

	struct onReplaceRejected: public OrderStateEvent
	{
	public:
		onReplaceRejected(const IdT &orderId):replId_(orderId){}
	public:
		IdT replId_;		
	};
	struct onReplacedRejected: public OrderStateEvent
	{};
	struct onCancelRejected: public OrderStateEvent
	{};
	struct onReplaceReceived: public OrderStateEvent
	{
	public:
		onReplaceReceived(const IdT &replId):replId_(replId){}
	public:
		IdT replId_;
	};
	struct onReplace: public OrderStateEvent
	{
	public:
		onReplace():origOrderId_(){}
	public:
		IdT origOrderId_;
	};
	struct onFinished: public OrderStateEvent
	{};
	struct onExecCancel: public OrderStateEvent
	{};
	struct onInternalCancel: public OrderStateEvent
	{};
	struct onNewDay: public OrderStateEvent
	{};
	struct onContinue: public OrderStateEvent
	{};
	struct onSuspended: public OrderStateEvent
	{};
	struct onExecReplace: public OrderStateEvent
	{
	public:
		onExecReplace(const IdT &orderId):replId_(orderId){}
	public:
		IdT replId_;	
	};

}
}