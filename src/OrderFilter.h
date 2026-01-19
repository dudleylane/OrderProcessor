/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "DataModelDef.h"
#include "FilterImpl.h"
#include "EntryFilter.h"

namespace COP{ namespace SubscrMgr{

/*	For the one entity, filter joined by AND
	enum FilterEnumOperations{
		equal,
		notequal,
		in,
		notin
	};

	enum FilterStringOperations{
		equal,
		notequal,
		match,
		notmatch,
		in,
		notin
	};

	enum FilterNumerOperations{
		equal,
		notequal,
		less,
		greater,
		lessEqual,
		greaterEqual,
		inRange,
		outOfRange
	};*/

	class OrderElementFilter{
	public:
		virtual ~OrderElementFilter(){}
		virtual bool match(const OrderParams &params)const = 0;	
	};

	class OrderEntryFilter{
	public:
		virtual ~OrderEntryFilter(){}
		virtual bool match(const OrderEntry &params)const = 0;	
	};

class OrderFilter //112
{
public:
	OrderFilter(void);
	~OrderFilter(void);

	void addFilter(OrderElementFilter *fltr);
	void addFilter(OrderEntryFilter *fltr);
	
	void addInstrumentFilter(InstrumentFilter *fltr);
	void addAccountFilter(AccountFilter *fltr);
	void addClearingFilter(ClearingFilter *fltr);

	bool match(const OrderEntry &params)const;

	void release();

	bool getInstrument(InstrumentEntry *val)const;
private:
	InstrumentFilter instrumentFilter_;//InstrumentEntry instrument_; 28
	AccountFilter accountFilter_;//AccountEntry account_; 28
	ClearingFilter clearingFilter_;//ClearingEntry clearing_; 28

	typedef std::deque<OrderElementFilter *> FiltersT;
	FiltersT filters_;

	typedef std::deque<OrderEntryFilter *> EntryFiltersT;
	EntryFiltersT entryFilters_;
	//OrderStatus status_;
	//Side side_;
	//OrderType ordType_;
	//TimeInForce tif_;
	//SettlTypeBase settlType_;
	//Capacity capacity_;
	//Currency currency_;
	//InstructionSetT execInstruct_;

	//SourceIdT destination_;
	//SourceIdT source_;

	//PriceT price_;
	//PriceT stopPx_;
	//PriceT avgPx_;
	//PriceT dayAvgPx_;
	//QuantityT minQty_;
	//QuantityT orderQty_;
	//QuantityT leavesQty_;
	//QuantityT cumQty_;
	//QuantityT dayOrderQty_;
	//QuantityT dayCumQty_;
	//DateTimeT expireTime_;
	//DateTimeT settlDate_;
	//DateTimeT creationTime_;
	//DateTimeT lastUpdateTime_;
};

class OrderStatusEqualFilter: public OrderElementFilter{
public:
	OrderStatusEqualFilter(OrderStatus value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<OrderStatus> impl_;
};

class OrderStatusInFilter: public Impl::EnumInSetFilter<OrderStatus>, public OrderElementFilter{
public:
	OrderStatusInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class SideEqualFilter: public OrderElementFilter{
public:
	SideEqualFilter(Side value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<Side> impl_;
};

class SideInFilter: public Impl::EnumInSetFilter<Side>, public OrderElementFilter{
public:
	SideInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class OrderTypeEqualFilter: public OrderElementFilter{
public:
	OrderTypeEqualFilter(OrderType value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<OrderType> impl_;
};

class OrderTypeInFilter: public Impl::EnumInSetFilter<OrderType>, public OrderElementFilter{
public:
	OrderTypeInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class TIFEqualFilter: public OrderElementFilter{
public:
	TIFEqualFilter(TimeInForce value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<TimeInForce> impl_;
};

class TIFInFilter: public Impl::EnumInSetFilter<TimeInForce>, public OrderElementFilter{
public:
	TIFInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class SettlTypeEqualFilter: public OrderElementFilter{
public:
	SettlTypeEqualFilter(SettlTypeBase value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<SettlTypeBase> impl_;
};

class SettlTypeInFilter: public Impl::EnumInSetFilter<SettlTypeBase>, public OrderElementFilter{
public:
	SettlTypeInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class CapacityEqualFilter: public OrderElementFilter{
public:
	CapacityEqualFilter(Capacity value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<Capacity> impl_;
};

class CapacityInFilter: public Impl::EnumInSetFilter<Capacity>, public OrderElementFilter{
public:
	CapacityInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class CurrencyEqualFilter: public OrderElementFilter{
public:
	CurrencyEqualFilter(Currency value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumEqualFilter<Currency> impl_;
};

class CurrencyInFilter: public Impl::EnumInSetFilter<Currency>, public OrderElementFilter{
public:
	CurrencyInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class InstructionsEqualFilter: public OrderElementFilter{
public:
	InstructionsEqualFilter(InstructionSetT value);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::EnumSetEqualFilter<Instructions> impl_;
};

class InstructionsInFilter: public Impl::EnumSetInSetFilter<Instructions>, public OrderElementFilter{
public:
	InstructionsInFilter(const ValuesT &values);
	virtual bool match(const OrderParams &params)const;
};

class DestinationFilter: public OrderElementFilter{
public:
	DestinationFilter(Impl::StringFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::StringFilter *filter_;
};

class SourceFilter: public OrderEntryFilter{
public:
	SourceFilter(Impl::StringFilter *flt);
	virtual bool match(const OrderEntry &params)const;
private:
	Impl::StringFilter *filter_;
};

class PxFilter: public OrderElementFilter{
public:
	PxFilter(Impl::PriceFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::PriceFilter *filter_;
};

class StopPxFilter: public OrderElementFilter{
public:
	StopPxFilter(Impl::PriceFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::PriceFilter *filter_;
};

class AvgPxFilter: public OrderElementFilter{
public:
	AvgPxFilter(Impl::PriceFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::PriceFilter *filter_;
};

class DayAvgPxFilter: public OrderElementFilter{
public:
	DayAvgPxFilter(Impl::PriceFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::PriceFilter *filter_;
};

class MinQtyFilter: public OrderElementFilter{
public:
	MinQtyFilter(Impl::QuantityFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::QuantityFilter *filter_;
};

class OrderQtyFilter: public OrderElementFilter{
public:
	OrderQtyFilter(Impl::QuantityFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::QuantityFilter *filter_;
};

class LeavesQtyFilter: public OrderElementFilter{
public:
	LeavesQtyFilter(Impl::QuantityFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::QuantityFilter *filter_;
};

class CumQtyFilter: public OrderElementFilter{
public:
	CumQtyFilter(Impl::QuantityFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::QuantityFilter *filter_;
};

class DayOrderQtyFilter: public OrderElementFilter{
public:
	DayOrderQtyFilter(Impl::QuantityFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::QuantityFilter *filter_;
};

class DayCumQtyFilter: public OrderElementFilter{
public:
	DayCumQtyFilter(Impl::QuantityFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::QuantityFilter *filter_;
};

class ExpireTimeFilter: public OrderElementFilter{
public:
	ExpireTimeFilter(Impl::DateTimeFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::DateTimeFilter *filter_;
};

class SettlDateFilter: public OrderElementFilter{
public:
	SettlDateFilter(Impl::DateTimeFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::DateTimeFilter *filter_;
};

class CreationTimeFilter: public OrderElementFilter{
public:
	CreationTimeFilter(Impl::DateTimeFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::DateTimeFilter *filter_;
};

class LastUpdateTimeFilter: public OrderElementFilter{
public:
	LastUpdateTimeFilter(Impl::DateTimeFilter *flt);
	virtual bool match(const OrderParams &params)const;
private:
	Impl::DateTimeFilter *filter_;
};


}}