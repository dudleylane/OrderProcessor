/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "OrderStateMachineImpl.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "DataModelDef.h"
#include "TransactionScope.h"
#include "TrOperations.h"
#include "DataModelDef.h"

using namespace std;
using namespace COP;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::OrdState;

namespace{
	class FindAnyOpositeOrder: public OrderFunctor{
	public:
		FindAnyOpositeOrder(OrderEntry *orderData):orderData_(orderData){
			assert(nullptr != orderData_);
			side_ = (BUY_SIDE == orderData_->side_)?(SELL_SIDE):(BUY_SIDE);
		}
		~FindAnyOpositeOrder(){}

		virtual SourceIdT instrument()const{
			return orderData_->instrument_.getId();
		}
		virtual bool match(const IdT &, bool *stop)const{
			*stop = true;
			return true;
		}
	private:
		OrderEntry *orderData_;
	};

}

void OrderStatePersistence::serialize(std::string &msg)const
{
	char buf[64];
	char *p = buf;
	memcpy(buf, &stateZone1Id_, sizeof(stateZone1Id_)); 
	p += sizeof(stateZone1Id_);
	*p = ':';
	++p;
	memcpy(p, &stateZone2Id_, sizeof(stateZone2Id_));
	p += sizeof(stateZone2Id_);
	msg.append(buf, p - buf);	
}

char *OrderStatePersistence::serialize(char *buf)const
{
	char *p = buf;
	memcpy(buf, &stateZone1Id_, sizeof(stateZone1Id_)); 
	p += sizeof(stateZone1Id_);
	*p = ':';
	++p;
	memcpy(p, &stateZone2Id_, sizeof(stateZone2Id_));
	p += sizeof(stateZone2Id_);
	return p;
}

const char *OrderStatePersistence::restore(const char *buf, size_t size)
{
	const char *p = buf;
	if(size < sizeof(stateZone1Id_) + 1 + sizeof(stateZone2Id_))
		throw std::runtime_error("Unable to restore OrderStatePersistence - buffer size less than expected!");
	memcpy(&stateZone1Id_, p, sizeof(stateZone1Id_));
	p += sizeof(stateZone1Id_);
	if(':' != *p)
		throw std::runtime_error("Unable to restore OrderStatePersistence - ':' missed after stateZone1Id!");
	++p;
	memcpy(&stateZone2Id_, p, sizeof(stateZone2Id_));
	p += sizeof(stateZone2Id_);
	return p;
}

bool OrderStatePersistence::compare(const OrderStatePersistence &val)const
{
	return (stateZone1Id_ == val.stateZone1Id_)&&
		(stateZone2Id_ == val.stateZone2Id_);
}

void OrdStateImpl::processReceive(OrderEntry **orderData, OrdState::onOrderReceived const&evnt)
{
	assert(nullptr != orderData);
	assert(nullptr == *orderData);
	assert(nullptr != evnt.order_);

	std::string reason;
	if(!evnt.order_->isValid(&reason))
		throw std::runtime_error(reason.c_str());

	*orderData = evnt.order_;
	// generate id for the order
	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);
	*orderData = evnt.orderStorage_->save(**orderData, evnt.generator_);
	(*orderData)->status_ = NEW_ORDSTATUS;

	if(MARKET_ORDERTYPE == (*orderData)->ordType_){
		assert(nullptr != evnt.orderBook_);
		if(!evnt.orderBook_->find(FindAnyOpositeOrder((*orderData))).isValid())
			throw std::runtime_error("There is no market for this instrument!");
	}

	std::unique_ptr<Operation> op(new MatchOrderTrOperation((*orderData)));
	evnt.transaction_->addOperation(op);

	/// market order should be matched once - no need to put it into OrderBook
	if(MARKET_ORDERTYPE != (*orderData)->ordType_){
		std::unique_ptr<Operation> op2(new AddToOrderBookTrOperation(*(*orderData), (*orderData)->instrument_.getId()));
		evnt.transaction_->addOperation(op2);
	}

}

void OrdStateImpl::processReceive(OrderEntry **orderData, OrdState::onRplOrderReceived const&evnt)
{
	assert(nullptr != orderData);
	assert(nullptr == *orderData);

	*orderData = evnt.order_;

	// locate original order and fill origOrderId
	OrderEntry *origOrder = evnt.orderStorage_->locateByClOrderId((*orderData)->origClOrderId_.get());
	if(nullptr == origOrder)
		throw std::runtime_error("Unable to locate original order for OrderReplace!");

	// read lock on original order during field access
	oneapi::tbb::spin_rw_mutex::scoped_lock origLock(origOrder->entryMutex_, false);

	// generate id for the order
	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);
	*orderData = evnt.orderStorage_->save(**orderData, evnt.generator_);

	(*orderData)->origOrderId_ = origOrder->orderId_;
	(*orderData)->status_ = PENDINGREPLACE_ORDSTATUS;

	// change original order to onReplaceReceived
	std::unique_ptr<Operation> op(new EnqueueOrderEventTrOperation<onReplaceReceived>(*origOrder, 
												onReplaceReceived((*orderData)->orderId_),
												(*orderData)->instrument_.getId()));
	evnt.transaction_->addOperation(op);

}

void OrdStateImpl::processReject(OrderEntry **orderData, OrdState::onRecvOrderRejected const&evnt)
{
	assert(nullptr != orderData);
	assert((nullptr == *orderData)||(evnt.order_ == *orderData));


	if(nullptr == *orderData)
		*orderData = evnt.order_;
	// generate id for the order
	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);
	try{
		*orderData = evnt.orderStorage_->save(**orderData, evnt.generator_);
	}catch(const std::exception &) //save could fails if ClOrderId already exists, just continue reject it
	{}
}

void OrdStateImpl::processReject(OrderEntry **orderData, OrdState::onRecvRplOrderRejected const&evnt)
{
	assert(nullptr != orderData);
	assert((nullptr == *orderData)||(evnt.order_ == *orderData));


	*orderData = evnt.order_;
	// generate id for the order
	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);
	try{
		*orderData = evnt.orderStorage_->save(**orderData, evnt.generator_);
	}catch(const std::exception &) //save could fails if ClOrderId already exists, just continue reject it
	{}
}

void OrdStateImpl::processAccept(OrderEntry **orderData, OrdState::onExternalOrder const&evnt)
{
	assert(nullptr != orderData);
	assert(nullptr == *orderData);


	*orderData = evnt.order_;
	string reason;
	if(!(*orderData)->isValid(&reason))
		throw std::runtime_error(reason.c_str());

	if(MARKET_ORDERTYPE == (*orderData)->ordType_){
		assert(nullptr != evnt.orderBook_);
		if(!evnt.orderBook_->find(FindAnyOpositeOrder(*orderData)).isValid())
			throw std::runtime_error("There is no market for this instrument!");
	}

	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);
	*orderData = evnt.orderStorage_->save(**orderData, evnt.generator_);

	std::unique_ptr<Operation> op(new MatchOrderTrOperation(evnt.order_));
	evnt.transaction_->addOperation(op);

	/// market order should be matched once - no need to put it into OrderBook
	if(MARKET_ORDERTYPE != (*orderData)->ordType_){
		std::unique_ptr<Operation> op2(new AddToOrderBookTrOperation(*evnt.order_, (*orderData)->instrument_.getId()));
		evnt.transaction_->addOperation(op2);
	}
}

void OrdStateImpl::processReject(OrderEntry **orderData, OrdState::onExternalOrderRejected const&evnt)
{
	assert(nullptr != orderData);
	assert((nullptr == *orderData)||(evnt.order_ == *orderData));


	*orderData = evnt.order_;
	// generate id for the order
	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);
	try{
		*orderData = evnt.orderStorage_->save(**orderData, evnt.generator_);
	}catch(const std::exception &) //save could fails if ClOrderId already exists, just continue reject it
	{}
}

void OrdStateImpl::processReject(OrderEntry *, OrdState::onOrderRejected const&)
{
	// Pend_New->Rejected: nothing to do
}

void OrdStateImpl::processAccept(OrderEntry *orderData, OrdState::onReplace const&evnt)
{
	std::string reason;
	assert(nullptr != orderData);


	if(!orderData->isValid(&reason))
		throw std::runtime_error(reason.c_str());
	if(MARKET_ORDERTYPE == orderData->ordType_){
		assert(nullptr != evnt.orderBook_);
		if(!evnt.orderBook_->find(FindAnyOpositeOrder(orderData)).isValid())
			throw std::runtime_error("There is no market for this instrument!");
	}

	assert(evnt.origOrderId_.isValid());
	assert(nullptr != evnt.orderStorage_);
	OrderEntry *origOrder = evnt.orderStorage_->locateByOrderId(orderData->origOrderId_);
	if(nullptr == origOrder)
		throw std::runtime_error("Unable to locate original order for OrderReplaceAccept!");

	// read lock on original order during field access
	oneapi::tbb::spin_rw_mutex::scoped_lock origLock(origOrder->entryMutex_, false);

	if(!origOrder->isReplaceValid(&reason))
		throw std::runtime_error(reason.c_str());

	std::unique_ptr<Operation> op(new MatchOrderTrOperation(orderData));
	evnt.transaction_->addOperation(op);

	/// market order should be matched once - no need to put it into OrderBook
	if(MARKET_ORDERTYPE != orderData->ordType_){
		std::unique_ptr<Operation> addOp(new AddToOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(addOp);
	}
	// change original order to CnclReplaced
	std::unique_ptr<Operation> execOp(new EnqueueOrderEventTrOperation<onExecReplace>(*origOrder,
												onExecReplace(orderData->orderId_)));
	evnt.transaction_->addOperation(execOp);
}


void OrdStateImpl::processReject(OrderEntry *orderData, OrdState::onRplOrderRejected const&evnt)
{

	OrderEntry *origOrder = evnt.orderStorage_->locateByOrderId(orderData->origOrderId_);
	if(nullptr == origOrder)
		throw std::runtime_error("Unable to locate original order for OrderReplaceReject!");

	// read lock on original order during field access
	oneapi::tbb::spin_rw_mutex::scoped_lock origLock(origOrder->entryMutex_, false);

	// change original order to NoCnlReplace
	std::unique_ptr<Operation> op(new EnqueueOrderEventTrOperation<onReplaceRejected>(*origOrder,
												onReplaceRejected(orderData->orderId_)));
	evnt.transaction_->addOperation(op);
}

void OrdStateImpl::processExpire(OrderEntry *orderData, OrdState::onRplOrderExpired const&evnt)
{

	OrderEntry *origOrder = evnt.orderStorage_->locateByOrderId(orderData->origOrderId_);
	if(nullptr == origOrder)
		throw std::runtime_error("Unable to locate original order for OrderReplaceExpire!");

	// read lock on original order during field access
	oneapi::tbb::spin_rw_mutex::scoped_lock origLock(origOrder->entryMutex_, false);

	// change original order to NoCnlReplace
	std::unique_ptr<Operation> op(new EnqueueOrderEventTrOperation<onReplaceRejected>(*origOrder,
												onReplaceRejected(orderData->orderId_)));
	evnt.transaction_->addOperation(op);
}




void OrdStateImpl::processReject(OrderEntry *, OrdState::onReplaceRejected const &)
{
}
void OrdStateImpl::processReject(OrderEntry *, OrdState::onCancelRejected const &)
{
}

bool OrdStateImpl::processComplete(OrderEntry *orderData, OrdState::onTradeExecution const &evnt)
{
	assert(nullptr != orderData);


	const ExecTradeParams *trade = evnt.trade();
	if(nullptr == trade)
		throw std::runtime_error("Invalid onTradeExecution event - trade is nullptr!");
	if(orderData->leavesQty_ < trade->lastQty_)
		throw std::runtime_error("Unable to execute trade - order's leaves Qty less than traded Qty!");

	return trade->lastQty_ == orderData->leavesQty_;
}

void OrdStateImpl::processFill(OrderEntry *orderData, OrdState::onTradeExecution const&evnt)
{
	//update order according executed trade
	assert(nullptr != orderData);
	assert(nullptr != evnt.generator_);
	assert(nullptr != evnt.orderStorage_);


	const ExecTradeParams *trade = evnt.trade();
	assert(nullptr != trade);

	ExecutionEntry ex;
	ex.type_ = TRADE_EXECTYPE;
	ex.transactTime_ = 1;
	ex.orderId_ = orderData->orderId_;

	TradeExecEntry tradeEntry(ex, *trade);
	tradeEntry.tradeDate_ = 1;
	
	ExecutionEntry *tr = evnt.orderStorage_->save(tradeEntry, evnt.generator_);
	assert(nullptr != tr);

	orderData->applyTrade(static_cast<TradeExecEntry *>(tr));
	if(0 == orderData->leavesQty_){
		assert(nullptr != evnt.orderBook_);
		evnt.orderBook_->remove(*orderData);
		tr->orderStatus_ = FILLED_ORDSTATUS;
	}else
		tr->orderStatus_ = PARTFILL_ORDSTATUS;
}

bool OrdStateImpl::processNotExecuted(OrderEntry *orderData, OrdState::onTradeCrctCncl const &evnt)
{
	assert(nullptr != orderData);


	const ExecCorrectExecEntry *crct = evnt.correct();
	if(nullptr == crct)
		throw std::runtime_error("Invalid onTradeCrctCncl event - correctExecution is nullptr!");

	return crct->leavesQty_ == orderData->orderQty_;
}

void OrdStateImpl::processCorrected(OrderEntry *orderData, OrdState::onTradeCrctCncl const &evnt)
{

	const ExecCorrectExecEntry *crct = evnt.correct();
	if(nullptr == crct)
		throw std::runtime_error("Invalid onTradeCrctCncl event - correctExecution is nullptr!");
	// if order was filled and gone to partiallyFill after correct - restore it in orderBook
	if((0 == orderData->leavesQty_)&&(0 < crct->leavesQty_)){
		std::unique_ptr<Operation> op(new MatchOrderTrOperation(orderData));
		evnt.transaction_->addOperation(op);

		/// market order should be matched once - no need to put it into OrderBook
		if(MARKET_ORDERTYPE != orderData->ordType_){
			std::unique_ptr<Operation> op1(new AddToOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
			evnt.transaction_->addOperation(op1);			
		}
	}
	//update order according corrected trade
	orderData->leavesQty_ = crct->leavesQty_;
	orderData->cumQty_ = crct->cumQty_;
	orderData->currency_ = crct->currency_;
}

void OrdStateImpl::processCorrectedWithoutRestore(OrderEntry *orderData, OrdState::onTradeCrctCncl const &evnt)
{

	const ExecCorrectExecEntry *crct = evnt.correct();
	if(nullptr == crct)
		throw std::runtime_error("Invalid onTradeCrctCncl event - correctExecution is nullptr!");

	//update order according corrected trade
	orderData->leavesQty_ = crct->leavesQty_;
	orderData->cumQty_ = crct->cumQty_;
	orderData->currency_ = crct->currency_;
}

void OrdStateImpl::processRejectNew(OrderEntry *orderData, OrdState::onOrderRejected const&evnt)
{
	assert(nullptr != orderData);
	

	if(MARKET_ORDERTYPE != orderData->ordType_){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}

bool OrdStateImpl::processNotExecuted(OrderEntry *, OrdState::onTradeExecution const &)
{
	return true;
}

void OrdStateImpl::processExpire(OrderEntry *orderData, OrdState::onExpired const&evnt)
{
	// Pend_New->Expired: nothing to do
	assert(nullptr != orderData);


	if((MARKET_ORDERTYPE != orderData->ordType_) && 
		((NEW_ORDSTATUS == orderData->status_)||(PARTFILL_ORDSTATUS == orderData->status_))){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}

void OrdStateImpl::processFinished(OrderEntry *orderData, OrdState::onFinished const&evnt)
{
	assert(nullptr != orderData);
	if((MARKET_ORDERTYPE != orderData->ordType_) && 
	   ((NEW_ORDSTATUS == orderData->status_)||(PARTFILL_ORDSTATUS == orderData->status_))){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}

bool OrdStateImpl::processNotExecuted(OrderEntry *orderData, OrdState::onNewDay const &)
{
	assert(nullptr != orderData);
	return orderData->leavesQty_ == orderData->orderQty_;
}
bool OrdStateImpl::processNotExecuted(OrderEntry *orderData, OrdState::onContinue const &)
{
	assert(nullptr != orderData);
	return orderData->leavesQty_ == orderData->orderQty_;
}
void OrdStateImpl::processRestored(OrderEntry *orderData, OrdState::onNewDay const &evnt)
{
	assert(nullptr != orderData);
	if(0 < orderData->leavesQty_){
		std::unique_ptr<Operation> op(new MatchOrderTrOperation(orderData));
		evnt.transaction_->addOperation(op);

		assert(MARKET_ORDERTYPE != orderData->ordType_);		
		std::unique_ptr<Operation> op1(new AddToOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);			
	}
}

void OrdStateImpl::processSuspended(OrderEntry *orderData, OrdState::onSuspended const&evnt)
{
	assert(nullptr != orderData);

	if((MARKET_ORDERTYPE != orderData->ordType_) && 
	   ((NEW_ORDSTATUS == orderData->status_)||(PARTFILL_ORDSTATUS == orderData->status_))){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}

void OrdStateImpl::processContinued(OrderEntry *orderData, OrdState::onContinue const &evnt)
{
	assert(nullptr != orderData);

	if(0 < orderData->leavesQty_){
		std::unique_ptr<Operation> op(new MatchOrderTrOperation(orderData));
		evnt.transaction_->addOperation(op);

		/// market order should be matched once - no need to put it into OrderBook
		if(MARKET_ORDERTYPE != orderData->ordType_){
			std::unique_ptr<Operation> op1(new AddToOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
			evnt.transaction_->addOperation(op1);			
		}
	}
}

void OrdStateImpl::processCancel(OrderEntry *, OrdState::onCanceled const&)
{
}
void OrdStateImpl::processCanceled(OrderEntry *orderData, OrdState::onExecCancel const&evnt)
{
	assert(nullptr != orderData);

	if((MARKET_ORDERTYPE != orderData->ordType_) && 
	   ((NEW_ORDSTATUS == orderData->status_)||(PARTFILL_ORDSTATUS == orderData->status_))){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}

void OrdStateImpl::processCanceled(OrderEntry *orderData, OrdState::onInternalCancel const&evnt)
{
	assert(nullptr != orderData);

	if((MARKET_ORDERTYPE != orderData->ordType_) && 
	   ((NEW_ORDSTATUS == orderData->status_)||(PARTFILL_ORDSTATUS == orderData->status_))){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}

void OrdStateImpl::processReplaced(OrderEntry *orderData, OrdState::onExecReplace const&evnt)
{
	assert(nullptr != orderData);

	if((MARKET_ORDERTYPE != orderData->ordType_) && 
	   ((NEW_ORDSTATUS == orderData->status_)||(PARTFILL_ORDSTATUS == orderData->status_))){
		std::unique_ptr<Operation> op1(new RemoveFromOrderBookTrOperation(*orderData, orderData->instrument_.getId()));
		evnt.transaction_->addOperation(op1);	
	}
}
void OrdStateImpl::processReceive(OrderEntry *, OrdState::onReplaceReceived const&)
{
}
bool OrdStateImpl::processAcceptable(OrdState::onReplaceReceived const&/*evnt*/)
{
	return true;

/*	assert(nullptr != evnt.order_);
	// check order parameters
	std::string reason;
	bool rez = evnt.order_->isValid(&reason);
	if(!rez)
		evnt.transaction_->setInvalidReason(reason);
	return rez;*/
}

void OrdStateImpl::processReceive(OrderEntry *, OrdState::onCancelReceived const&)
{
}

bool OrdStateImpl::processAcceptable(OrdState::onCancelReceived const&/*evnt*/)
{
	return true;

/*	assert(nullptr != evnt.order_);
	// check order parameters
	std::string reason;
	bool rez = evnt.order_->isValid(&reason);
	if(!rez)
		evnt.transaction_->setInvalidReason(reason);
	return rez;*/
}
