/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "FilterImpl.h"

#include <cassert>
 
#include <boost/regex.hpp>

using namespace COP;
using namespace COP::Impl;

StringEqualFilter::StringEqualFilter(const StringT &val):val_(val)
{}

bool StringEqualFilter::filter(const StringT &val)const
{
/*	if(val.length() == val_.length()){
		return 0 == memcmp(val.c_str(), val_.c_str(), val_.length());
	}
	return false;*/
	return val == val_;
}

bool StringEqualFilter::getVal(StringT *val)const
{
	assert(NULL != val);
	*val = val_;
	return true;
}

StringInSetFilter::StringInSetFilter(const ValuesT &val):val_(val)
{}

bool StringInSetFilter::filter(const StringT &val)const
{
	return val_.end() != val_.find(val);
}

bool StringInSetFilter::getVal(StringT *)const
{
	return false;
}

StringMatchFilter::StringMatchFilter(const std::string &val):pattern_(val), regex_(pattern_)
{}

bool StringMatchFilter::filter(const StringT &val)const
{
	return boost::regex_match( val, regex_/*boost::regex(pattern_)*/);
}

bool StringMatchFilter::getVal(StringT *)const
{
	return false;
}

DateTimeEqualFilter::DateTimeEqualFilter(const DateTimeT &val):val_(val)
{}

bool DateTimeEqualFilter::filter(const DateTimeT &val)const
{
	return val == val_;
}

DateTimeLessFilter::DateTimeLessFilter(const DateTimeT &val):val_(val)
{}

bool DateTimeLessFilter::filter(const DateTimeT &val)const
{
	return val < val_;
}

DateTimeGreaterFilter::DateTimeGreaterFilter(const DateTimeT &val):val_(val)
{}

bool DateTimeGreaterFilter::filter(const DateTimeT &val)const
{
	return val > val_;
}

DateTimeLessEqualFilter::DateTimeLessEqualFilter(const DateTimeT &val):val_(val)
{}

bool DateTimeLessEqualFilter::filter(const DateTimeT &val)const
{
	return val <= val_;
}

DateTimeGreaterEqualFilter::DateTimeGreaterEqualFilter(const DateTimeT &val):val_(val)
{}

bool DateTimeGreaterEqualFilter::filter(const DateTimeT &val)const
{
	return val >= val_;
}

DateTimeInRangeFilter::DateTimeInRangeFilter(const DateTimeT &begin, const DateTimeT &end):
	begin_(begin), end_(end)
{}

bool DateTimeInRangeFilter::filter(const DateTimeT &val)const
{
	return (val <= end_)&&(val >= begin_);
}

PriceEqualFilter::PriceEqualFilter(const PriceT &val):val_(val)
{}

bool PriceEqualFilter::filter(const PriceT &val)const
{
	return val == val_;// todo
}

PriceLessFilter::PriceLessFilter(const PriceT &val):val_(val)
{}

bool PriceLessFilter::filter(const PriceT &val)const
{
	return val < val_;// todo
}

PriceGreaterFilter::PriceGreaterFilter(const PriceT &val):val_(val)
{}

bool PriceGreaterFilter::filter(const PriceT &val)const
{
	return val > val_;//todo
}

PriceLessEqualFilter::PriceLessEqualFilter(const PriceT &val):val_(val)
{}

bool PriceLessEqualFilter::filter(const PriceT &val)const
{
	return val <= val_;//todo
}

PriceGreaterEqualFilter::PriceGreaterEqualFilter(const PriceT &val):val_(val)
{}

bool PriceGreaterEqualFilter::filter(const PriceT &val)const
{
	return val >= val_;//todo
}

PriceInRangeFilter::PriceInRangeFilter(const PriceT &begin, const PriceT &end):
	begin_(begin), end_(end)
{}

bool PriceInRangeFilter::filter(const PriceT &val)const
{
	return (val <= end_)&&(val >= begin_);//todo
}

IdTDateEqualFilter::IdTDateEqualFilter(const u32 &val):val_(val)
{}

bool IdTDateEqualFilter::filter(const u32 &val)const
{
	return val == val_;
}

bool IdTDateEqualFilter::getVal(u32 *val)const
{
	assert(NULL != val);
	*val = val_;
	return true;
}

IdTDateLessFilter::IdTDateLessFilter(const u32 &val):val_(val)
{}

bool IdTDateLessFilter::filter(const u32 &val)const
{
	return val < val_;
}

bool IdTDateLessFilter::getVal(u32 *)const
{
	return false;
}

IdTDateGreaterFilter::IdTDateGreaterFilter(const u32 &val):val_(val)
{}

bool IdTDateGreaterFilter::filter(const u32 &val)const
{
	return val > val_;
}

bool IdTDateGreaterFilter::getVal(u32 *)const
{
	return false;
}

IdTDateLessEqualFilter::IdTDateLessEqualFilter(const u32 &val):val_(val)
{}

bool IdTDateLessEqualFilter::filter(const u32 &val)const
{
	return val <= val_;
}

bool IdTDateLessEqualFilter::getVal(u32 *)const
{
	return false;
}

IdTDateGreaterEqualFilter::IdTDateGreaterEqualFilter(const u32 &val):val_(val)
{}

bool IdTDateGreaterEqualFilter::filter(const u32 &val)const
{
	return val >= val_;
}

bool IdTDateGreaterEqualFilter::getVal(u32 *)const
{
	return false;
}

IdTDateInRangeFilter::IdTDateInRangeFilter(const u32 &begin, const u32 &end):
	begin_(begin), end_(end)
{}

bool IdTDateInRangeFilter::filter(const u32 &val)const
{
	return (val <= end_)&&(val >= begin_);
}

bool IdTDateInRangeFilter::getVal(u32 *)const
{
	return false;
}

IdTDateMatchFilter::IdTDateMatchFilter(const std::string &val):pattern_(val)
{}

bool IdTDateMatchFilter::filter(const u32 &)const
{
	return false; //todo
}

bool IdTDateMatchFilter::getVal(u32 *)const
{
	return false;
}

IdTIdEqualFilter::IdTIdEqualFilter(const IdT &val):val_(val)
{}

bool IdTIdEqualFilter::filter(const IdT &val)const
{
	return val == val_;
}

bool IdTIdEqualFilter::getVal(IdT *val)const
{
	assert(NULL != val);
	*val = val_;
	return true;
}

QuantityEqualFilter::QuantityEqualFilter(const QuantityT &val):val_(val)
{}

bool QuantityEqualFilter::filter(const QuantityT &val)const
{
	return val == val_;
}

QuantityLessFilter::QuantityLessFilter(const QuantityT &val):val_(val)
{}

bool QuantityLessFilter::filter(const QuantityT &val)const
{
	return val < val_;
}

QuantityGreaterFilter::QuantityGreaterFilter(const QuantityT &val):val_(val)
{}

bool QuantityGreaterFilter::filter(const QuantityT &val)const
{
	return val > val_;
}

QuantityLessEqualFilter::QuantityLessEqualFilter(const QuantityT &val):val_(val)
{}

bool QuantityLessEqualFilter::filter(const QuantityT &val)const
{
	return val <= val_;
}

QuantityGreaterEqualFilter::QuantityGreaterEqualFilter(const QuantityT &val):val_(val)
{}

bool QuantityGreaterEqualFilter::filter(const QuantityT &val)const
{
	return val >= val_;
}

QuantityInRangeFilter::QuantityInRangeFilter(const QuantityT &begin, const QuantityT &end):
	begin_(begin), end_(end)
{}

bool QuantityInRangeFilter::filter(const QuantityT &val)const
{
	return (val <= end_)&&(val >= begin_);
}

IdTFilter::IdTFilter():idFilter_(NULL)
{}

void IdTFilter::setIdFilter(IdTIdEqualFilter *fltr)
{
	assert(NULL == idFilter_);
	idFilter_ = fltr;
}

void IdTFilter::addFilter(IdTDateFilter *fltr)
{
	dateFilters_.push_back(fltr);
}

bool IdTFilter::match(const IdT &params)const
{
	if((NULL != idFilter_)&& (!idFilter_->filter(params)))
		return false;
	for(FiltersT::const_iterator it = dateFilters_.begin(); it != dateFilters_.end(); ++it){
		assert(NULL != *it);
		if(!(*it)->filter(params.date_))
			return false;
	}
	return true;
}

bool IdTFilter::getVal(IdT *params)const
{
	if((NULL != idFilter_)&& (!idFilter_->getVal(params)))
		return false;

	if((1 == dateFilters_.size())&&(*(dateFilters_.begin()))->getVal(&(params->date_)))
		return true;
	return false;
}



