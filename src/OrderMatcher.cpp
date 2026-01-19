/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#define NOMINMAX

#include <stdexcept>
#include <cassert>
#include <algorithm>
#include "StateMachine.h"
#include "OrderMatcher.h"
#include "TransactionDef.h"
#include "TransactionScope.h"
#include "DataModelDef.h"
#include "OrderStorage.h"
#include "DeferedEvents.h"
#include "Logger.h"

using namespace std;
using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;
using namespace COP::ACID;
using namespace COP::OrdState;
using namespace COP::Store;

namespace{
	class FindOpositeOrder: public OrderFunctor{
	public:
		FindOpositeOrder(OrderEntry *order, OrderDataStorage *orderStorage): 
					order_(order), orderStorage_(orderStorage)
		{
			assert(nullptr != order_);
			assert(nullptr != orderStorage_);
			if(BUY_SIDE == order_->side_)
				side_ = SELL_SIDE;
			else 
				side_ = BUY_SIDE;
		}
		virtual SourceIdT instrument()const;
		virtual bool match(const IdT &order, bool *stop)const;
	private: 
		OrderEntry *order_;
		OrderDataStorage *orderStorage_;
	};

	SourceIdT FindOpositeOrder::instrument()const{
		return order_->instrument_.getId();
	}

	bool FindOpositeOrder::match(const IdT &order, bool *stop)const{
		OrderEntry *contrOrd = orderStorage_->locateByOrderId(order);
		if(nullptr == contrOrd)
			return false;
		if(0 == contrOrd->leavesQty_)
			return false;
		if(MARKET_ORDERTYPE == order_->ordType_)
			return true;
		if(MARKET_ORDERTYPE == contrOrd->ordType_)
			return true;
		if(BUY_SIDE == order_->side_){
			if(order_->price_ >= contrOrd->price_)
				return true;
			else
				*stop = true;
		}else{
			if(order_->price_ <= contrOrd->price_)
				return true;
			else
				*stop = true;
		}
		assert(nullptr != stop);
		return false;
	}
}

OrderMatcher::OrderMatcher(void): stateMachine_(nullptr), defered_(nullptr)
{
	aux::ExchLogger::instance()->note("OrderMatcher created.");
}

OrderMatcher::~OrderMatcher(void)
{
	delete stateMachine_;
	stateMachine_ = nullptr;
	aux::ExchLogger::instance()->note("OrderMatcher destroyed.");
}

void OrderMatcher::init(DeferedEventContainer *cont)
{
	assert(nullptr != cont);
	defered_ = cont;

	stateMachine_ = new OrderState();
	stateMachine_->start();
	aux::ExchLogger::instance()->note("OrderMatcher initialized.");
}

void OrderMatcher::match(OrderEntry *order, const Context &ctxt)
{
	assert(nullptr != order);
	assert(nullptr != ctxt.orderBook_);
	assert(nullptr != ctxt.orderStorage_);

	//aux::ExchLogger::instance()->debug("OrderMatcher start matching.");

	IdT id = ctxt.orderBook_->find(FindOpositeOrder(order, ctxt.orderStorage_));
	if(!id.isValid()){
		if(MARKET_ORDERTYPE == order->ordType_){
			/// if market not available - market order should be canceled
			aux::ExchLogger::instance()->error("Unable to find oposite order for Market order during order matching.");
			std::unique_ptr<CancelOrderDeferedEvent> defEvnt(new CancelOrderDeferedEvent(order));
			defEvnt->cancelReason_ = "Unable to find oposite order for Market order during order matching.";
			defEvnt->order_ = order;
			defered_->addDeferedEvent(defEvnt.release());
		}else{
			if(aux::ExchLogger::instance()->isNoteOn())
				aux::ExchLogger::instance()->note("Unable to find oposite order during order matching, stop matching.");
		}
		return;
	}

	OrderEntry *contrOrd = ctxt.orderStorage_->locateByOrderId(id);
	if(nullptr == contrOrd)
		throw std::runtime_error("Unable to retrive order from OrderStorage after search!");

	/// add trade event
	std::unique_ptr<ExecutionDeferedEvent> defEvnt(new ExecutionDeferedEvent(order));
	TradeParams trade;
	trade.order_ = contrOrd;
	trade.lastQty_ = std::min(order->leavesQty_, contrOrd->leavesQty_);
	trade.lastPx_ = contrOrd->price_; // ToDo: should be changed for the situation with MarketOrder
	defEvnt->trades_.push_back(trade);
	defered_->addDeferedEvent(defEvnt.release());

	/// add another match event to continue matching
	if(order->leavesQty_ - trade.lastQty_ > 0){
		std::unique_ptr<MatchOrderDeferedEvent> defEvnt(new MatchOrderDeferedEvent(order));
		defEvnt->order_ = order;
		defered_->addDeferedEvent(defEvnt.release());	
	}

	//aux::ExchLogger::instance()->debug("OrderMatcher finish matching.");
}


