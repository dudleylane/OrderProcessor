/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "OrderFilter.h"

#include <cassert>

using namespace COP;
using namespace COP::SubscrMgr;

OrderFilter::OrderFilter(void)
{
}

OrderFilter::~OrderFilter(void)
{
}

void OrderFilter::release()
{
	instrumentFilter_.release();
	accountFilter_.release();
	clearingFilter_.release();
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		if(NULL != *it)
			delete *it;
	}

	for(EntryFiltersT::const_iterator eit = entryFilters_.begin(); eit != entryFilters_.end(); ++eit){
		if(NULL != *eit)
			delete *eit;
	}

}

void OrderFilter::addFilter(OrderElementFilter *fltr)
{
	assert(NULL != fltr);
	filters_.push_back(fltr);
}

void OrderFilter::addFilter(OrderEntryFilter *fltr)
{
	assert(NULL != fltr);
	entryFilters_.push_back(fltr);
}

void OrderFilter::addInstrumentFilter(InstrumentFilter *fltr)
{
	instrumentFilter_ = *fltr;
}

void OrderFilter::addAccountFilter(AccountFilter *fltr)
{
	accountFilter_ = *fltr;
}

void OrderFilter::addClearingFilter(ClearingFilter *fltr)
{
	clearingFilter_ = *fltr;
}

bool OrderFilter::match(const OrderEntry &params)const
{
	if(instrumentFilter_.match(params)&&
		accountFilter_.match(params)&&
		clearingFilter_.match(params)){
		for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
			assert(NULL != *it);
			if(!(*it)->match(params))
				return false;
		}
		for(EntryFiltersT::const_iterator eit = entryFilters_.begin(); eit != entryFilters_.end(); ++eit){
			assert(NULL != *eit);
			if(!(*eit)->match(params))
				return false;
		}
		return true;
	}
	return false;
}

bool OrderFilter::getInstrument(InstrumentEntry *val)const
{
	return instrumentFilter_.getInstrumentEntry(val);
}

OrderStatusEqualFilter::OrderStatusEqualFilter(OrderStatus v): impl_(v)
{}

bool OrderStatusEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.status_);
}

OrderStatusInFilter::OrderStatusInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<OrderStatus>(values)
{}
	
bool OrderStatusInFilter::match(const OrderParams &params)const
{
	return filter(params.status_);
}

SideEqualFilter::SideEqualFilter(Side v): impl_(v)
{}

bool SideEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.side_);
}

SideInFilter::SideInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<Side>(values)
{}
	
bool SideInFilter::match(const OrderParams &params)const
{
	return filter(params.side_);
}

OrderTypeEqualFilter::OrderTypeEqualFilter(OrderType v): impl_(v)
{}

bool OrderTypeEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.ordType_);
}

OrderTypeInFilter::OrderTypeInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<OrderType>(values)
{}
	
bool OrderTypeInFilter::match(const OrderParams &params)const
{
	return filter(params.ordType_);
}

TIFEqualFilter::TIFEqualFilter(TimeInForce v): impl_(v)
{}

bool TIFEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.tif_);
}

TIFInFilter::TIFInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<TimeInForce>(values)
{}
	
bool TIFInFilter::match(const OrderParams &params)const
{
	return filter(params.tif_);
}

SettlTypeEqualFilter::SettlTypeEqualFilter(SettlTypeBase v): impl_(v)
{}

bool SettlTypeEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.settlType_);
}

SettlTypeInFilter::SettlTypeInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<SettlTypeBase>(values)
{}
	
bool SettlTypeInFilter::match(const OrderParams &params)const
{
	return filter(params.settlType_);
}

CapacityEqualFilter::CapacityEqualFilter(Capacity v): impl_(v)
{}

bool CapacityEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.capacity_);
}

CapacityInFilter::CapacityInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<Capacity>(values)
{}
	
bool CapacityInFilter::match(const OrderParams &params)const
{
	return filter(params.capacity_);
}

CurrencyEqualFilter::CurrencyEqualFilter(Currency v): impl_(v)
{}

bool CurrencyEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.currency_);
}

CurrencyInFilter::CurrencyInFilter(const ValuesT &values): 
	Impl::EnumInSetFilter<Currency>(values)
{}
	
bool CurrencyInFilter::match(const OrderParams &params)const
{
	return filter(params.currency_);
}

InstructionsEqualFilter::InstructionsEqualFilter(InstructionSetT v): impl_(v)
{}

bool InstructionsEqualFilter::match(const OrderParams &params)const
{
	return impl_.filter(params.execInstruct_);
}

InstructionsInFilter::InstructionsInFilter(const ValuesT &values): 
	Impl::EnumSetInSetFilter<Instructions>(values)
{}
	
bool InstructionsInFilter::match(const OrderParams &params)const
{
	return filter(params.execInstruct_);
}

DestinationFilter::DestinationFilter(Impl::StringFilter *flt):filter_(flt)
{}

bool DestinationFilter::match(const OrderParams &params)const{
	return filter_->filter(params.destination_.get());
}

SourceFilter::SourceFilter(Impl::StringFilter *flt):filter_(flt)
{}

bool SourceFilter::match(const OrderEntry &params)const{
	return filter_->filter(params.source_.get());
}

PxFilter::PxFilter(Impl::PriceFilter *flt):filter_(flt)
{}

bool PxFilter::match(const OrderParams &params)const{
	return filter_->filter(params.price_);
}

StopPxFilter::StopPxFilter(Impl::PriceFilter *flt):filter_(flt)
{}

bool StopPxFilter::match(const OrderParams &params)const{
	return filter_->filter(params.stopPx_);
}

AvgPxFilter::AvgPxFilter(Impl::PriceFilter *flt):filter_(flt)
{}

bool AvgPxFilter::match(const OrderParams &params)const{
	return filter_->filter(params.avgPx_);
}

DayAvgPxFilter::DayAvgPxFilter(Impl::PriceFilter *flt):filter_(flt)
{}

bool DayAvgPxFilter::match(const OrderParams &params)const{
	return filter_->filter(params.dayAvgPx_);
}

MinQtyFilter::MinQtyFilter(Impl::QuantityFilter *flt):filter_(flt)
{}

bool MinQtyFilter::match(const OrderParams &params)const{
	return filter_->filter(params.minQty_);
}

OrderQtyFilter::OrderQtyFilter(Impl::QuantityFilter *flt):filter_(flt)
{}

bool OrderQtyFilter::match(const OrderParams &params)const{
	return filter_->filter(params.orderQty_);
}

LeavesQtyFilter::LeavesQtyFilter(Impl::QuantityFilter *flt):filter_(flt)
{}

bool LeavesQtyFilter::match(const OrderParams &params)const{
	return filter_->filter(params.leavesQty_);
}

CumQtyFilter::CumQtyFilter(Impl::QuantityFilter *flt):filter_(flt)
{}

bool CumQtyFilter::match(const OrderParams &params)const{
	return filter_->filter(params.cumQty_);
}

DayOrderQtyFilter::DayOrderQtyFilter(Impl::QuantityFilter *flt):filter_(flt)
{}

bool DayOrderQtyFilter::match(const OrderParams &params)const{
	return filter_->filter(params.dayOrderQty_);
}

DayCumQtyFilter::DayCumQtyFilter(Impl::QuantityFilter *flt):filter_(flt)
{}

bool DayCumQtyFilter::match(const OrderParams &params)const{
	return filter_->filter(params.dayCumQty_);
}

ExpireTimeFilter::ExpireTimeFilter(Impl::DateTimeFilter *flt):filter_(flt)
{}

bool ExpireTimeFilter::match(const OrderParams &params)const{
	return filter_->filter(params.expireTime_);
}

SettlDateFilter::SettlDateFilter(Impl::DateTimeFilter *flt):filter_(flt)
{}

bool SettlDateFilter::match(const OrderParams &params)const{
	return filter_->filter(params.settlDate_);
}

CreationTimeFilter::CreationTimeFilter(Impl::DateTimeFilter *flt):filter_(flt)
{}

bool CreationTimeFilter::match(const OrderParams &params)const{
	return filter_->filter(params.creationTime_);
}

LastUpdateTimeFilter::LastUpdateTimeFilter(Impl::DateTimeFilter *flt):filter_(flt)
{}

bool LastUpdateTimeFilter::match(const OrderParams &params)const{
	return filter_->filter(params.lastUpdateTime_);
}



