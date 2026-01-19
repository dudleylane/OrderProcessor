/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "EntryFilter.h"

#include <cassert>

using namespace COP;
using namespace COP::SubscrMgr;



InstrumentIdFilter::InstrumentIdFilter(Impl::IdTFilter *filter): impl_(*filter)
{}

bool InstrumentIdFilter::match(const InstrumentEntry &params)const
{
	return impl_.match(params.id_);
}

bool InstrumentIdFilter::getInstrumentEntry(InstrumentEntry *val)const
{
	assert(NULL != val);
	return impl_.getVal(&(val->id_));
}

InstrumentSymbolFilter::InstrumentSymbolFilter(Impl::StringFilter *filter): filter_(filter)
{}

InstrumentSymbolFilter::~InstrumentSymbolFilter()
{
	delete filter_;
}

bool InstrumentSymbolFilter::match(const InstrumentEntry &params)const
{
	return filter_->filter(params.symbol_);
}

bool InstrumentSymbolFilter::getInstrumentEntry(InstrumentEntry *val)const
{
	assert(NULL != val);
	return filter_->getVal(&(val->symbol_));
}

InstrumentSecurityIdFilter::InstrumentSecurityIdFilter(Impl::StringFilter *filter): filter_(filter)
{}

bool InstrumentSecurityIdFilter::match(const InstrumentEntry &params)const
{
	return filter_->filter(params.securityId_);
}

bool InstrumentSecurityIdFilter::getInstrumentEntry(InstrumentEntry *val)const
{
	assert(NULL != val);
	return filter_->getVal(&(val->securityId_));
}

InstrumentSecurityIdSourceFilter::InstrumentSecurityIdSourceFilter(Impl::StringFilter *filter): filter_(filter)
{}

bool InstrumentSecurityIdSourceFilter::match(const InstrumentEntry &params)const
{
	return filter_->filter(params.securityIdSource_);
}

bool InstrumentSecurityIdSourceFilter::getInstrumentEntry(InstrumentEntry *val)const
{
	assert(NULL != val);
	return filter_->getVal(&(val->securityIdSource_));
}

void InstrumentFilter::release()
{
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		if(NULL != *it)
			delete *it;
	}
	filters_.clear();
}

bool InstrumentFilter::getInstrumentEntry(InstrumentEntry *val)const
{
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		assert(NULL != *it);
		if(!(*it)->getInstrumentEntry(val))
			return false;
	}	
	return !filters_.empty();
}

void InstrumentFilter::addFilter(InstrumentElementFilter *fltr)
{
	assert(NULL != fltr);
	filters_.push_back(fltr);
}

bool InstrumentFilter::match(const OrderParams &params)const
{
	if(filters_.empty())
		return true;
	const InstrumentEntry &entry = params.instrument_.get();
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		assert(NULL != *it);
		if(!(*it)->match(entry))
			return false;
	}
	return true;
}

AccountIdFilter::AccountIdFilter(Impl::IdTFilter *filter): impl_(*filter)
{}

bool AccountIdFilter::match(const AccountEntry &params)const
{
	return impl_.match(params.id_);
}

AccountAccountFilter::AccountAccountFilter(Impl::StringFilter *filter): filter_(filter)
{}

bool AccountAccountFilter::match(const AccountEntry &params)const
{
	return filter_->filter(params.account_);
}

AccountFirmFilter::AccountFirmFilter(Impl::StringFilter *filter): filter_(filter)
{}

bool AccountFirmFilter::match(const AccountEntry &params)const
{
	return filter_->filter(params.firm_);
}

AccountTypeEqualFilter::AccountTypeEqualFilter(AccountType value): impl_(value)
{}

bool AccountTypeEqualFilter::match(const AccountEntry &params)const
{
	return impl_.filter(params.type_);
}

AccountTypeInFilter::AccountTypeInFilter(const ValuesT &values): Impl::EnumInSetFilter<AccountType>(values)
{}

bool AccountTypeInFilter::match(const AccountEntry &params)const
{
	return filter(params.type_);
}

void AccountFilter::addFilter(AccountElementFilter *fltr)
{
	assert(NULL != fltr);
	filters_.push_back(fltr);
}

void AccountFilter::release()
{
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		if(NULL != *it)
			delete *it;
	}
	filters_.clear();
}

bool AccountFilter::match(const OrderParams &params)const
{
	if(filters_.empty())
		return true;
	const AccountEntry &entry = params.account_.get();
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		assert(NULL != *it);
		if(!(*it)->match(entry))
			return false;
	}
	return true;
}

ClearingIdFilter::ClearingIdFilter(Impl::IdTFilter *filter): impl_(*filter)
{}

bool ClearingIdFilter::match(const ClearingEntry &params)const
{
	return impl_.match(params.id_);
}

ClearingFirmFilter::ClearingFirmFilter(Impl::StringFilter *filter): filter_(filter)
{}

bool ClearingFirmFilter::match(const ClearingEntry &params)const
{
	return filter_->filter(params.firm_);
}

void ClearingFilter::addFilter(ClearingElementFilter *fltr)
{
	assert(NULL != fltr);
	filters_.push_back(fltr);
}

void ClearingFilter::release()
{
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		if(NULL != *it)
			delete *it;
	}
	filters_.clear();
}

bool ClearingFilter::match(const OrderParams &params)const
{
	if(filters_.empty())
		return true;
	const ClearingEntry &entry = params.clearing_.get();
	for(FiltersT::const_iterator it = filters_.begin(); it != filters_.end(); ++it){
		assert(NULL != *it);
		if(!(*it)->match(entry))
			return false;
	}
	return true;
}

