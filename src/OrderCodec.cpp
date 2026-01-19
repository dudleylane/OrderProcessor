/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "OrderCodec.h"
#include "StringTCodec.h"

using namespace std;
using namespace COP;
using namespace COP::Codec;

namespace{
	const int BUFFER_SIZE = 256;
}

/*	
	struct OrderParams{
	public:
		explicit OrderParams(const SourceIdT &dest, const SourceIdT &instrument, 
				const SourceIdT &account, const SourceIdT &clearing,
				const SourceIdT &source, const SourceIdT &clOrderId, 
				const SourceIdT &origClOrderID, const SourceIdT &executions);
		OrderParams(const OrderParams &ord);
		~OrderParams();

		WideDataLazyRef<InstrumentEntry> instrument_;//136
		WideDataLazyRef<AccountEntry> account_;//112
		WideDataLazyRef<ClearingEntry> clearing_;//72
		WideDataLazyRef<StringT> destination_; //~32 + 24
		InstructionSetT execInstruct_; //28

		DateTimeT creationTime_; //8
		DateTimeT lastUpdateTime_;
		DateTimeT expireTime_;
		DateTimeT settlDate_;

		PriceT price_; //8
		PriceT stopPx_;
		PriceT avgPx_;
		PriceT dayAvgPx_;

		OrderStatus status_;
		Side side_;
		OrderType ordType_;
		TimeInForce tif_;
		SettlTypeBase settlType_;
		Capacity capacity_;
		Currency currency_;
		QuantityT minQty_; //4
		QuantityT orderQty_;
		QuantityT leavesQty_;
		QuantityT cumQty_;
		QuantityT dayOrderQty_;
		QuantityT dayCumQty_;

		WideDataLazyRef<RawDataEntry> clOrderId_; //56
		WideDataLazyRef<RawDataEntry> origClOrderId_;
		WideDataLazyRef<StringT> source_;

		IdT orderId_;
		IdT origOrderId_;

		WideDataLazyRef<ExecutionsT *> executions_; //todo: add executions when transaction commits

		OrdState::OrderStatePersistence stateMachinePersistance_;

		void applyTrade(const TradeExecEntry *trade);
	private:
		const OrderParams& operator =(const OrderParams &);
	};
*/

OrderCodec::OrderCodec(void)
{
}

OrderCodec::~OrderCodec(void)
{
}

void OrderCodec::encode(const OrderEntry &val, std::string *buf, IdT *id, u32 *version)
{
	assert(nullptr != buf);
	assert(nullptr != id);
	assert(nullptr != version);

	*id = val.orderId_;
	*version = 0;

	string strbuf;
	buf->reserve(buf->size() + BUFFER_SIZE);
	val.instrument_.getId().serialize(*buf);
	buf->append(1, '.');

	val.account_.getId().serialize(*buf);
	buf->append(1, '.');

	val.clearing_.getId().serialize(*buf);
	buf->append(1, '.');

	val.destination_.getId().serialize(*buf);
	buf->append(1, '.');

	// todo: implement
	//val.execInstruct_.id_.serialize(*buf);
	//buf->append(1, '.');

	val.clOrderId_.getId().serialize(*buf);
	buf->append(1, '.');

	val.origClOrderId_.getId().serialize(*buf);
	buf->append(1, '.');

	val.source_.getId().serialize(*buf);
	buf->append(1, '.');

	val.executions_.getId().serialize(*buf);
	buf->append(1, '.');

	val.origOrderId_.serialize(*buf);
	buf->append(1, '.');

	char typebuf[36];
	memcpy(typebuf, &(val.creationTime_), sizeof(val.creationTime_));
	buf->append(typebuf, sizeof(val.creationTime_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.lastUpdateTime_), sizeof(val.lastUpdateTime_));
	buf->append(typebuf, sizeof(val.lastUpdateTime_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.expireTime_), sizeof(val.expireTime_));
	buf->append(typebuf, sizeof(val.expireTime_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.settlDate_), sizeof(val.settlDate_));
	buf->append(typebuf, sizeof(val.settlDate_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.price_), sizeof(val.price_));
	buf->append(typebuf, sizeof(val.price_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.stopPx_), sizeof(val.stopPx_));
	buf->append(typebuf, sizeof(val.stopPx_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.avgPx_), sizeof(val.avgPx_));
	buf->append(typebuf, sizeof(val.avgPx_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.dayAvgPx_), sizeof(val.dayAvgPx_));
	buf->append(typebuf, sizeof(val.dayAvgPx_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.status_), sizeof(val.status_));
	buf->append(typebuf, sizeof(val.status_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.side_), sizeof(val.side_));
	buf->append(typebuf, sizeof(val.side_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.ordType_), sizeof(val.ordType_));
	buf->append(typebuf, sizeof(val.ordType_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.tif_), sizeof(val.tif_));
	buf->append(typebuf, sizeof(val.tif_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.settlType_), sizeof(val.settlType_));
	buf->append(typebuf, sizeof(val.settlType_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.capacity_), sizeof(val.capacity_));
	buf->append(typebuf, sizeof(val.capacity_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.currency_), sizeof(val.currency_));
	buf->append(typebuf, sizeof(val.currency_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.minQty_), sizeof(val.minQty_));
	buf->append(typebuf, sizeof(val.minQty_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.orderQty_), sizeof(val.orderQty_));
	buf->append(typebuf, sizeof(val.orderQty_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.leavesQty_), sizeof(val.leavesQty_));
	buf->append(typebuf, sizeof(val.leavesQty_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.cumQty_), sizeof(val.cumQty_));
	buf->append(typebuf, sizeof(val.cumQty_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.dayOrderQty_), sizeof(val.dayOrderQty_));
	buf->append(typebuf, sizeof(val.dayOrderQty_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.dayCumQty_), sizeof(val.dayCumQty_));
	buf->append(typebuf, sizeof(val.dayCumQty_));
	buf->append(1, '.');


	val.stateMachinePersistance_.serialize(*buf);
}

char *OrderCodec::encode(const OrderEntry &val, char *buf, IdT *id, u32 *version)
{
	assert(nullptr != buf);
	assert(nullptr != id);
	assert(nullptr != version);

	*id = val.orderId_;
	*version = 0;

	char *p = buf;
	p = val.instrument_.getId().serialize(p);
	*p = '.';++p;

	p = val.account_.getId().serialize(p);
	*p = '.';++p;

	p = val.clearing_.getId().serialize(p);
	*p = '.';++p;

	p = val.destination_.getId().serialize(p);
	*p = '.';++p;

	// todo: implement
	//val.execInstruct_.id_.serialize(*buf);
	//buf->append(1, '.');

	p = val.clOrderId_.getId().serialize(p);
	*p = '.';++p;

	p = val.origClOrderId_.getId().serialize(p);
	*p = '.';++p;

	p = val.source_.getId().serialize(p);
	*p = '.';++p;

	p = val.executions_.getId().serialize(p);
	*p = '.';++p;

	p = val.origOrderId_.serialize(p);
	*p = '.';++p;

	memcpy(p, &(val.creationTime_), sizeof(val.creationTime_));
	p += sizeof(val.creationTime_);
	*p = '.';++p;

	memcpy(p, &(val.lastUpdateTime_), sizeof(val.lastUpdateTime_));
	p += sizeof(val.lastUpdateTime_);
	*p = '.';++p;

	memcpy(p, &(val.expireTime_), sizeof(val.expireTime_));
	p += sizeof(val.expireTime_);
	*p = '.';++p;

	memcpy(p, &(val.settlDate_), sizeof(val.settlDate_));
	p += sizeof(val.settlDate_);
	*p = '.';++p;

	memcpy(p, &(val.price_), sizeof(val.price_));
	p += sizeof(val.price_);
	*p = '.';++p;

	memcpy(p, &(val.stopPx_), sizeof(val.stopPx_));
	p += sizeof(val.stopPx_);
	*p = '.';++p;

	memcpy(p, &(val.avgPx_), sizeof(val.avgPx_));
	p += sizeof(val.avgPx_);
	*p = '.';++p;

	memcpy(p, &(val.dayAvgPx_), sizeof(val.dayAvgPx_));
	p += sizeof(val.dayAvgPx_);
	*p = '.';++p;

	memcpy(p, &(val.status_), sizeof(val.status_));
	p += sizeof(val.status_);
	*p = '.';++p;

	memcpy(p, &(val.side_), sizeof(val.side_));
	p += sizeof(val.side_);
	*p = '.';++p;

	memcpy(p, &(val.ordType_), sizeof(val.ordType_));
	p += sizeof(val.ordType_);
	*p = '.';++p;

	memcpy(p, &(val.tif_), sizeof(val.tif_));
	p += sizeof(val.tif_);
	*p = '.';++p;

	memcpy(p, &(val.settlType_), sizeof(val.settlType_));
	p += sizeof(val.settlType_);
	*p = '.';++p;

	memcpy(p, &(val.capacity_), sizeof(val.capacity_));
	p += sizeof(val.capacity_);
	*p = '.';++p;

	memcpy(p, &(val.currency_), sizeof(val.currency_));
	p += sizeof(val.currency_);
	*p = '.';++p;

	memcpy(p, &(val.minQty_), sizeof(val.minQty_));
	p += sizeof(val.minQty_);
	*p = '.';++p;

	memcpy(p, &(val.orderQty_), sizeof(val.orderQty_));
	p += sizeof(val.orderQty_);
	*p = '.';++p;

	memcpy(p, &(val.leavesQty_), sizeof(val.leavesQty_));
	p += sizeof(val.leavesQty_);
	*p = '.';++p;

	memcpy(p, &(val.cumQty_), sizeof(val.cumQty_));
	p += sizeof(val.cumQty_);
	*p = '.';++p;

	memcpy(p, &(val.dayOrderQty_), sizeof(val.dayOrderQty_));
	p += sizeof(val.dayOrderQty_);
	*p = '.';++p;

	memcpy(p, &(val.dayCumQty_), sizeof(val.dayCumQty_));
	p += sizeof(val.dayCumQty_);
	*p = '.';++p;


	p = val.stateMachinePersistance_.serialize(p);
	return p;
}

OrderEntry *OrderCodec::decode(const IdT& id, u32 /*version*/, const char *buf, size_t size)
{
	assert(nullptr != buf);
	assert(0 < size);

	SourceIdT sourceId, destId, clOrderId, origClOrderID, instrumentId, accountId, clearingId, executionsId;

	const char *p = instrumentId.restore(buf, size);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after instrumentId!");
	++p;

	p = accountId.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after accountId!");
	++p;

	p = clearingId.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after cleaingId!");
	++p;

	p = destId.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after destinationId!");
	++p;

	/* todo: implement
	p = val->execInstruct_.id_.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after execInstructId!");
	++p;*/

	p = clOrderId.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after clOrderId_!");
	++p;

	p = origClOrderID.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after origClOrderId_!");
	++p;

	p = sourceId.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after source_!");
	++p;

	p = executionsId.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after executions_!");
	++p;

	std::unique_ptr<OrderEntry> val(new OrderEntry(sourceId, destId, clOrderId, origClOrderID, 
					instrumentId, accountId, clearingId, executionsId));
	val->orderId_ = id;

	p = val->origOrderId_.restore(p, size - (p - buf));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after origOrderId_!");
	++p;

	if(size < (p - buf) + sizeof(val->creationTime_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode creationTime!");
	memcpy(&(val->creationTime_), p, sizeof(val->creationTime_));
	p += sizeof(val->creationTime_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after creationTime!");
	++p;

	if(size < (p - buf) + sizeof(val->lastUpdateTime_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode lastUpdateTime_!");
	memcpy(&(val->lastUpdateTime_), p, sizeof(val->lastUpdateTime_));
	p += sizeof(val->lastUpdateTime_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after lastUpdateTime_!");
	++p;

	if(size < (p - buf) + sizeof(val->expireTime_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode expireTime_!");
	memcpy(&(val->expireTime_), p, sizeof(val->expireTime_));
	p += sizeof(val->expireTime_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after expireTime_!");
	++p;

	if(size < (p - buf) + sizeof(val->settlDate_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode settlDate_!");
	memcpy(&(val->settlDate_), p, sizeof(val->settlDate_));
	p += sizeof(val->settlDate_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after settlDate_!");
	++p;

	if(size < (p - buf) + sizeof(val->price_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode price_!");
	memcpy(&(val->price_), p, sizeof(val->price_));
	p += sizeof(val->price_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after price_!");
	++p;

	if(size < (p - buf) + sizeof(val->stopPx_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode stopPx_!");
	memcpy(&(val->stopPx_), p, sizeof(val->stopPx_));
	p += sizeof(val->stopPx_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after stopPx_!");
	++p;

	if(size < (p - buf) + sizeof(val->avgPx_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode avgPx_!");
	memcpy(&(val->avgPx_), p, sizeof(val->avgPx_));
	p += sizeof(val->avgPx_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after avgPx_!");
	++p;

	if(size < (p - buf) + sizeof(val->dayAvgPx_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode dayAvgPx_!");
	memcpy(&(val->dayAvgPx_), p, sizeof(val->dayAvgPx_));
	p += sizeof(val->dayAvgPx_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after dayAvgPx_!");
	++p;

	if(size < (p - buf) + sizeof(val->status_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode status_!");
	memcpy(&(val->status_), p, sizeof(val->status_));
	p += sizeof(val->status_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after status_!");
	++p;

	if(size < (p - buf) + sizeof(val->side_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode side_!");
	memcpy(&(val->side_), p, sizeof(val->side_));
	p += sizeof(val->side_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after side_!");
	++p;

	if(size < (p - buf) + sizeof(val->ordType_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode ordType_!");
	memcpy(&(val->ordType_), p, sizeof(val->ordType_));
	p += sizeof(val->ordType_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after ordType_!");
	++p;

	if(size < (p - buf) + sizeof(val->tif_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode tif_!");
	memcpy(&(val->tif_), p, sizeof(val->tif_));
	p += sizeof(val->tif_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after tif_!");
	++p;

	if(size < (p - buf) + sizeof(val->settlType_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode settlType_!");
	memcpy(&(val->settlType_), p, sizeof(val->settlType_));
	p += sizeof(val->settlType_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after settlType_!");
	++p;

	if(size < (p - buf) + sizeof(val->capacity_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode capacity_!");
	memcpy(&(val->capacity_), p, sizeof(val->capacity_));
	p += sizeof(val->capacity_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after capacity_!");
	++p;

	if(size < (p - buf) + sizeof(val->currency_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode currency_!");
	memcpy(&(val->currency_), p, sizeof(val->currency_));
	p += sizeof(val->currency_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after currency_!");
	++p;

	if(size < (p - buf) + sizeof(val->minQty_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode minQty_!");
	memcpy(&(val->minQty_), p, sizeof(val->minQty_));
	p += sizeof(val->minQty_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after minQty_!");
	++p;

	if(size < (p - buf) + sizeof(val->orderQty_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode orderQty_!");
	memcpy(&(val->orderQty_), p, sizeof(val->orderQty_));
	p += sizeof(val->orderQty_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after orderQty_!");
	++p;

	if(size < (p - buf) + sizeof(val->leavesQty_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode leavesQty_!");
	memcpy(&(val->leavesQty_), p, sizeof(val->leavesQty_));
	p += sizeof(val->leavesQty_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after leavesQty_!");
	++p;

	if(size < (p - buf) + sizeof(val->cumQty_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode cumQty_!");
	memcpy(&(val->cumQty_), p, sizeof(val->cumQty_));
	p += sizeof(val->cumQty_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after cumQty_!");
	++p;

	if(size < (p - buf) + sizeof(val->dayOrderQty_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode dayOrderQty_!");
	memcpy(&(val->dayOrderQty_), p, sizeof(val->dayOrderQty_));
	p += sizeof(val->dayOrderQty_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after dayOrderQty_!");
	++p;

	if(size < (p - buf) + sizeof(val->dayCumQty_))
		throw std::runtime_error("Invalid format of the encoded OrderEntry - size less than required, unable decode dayCumQty_!");
	memcpy(&(val->dayCumQty_), p, sizeof(val->dayCumQty_));
	p += sizeof(val->dayCumQty_);
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded OrderEntry - missed '.' after dayCumQty_!");
	++p;

	p = val->stateMachinePersistance_.restore(p, size - (p - buf));

	return val.release();
}