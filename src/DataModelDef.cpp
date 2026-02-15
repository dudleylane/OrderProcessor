/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "DataModelDef.h"
#include "ExchUtils.h"

using namespace std;
using namespace COP;
using namespace COP::OrdState;

namespace COP{

	bool operator < (const RawDataEntry &lft, const RawDataEntry &rght)
	{
		if(lft.type_ == rght.type_){
			if(lft.length_ == rght.length_){
				return 0 < memcmp(lft.data_, rght.data_, lft.length_);
			}else
				return lft.length_ < rght.length_;
		}else 
			return lft.type_ < rght.type_;
	}

}

RawDataEntry::RawDataEntry():type_(INVALID_RAWDATATYPE), data_(nullptr), length_(0)
{
}

RawDataEntry::RawDataEntry(RawDataType type, const char *data, u32 len):
	type_(type), data_(nullptr), length_(len)
{
	data_ = new char[length_];
	memcpy(data_, data, len);
}

IdT::IdT(const u64 &id, const u32 &date): id_(id), date_(date)
{}

void IdT::reset()
{
	date_ = 0;
	id_ = 0;
}

void IdT::toString(std::string &msg)const
{
	char buf[36];
	char *p = aux::toStr(buf, date_);
	p[0] = '.'; ++p;
	p = aux::toStr(p, id_);
	msg.append(buf, p - buf);
}

void IdT::serialize(std::string &msg)const
{
	char buf[32];
	char *p = buf;
	memcpy(buf, &date_, sizeof(date_)); 
	p += sizeof(date_);
	*p = ':';
	++p;
	memcpy(p, &id_, sizeof(id_));
	p += sizeof(id_);
	msg.append(buf, p - buf);
}

char *IdT::serialize(char *buf)const
{
	char *p = buf;
	memcpy(p, &date_, sizeof(date_)); 
	p += sizeof(date_);
	*p = ':';
	++p;
	memcpy(p, &id_, sizeof(id_));
	p += sizeof(id_);
/*	memcpy(p, this, sizeof(*this));
	p += sizeof(*this);*/
	return p;
}

const char *IdT::restore(const char *buf, size_t size)
{
	const char *p = buf;
	if(size < sizeof(date_) + 1 + sizeof(id_))
		throw std::runtime_error("Unable to restore IdT - buffer size less than expected!");
	memcpy(&date_, p, sizeof(date_));
	p += sizeof(date_);
	if(':' != *p)
		throw std::runtime_error("Unable to restore IdT - ':' missed after date!");
	++p;
	memcpy(&id_, p, sizeof(id_));
	p += sizeof(id_);
	return p;
}

EventData::EventData(const IdT &eventId):
	eventId_(eventId)
{}


bool GroupInstrumentsBySymbol::operator()(const InstrumentEntry &lft, const InstrumentEntry &rght)const
{
	return lft.symbol_ < rght.symbol_;
/*	if(lft.symbol_ == rght.symbol_){
		if(lft.securityId_ == rght.securityId_){
			if(lft.securityIdSource_ == rght.securityIdSource_){
				return lft.id_ < rght.id_;
			}else
				return lft.securityIdSource_ < rght.securityIdSource_;
		}else
			return lft.securityId_ < rght.securityId_;
	}else
		return lft.symbol_ < rght.symbol_;*/
}




OrderParams::OrderParams(const SourceIdT &dest, const SourceIdT &instrument, 
				const SourceIdT &account, const SourceIdT &clearing,
				const SourceIdT &source, const SourceIdT &clOrderId, 
				const SourceIdT &origClOrderID, const SourceIdT &executions):
	instrument_(instrument), account_(account), clearing_(clearing), destination_(dest), 
	execInstruct_(), creationTime_(0), lastUpdateTime_(0), expireTime_(0),
	settlDate_(0), price_(0.0), stopPx_(0.0), avgPx_(0.0), dayAvgPx_(0.0),	
	status_(INVALID_ORDSTATUS),	side_(INVALID_SIDE), ordType_(INVALID_ORDERTYPE),
	tif_(INVALID_TIF), settlType_(INVALID_SETTLTYPE), capacity_(INVALID_CAPACITY),
	currency_(INVALID_CURRENCY), minQty_(0), orderQty_(0), leavesQty_(0),
	cumQty_(0), dayOrderQty_(0), dayCumQty_(0), clOrderId_(clOrderId), origClOrderId_(origClOrderID),
	source_(source), orderId_(), origOrderId_(), executions_(executions)
{
	instrument_.load();
}

OrderParams::OrderParams(const OrderParams &ord):
	instrument_(ord.instrument_), account_(ord.account_), clearing_(ord.clearing_),
	destination_(ord.destination_), execInstruct_(ord.execInstruct_),
	creationTime_(ord.creationTime_), lastUpdateTime_(ord.lastUpdateTime_),
	expireTime_(ord.expireTime_), settlDate_(ord.settlDate_), price_(ord.price_),
	stopPx_(ord.stopPx_), avgPx_(ord.avgPx_), dayAvgPx_(ord.dayAvgPx_),
	status_(ord.status_), side_(ord.side_), ordType_(ord.ordType_),
	tif_(ord.tif_), settlType_(ord.settlType_), capacity_(ord.capacity_),
	currency_(ord.currency_), minQty_(ord.minQty_), orderQty_(ord.orderQty_),
	leavesQty_(ord.leavesQty_), cumQty_(ord.cumQty_), dayOrderQty_(ord.dayOrderQty_),
	dayCumQty_(ord.dayCumQty_), clOrderId_(ord.clOrderId_), origClOrderId_(ord.origClOrderId_),
	source_(ord.source_), orderId_(ord.orderId_), origOrderId_(ord.origOrderId_), executions_(ord.executions_),
	entryMutex_()
{
	instrument_.load();
}

OrderParams::~OrderParams()
{}

void OrderParams::applyTrade(const TradeExecEntry *trade)
{
	assert(nullptr != trade);
	if(leavesQty_ < trade->lastQty_)
		throw std::runtime_error("OrderParams::applyTrade: trade quantity exceeds leaves quantity!");
	leavesQty_ -= trade->lastQty_;
	cumQty_ += trade->lastQty_;

	executions_.get()->push_back(EventData(trade->execId_));
}

void OrderParams::addExecution(const IdT &execId)
{
	executions_.get()->push_back(EventData(execId));
}

OrderEntry::OrderEntry(const SourceIdT &source, const SourceIdT &dest, 
		const SourceIdT &clOrderId, const SourceIdT &origClOrderID, const SourceIdT &instrument, 
		const SourceIdT &account, const SourceIdT &clearing, const SourceIdT &executions):
	OrderParams(dest, instrument, account, clearing, source, clOrderId, origClOrderID, executions)
{}

OrderEntry::OrderEntry(const OrderEntry &ord):
	OrderParams(ord)
{
}

OrderEntry::~OrderEntry()
{}

bool OrderEntry::compare(const OrderEntry &val)const
{
	return (val.instrument_.getId() == instrument_.getId())&&
		(val.account_.getId() == account_.getId())&&
		(val.clearing_.getId() == clearing_.getId())&&
		(val.destination_.getId() == destination_.getId())&&
		(val.execInstruct_ == execInstruct_)&&
		(val.creationTime_ == creationTime_)&&
		(val.lastUpdateTime_ == lastUpdateTime_)&&
		(val.expireTime_ == expireTime_)&&
		(val.settlDate_ == settlDate_)&&
		(val.price_ == price_)&&
		(val.stopPx_ == stopPx_)&&
		(val.avgPx_ == avgPx_)&&
		(val.dayAvgPx_ == dayAvgPx_)&&
		(val.status_ == status_)&&
		(val.side_ == side_)&&
		(val.ordType_ == ordType_)&&
		(val.tif_ == tif_)&&
		(val.settlType_ == settlType_)&&
		(val.capacity_ == capacity_)&&
		(val.currency_ == currency_)&&
		(val.minQty_ == minQty_)&&
		(val.orderQty_ == orderQty_)&&
		(val.leavesQty_ == leavesQty_)&&
		(val.cumQty_ == cumQty_)&&
		(val.dayOrderQty_ == dayOrderQty_)&&
		(val.dayCumQty_ == dayCumQty_)&&
		(val.clOrderId_.getId() == clOrderId_.getId())&&
		(val.origClOrderId_.getId() == origClOrderId_.getId())&&
		(val.source_.getId() == source_.getId())&&
		(val.orderId_ == orderId_)&&
		(val.origOrderId_ == origOrderId_)&&
		(val.executions_.getId() == executions_.getId())&&
		(stateMachinePersistance_.compare(val.stateMachinePersistance_));
}

OrderStatePersistence OrderEntry::stateMachinePersistance()const
{
	OrderStatePersistence pers = stateMachinePersistance_;
	pers.orderData_ = const_cast<OrderEntry *>(this);
	return pers;
}

void OrderEntry::setStateMachinePersistance(const OrderStatePersistence &persist)
{
	stateMachinePersistance_ = persist;
	stateMachinePersistance_.orderData_ = nullptr;
}

OrderEntry *OrderEntry::clone()const
{
	return new OrderEntry(*this);
}

bool OrderEntry::isValid(std::string *invalid)const
{
	assert(nullptr != invalid);
	try{
		if(0 == clOrderId_.get().length_){
			*invalid = "Invalid value of the client order Id!";
			return false;
		}
		if(!instrument_.get().isValid(invalid))
			return false;
		if(!account_.get().isValid(invalid))
			return false;
		if(!clearing_.get().isValid(invalid))
			return false;

		if(0 == creationTime_){
			*invalid = "Invalid value of the creation time!";
			return false;		
		}
		if(0 == lastUpdateTime_){
			*invalid = "Invalid value of the lastUpdate time!";
			return false;		
		}
		if(INVALID_ORDSTATUS == status_){
			*invalid = "Invalid value of the order status!";
			return false;		
		}
		if(INVALID_SIDE == side_){
			*invalid = "Invalid value of the side!";
			return false;		
		}
		if(INVALID_ORDERTYPE == ordType_){
			*invalid = "Invalid value of the order type!";
			return false;		
		}
		if(INVALID_TIF == tif_){
			*invalid = "Invalid value of the timeInForce!";
			return false;		
		}
		if(INVALID_SETTLTYPE == settlType_){
			*invalid = "Invalid value of the settlement type!";
			return false;
		}
		if(INVALID_CAPACITY == capacity_){
			*invalid = "Invalid value of the order capacity!";
			return false;		
		}
		if(INVALID_CURRENCY == currency_){
			*invalid = "Invalid value of the order currency!";
			return false;
		}
		return true;
	}catch(const std::exception &ex){
		*invalid = ex.what();
	}catch(...){
		*invalid = "Unexpected error during OrderEntry::isValid()";
	}
	return false;
}

bool OrderEntry::isReplaceValid(std::string *invalid)const
{
	assert(nullptr != invalid);
	bool rez = true;
	return rez;
}

bool InstrumentEntry::isValid(std::string *invalid)const
{
	assert(nullptr != invalid);
	try{
		if(symbol_.empty()){
			*invalid = "Invalid value of the instrument's symbol!";
			return false;		
		}
		if(securityId_.empty()){
			*invalid = "Invalid value of the instrument's securityId!";
			return false;		
		}
		if(securityIdSource_.empty()){
			*invalid = "Invalid value of the instrument's securityIdSource!";
			return false;		
		}
		if(!id_.isValid()){
			*invalid = "Invalid value of the instrument's id!";
			return false;		
		}
		return true;
	}catch(const std::exception &ex){
		*invalid = ex.what();
	}catch(...){
		*invalid = "Unexpected error during InstrumentEntry::isValid()";
	}
	return false;
}

bool AccountEntry::isValid(std::string *invalid)const
{
	assert(nullptr != invalid);
	try{
		if(account_.empty()){
			*invalid = "Invalid value of the account's account!";
			return false;		
		}
		if(firm_.empty()){
			*invalid = "Invalid value of the account's firm!";
			return false;		
		}
		if(INVALID_ACCOUNTTYPE == type_){
			*invalid = "Invalid value of the account's type!";
			return false;		
		}
		if(!id_.isValid()){
			*invalid = "Invalid value of the account's id!";
			return false;		
		}
		return true;
	}catch(const std::exception &ex){
		*invalid = ex.what();
	}catch(...){
		*invalid = "Unexpected error during InstrumentEntry::isValid()";
	}
	return false;
}

bool ClearingEntry::isValid(std::string *invalid)const
{
	assert(nullptr != invalid);
	try{
		if(firm_.empty()){
			*invalid = "Invalid value of the clearing firm!";
			return false;		
		}
		if(!id_.isValid()){
			*invalid = "Invalid value of the clearing id!";
			return false;		
		}
		return true;
	}catch(const std::exception &ex){
		*invalid = ex.what();
	}catch(...){
		*invalid = "Unexpected error during InstrumentEntry::isValid()";
	}
	return false;
}

ExecParams::ExecParams(): 
	type_(INVALID_EXECTYPE), transactTime_(0), orderId_(), execId_(), orderStatus_(INVALID_ORDSTATUS)
{}

ExecParams::ExecParams(const ExecParams &param):
	type_(param.type_), transactTime_(param.transactTime_), orderId_(param.orderId_), execId_(param.execId_), 
	orderStatus_(param.orderStatus_)
{}

ExecTradeParams::ExecTradeParams(): 
	lastQty_(0), lastPx_(0.0), currency_(INVALID_CURRENCY), tradeDate_(0)
{}
ExecTradeParams::ExecTradeParams(const ExecTradeParams &param): 
	lastQty_(param.lastQty_), lastPx_(param.lastPx_), currency_(param.currency_), tradeDate_(param.tradeDate_)
{}

ExecutionEntry::ExecutionEntry(): ExecParams()
{}

ExecutionEntry::ExecutionEntry(const ExecParams &param): ExecParams(param)
{}

ExecutionEntry *ExecutionEntry::clone()const
{
	return new ExecutionEntry();
}

TradeExecEntry::TradeExecEntry()
{}

TradeExecEntry::TradeExecEntry(const ExecutionEntry &exec, const ExecTradeParams &trade): 
	ExecutionEntry(exec), ExecTradeParams(trade)
{} 

TradeExecEntry::TradeExecEntry(const TradeExecEntry &params): ExecutionEntry(params), ExecTradeParams(params)
{}

ExecutionEntry *TradeExecEntry::clone()const
{
	return new TradeExecEntry(*this);
}

ExecutionEntry *RejectExecEntry::clone()const
{
	return new RejectExecEntry;
}

ExecutionEntry *ExecCorrectExecEntry::clone()const
{
	return new ExecCorrectExecEntry;
}

ExecutionEntry *ReplaceExecEntry::clone()const
{
	return new ReplaceExecEntry;
}

ExecutionEntry *TradeCancelExecEntry::clone()const
{
	return new TradeCancelExecEntry;
}


