/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "OrderBookImpl.h"
#include "FileStorageDef.h"
#include "Logger.h"

using namespace COP;
using namespace tbb;
using namespace std;
using namespace COP::Store;

namespace {
	enum LogMessages{
		INVALID_MSG = 0,
		ADDORDER_FINAL_MSG,
		ADDORDER_START_MSG,
		REMOVEORDER_FINAL_MSG,
		REMOVEORDER_START_MSG,
		RESTOREORDER_FINAL_MSG,
		RESTOREORDER_START_MSG,

	};

	class OBLogMessage: public aux::LogMessage{
	public:
		OBLogMessage(LogMessages type, const IdT &id): type_(type), id_(id)
		{}

		virtual void prepareMessage(){
			switch(type_){
			case ADDORDER_FINAL_MSG:
				msg.reserve(128);
				msg = "OrderBookImpl Added order '";
				id_.toString(msg);
				msg += "' into OrderBook.";
				break;
			case ADDORDER_START_MSG:
				msg.reserve(128);
				msg = "OrderBookImpl Adding order '";
				id_.toString(msg);
				msg += "' into OrderBook.";
				break;
			case REMOVEORDER_START_MSG:
				msg.reserve(128);
				msg = "OrderBookImpl Removing order '";
				id_.toString(msg);
				msg += "' from OrderBook.";
				break;
			case REMOVEORDER_FINAL_MSG:
				msg.reserve(128);
				msg = "OrderBookImpl Finish removing order '";
				id_.toString(msg);
				msg += "' from OrderBook.";
				break;
			case RESTOREORDER_FINAL_MSG:
				msg.reserve(128);
				msg = "OrderBookImpl restored order '";
				id_.toString(msg);
				msg += "' into OrderBook.";
				break;
			case RESTOREORDER_START_MSG:
				msg.reserve(128);
				msg = "OrderBookImpl restoring order '";
				id_.toString(msg);
				msg += "' into OrderBook.";
				break;
			default:
				assert(false);
				throw std::runtime_error("OBLogMessage used with invalid type!");
			};	
		}
	private:
		OBLogMessage &operator=(const OBLogMessage &);
		LogMessages type_;
		const IdT &id_;
	};

}

OrderBookImpl::OrderBookImpl(void): storage_(NULL)
{
	//aux::ExchLogger::instance()->note("OrderBook created.");
}

OrderBookImpl::~OrderBookImpl(void)
{
	//aux::ExchLogger::instance()->debug("OrderBook destroing.");
	OrderGroupsByInstrumentT tmp;
	swap(tmp, orderGroups_);
	for(OrderGroupsByInstrumentT::const_iterator it = tmp.begin(); it != tmp.end(); ++it){
		delete it->second;
	}
	//aux::ExchLogger::instance()->note("OrderBook destroyed.");
}

void OrderBookImpl::init(const InstrumentsT &instr, OrderSaver *storage)
{
	//aux::ExchLogger::instance()->debug("OrderBook initializing.");
	assert(NULL != storage);
	assert(NULL == storage_);
	storage_ = storage;

	for(InstrumentsT::const_iterator it = instr.begin(); it != instr.end(); ++it){
		auto_ptr<OrdersGroup> grp(new OrdersGroup);
		orderGroups_.insert(OrderGroupsByInstrumentT::value_type(*it, grp.get()));	
		grp.release();
	}
	//aux::ExchLogger::instance()->note("OrderBook initialized.");
}

void OrderBookImpl::add(const OrderEntry& order)
{
	assert(order.orderId_.isValid());
	assert(order.instrument_.getId().isValid());
	{
		//OBLogMessage msg(ADDORDER_START_MSG, order.orderId_);
		//aux::ExchLogger::instance()->debug(msg);
	}
	OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
	if(orderGroups_.end() == it)
		throw std::runtime_error("Unable to add order into book - instrument not registered!");
	if(BUY_SIDE == order.side_){
		mutex::scoped_lock lock(it->second->buyLock_);
		it->second->buyOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));
	}else if(SELL_SIDE == order.side_){
		mutex::scoped_lock lock(it->second->sellLock_);
		it->second->sellOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));		
	}else
		throw std::runtime_error("Unable to add order into book - side is not supported!");

	if(NULL != storage_){
		storage_->save(order);
	}
	if(aux::ExchLogger::instance()->isNoteOn()){
		OBLogMessage msg(ADDORDER_FINAL_MSG, order.orderId_);
		aux::ExchLogger::instance()->note(msg);
	}
}

void OrderBookImpl::restore(const OrderEntry& order)
{
	assert(order.orderId_.isValid());
	assert(order.instrument_.getId().isValid());
	{
		//OBLogMessage msg(RESTOREORDER_START_MSG, order.orderId_);
		//aux::ExchLogger::instance()->debug(msg);
	}
	OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
	if(orderGroups_.end() == it)
		throw std::runtime_error("Unable to restore order into book - instrument not registered!");
	if(BUY_SIDE == order.side_){
		mutex::scoped_lock lock(it->second->buyLock_);
		it->second->buyOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));
	}else if(SELL_SIDE == order.side_){
		mutex::scoped_lock lock(it->second->sellLock_);
		it->second->sellOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));		
	}else
		throw std::runtime_error("Unable to restore order into book - side is not supported!");

	if(aux::ExchLogger::instance()->isNoteOn()){
		OBLogMessage msg(RESTOREORDER_FINAL_MSG, order.orderId_);
		aux::ExchLogger::instance()->note(msg);
	}
}

void OrderBookImpl::remove(const OrderEntry& order)
{
	assert(order.instrument_.getId().isValid());
	if(aux::ExchLogger::instance()->isDebugOn()){
		OBLogMessage msg(REMOVEORDER_START_MSG, order.orderId_);
		aux::ExchLogger::instance()->debug(msg);
	}

	OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
	if(orderGroups_.end() == it)
		throw std::runtime_error("Unable to remove order from book - instrument not registered!");
	bool found = false;
	if(BUY_SIDE == order.side_){
		mutex::scoped_lock lock(it->second->buyLock_);
		OrdersByPriceDescT::iterator oit = it->second->buyOrder_.lower_bound(order.price_);
		while((it->second->buyOrder_.end() != oit)&&(order.price_ == oit->first)){
			found = oit->second == order.orderId_;
			if(found){
				it->second->buyOrder_.erase(oit);
				break;
			}
			++oit;
		}
		if(!found)
			throw std::runtime_error("Unable to remove order from book - order not found!");
	}else if(SELL_SIDE == order.side_){
		mutex::scoped_lock lock(it->second->sellLock_);
		OrdersByPriceAscT::iterator oit = it->second->sellOrder_.lower_bound(order.price_);
		while((it->second->sellOrder_.end() != oit)&&(order.price_ == oit->first)){
			found = oit->second == order.orderId_;
			if(found){
				it->second->sellOrder_.erase(oit);
				break;
			}
			++oit;
		}
		if(!found)
			throw std::runtime_error("Unable to remove order from book - order not found!");
	}else
		throw std::runtime_error("Unable to remove order into book - side is not supported!");

	if(aux::ExchLogger::instance()->isNoteOn()){
		OBLogMessage msg(REMOVEORDER_FINAL_MSG, order.orderId_);
		aux::ExchLogger::instance()->note(msg);
	}
}

IdT OrderBookImpl::find(const OrderFunctor &functor)const
{
	//aux::ExchLogger::instance()->debug("Searching for the order in ORderBook");

	OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(functor.instrument());
	if(orderGroups_.end() != it){
		if(BUY_SIDE == functor.side()){
			bool stop = false;
			mutex::scoped_lock lock(it->second->buyLock_);
			for(OrdersByPriceDescT::const_iterator oit = it->second->buyOrder_.begin(); 
					oit != it->second->buyOrder_.end(); ++oit){
				if(functor.match(oit->second, &stop))
					return oit->second;
				if(stop)
					break;
			}
		}else if(SELL_SIDE == functor.side()){
			bool stop = false;
			mutex::scoped_lock lock(it->second->sellLock_);
			for(OrdersByPriceAscT::const_iterator oit = it->second->sellOrder_.begin(); 
					oit != it->second->sellOrder_.end(); ++oit){
				if(functor.match(oit->second, &stop))
					return oit->second;
				if(stop)
					break;
			}
		}else{
			throw std::runtime_error("Invalid Side for the order's search");
		}
	}else{
		throw std::runtime_error("Unknown instrument for the order's search");
	}
	//aux::ExchLogger::instance()->debug("Order search in OrderBook failed");
	return IdT();
}

void OrderBookImpl::findAll(const OrderFunctor &functor, OrdersT *result)const
{
	assert(NULL != result);
	//aux::ExchLogger::instance()->debug("Searching for the orders in OrderBook");
	OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(functor.instrument());
	if(orderGroups_.end() != it){
		if(BUY_SIDE == functor.side()){
			bool stop = false;
			mutex::scoped_lock lock(it->second->buyLock_);
			for(OrdersByPriceDescT::const_iterator oit = it->second->buyOrder_.begin(); 
					oit != it->second->buyOrder_.end(); ++oit){
				if(functor.match(oit->second, &stop))
					result->push_back(oit->second);
				if(stop)
					break;
			}
		}else if(SELL_SIDE == functor.side()){
			bool stop = false;
			mutex::scoped_lock lock(it->second->sellLock_);
			for(OrdersByPriceAscT::const_iterator oit = it->second->sellOrder_.begin(); 
					oit != it->second->sellOrder_.end(); ++oit){
				if(functor.match(oit->second, &stop))
					result->push_back(oit->second);
				if(stop)
					break;
			}
		}else{
			throw std::runtime_error("Invalid Side for the order's search");
		}
	}else{
		throw std::runtime_error("Unknown instrument for the order's search");
	}
}

IdT OrderBookImpl::getTop(const SourceIdT &instrument, const Side &side)const
{
	//aux::ExchLogger::instance()->debug("Locating top order in OrderBook");

	OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(instrument);
	if(orderGroups_.end() != it){
		if(BUY_SIDE == side){
			mutex::scoped_lock lock(it->second->buyLock_);
			if(0 < it->second->buyOrder_.size())
				return it->second->buyOrder_.begin()->second;
		}else if(SELL_SIDE == side){
			mutex::scoped_lock lock(it->second->sellLock_);
			if(0 < it->second->sellOrder_.size())
				return it->second->sellOrder_.begin()->second;
		}else{
			throw std::runtime_error("Invalid Side for the order's top");
		}
	}else{
		throw std::runtime_error("Unknown instrument for the order's top");
	}
	//aux::ExchLogger::instance()->debug("Top order locate failed in OrderBook");
	return IdT();
}
