/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <set>
#include <string>
#include <deque>
#include <regex>
#include "TypesDef.h"

namespace COP{ namespace Impl{

template<typename T> 
class EnumEqualFilter{
public:
	EnumEqualFilter(const T &val): val_(val){}

	bool filter(const T &val)const{return val == val_;}
private:
	T val_;
};

template<typename T> 
class EnumInSetFilter{
public:
	typedef std::set<T> ValuesT;

	EnumInSetFilter(const ValuesT &val): val_(val){}

	bool filter(const T &val)const{return val_.end() != val_.find(val);}
private:
	ValuesT val_;
};

template<typename T> 
class EnumSetEqualFilter{
public:
	typedef std::set<T> ValuesT;
	EnumSetEqualFilter(const ValuesT &val): val_(val){}

	bool filter(const ValuesT &val)const{return val == val_;}
private:
	ValuesT val_;
};

template<typename T> 
class EnumSetInSetFilter{
public:
	typedef std::set<T> ValuesT;

	EnumSetInSetFilter(const ValuesT &val): val_(val){}

	bool filter(const ValuesT &val)const{
		for(ValuesT::const_iterator it = val.begin(); it != val.end(); ++it)
			if(val_.end() == val_.find(*it))
				return false;
		return true;
	}
private:
	ValuesT val_;
};

class StringFilter{
public:
	virtual ~StringFilter(){};
	virtual bool filter(const StringT &val)const = 0;
	virtual bool getVal(StringT *val)const = 0;
};

class StringEqualFilter: public StringFilter{
public:
	StringEqualFilter(const StringT &val);
	virtual bool filter(const StringT &val)const;
	virtual bool getVal(StringT *val)const;
private:
	StringT val_;
};

class StringInSetFilter: public StringFilter{
public:
	typedef std::set<StringT> ValuesT;

	StringInSetFilter(const ValuesT &val);
	virtual bool filter(const StringT &val)const;
	virtual bool getVal(StringT *val)const;
private:
	ValuesT val_;
};

class StringMatchFilter: public StringFilter{
public:
	StringMatchFilter(const std::string &pattern);
	virtual bool filter(const StringT &val)const;
	virtual bool getVal(StringT *val)const;
private:
	std::string pattern_;
	std::regex regex_;
};

class DateTimeFilter{
public:
	virtual ~DateTimeFilter(){};
	virtual bool filter(const DateTimeT &val)const = 0;
};

class DateTimeEqualFilter: public DateTimeFilter{
public:
	DateTimeEqualFilter(const DateTimeT &val);
	virtual bool filter(const DateTimeT &val)const;
private:
	DateTimeT val_;
};

class DateTimeLessFilter: public DateTimeFilter{
public:
	DateTimeLessFilter(const DateTimeT &val);
	virtual bool filter(const DateTimeT &val)const;
private:
	DateTimeT val_;
};

class DateTimeGreaterFilter: public DateTimeFilter{
public:
	DateTimeGreaterFilter(const DateTimeT &val);
	virtual bool filter(const DateTimeT &val)const;
private:
	DateTimeT val_;
};

class DateTimeLessEqualFilter: public DateTimeFilter{
public:
	DateTimeLessEqualFilter(const DateTimeT &val);
	virtual bool filter(const DateTimeT &val)const;
private:
	DateTimeT val_;
};

class DateTimeGreaterEqualFilter: public DateTimeFilter{
public:
	DateTimeGreaterEqualFilter(const DateTimeT &val);
	virtual bool filter(const DateTimeT &val)const;
private:
	DateTimeT val_;
};

class DateTimeInRangeFilter: public DateTimeFilter{
public:
	DateTimeInRangeFilter(const DateTimeT &begin, const DateTimeT &end);
	virtual bool filter(const DateTimeT &val)const;
private:
	DateTimeT begin_;
	DateTimeT end_;
};

class PriceFilter{
public:
	virtual ~PriceFilter(){};
	virtual bool filter(const PriceT &val)const = 0;
};

class PriceEqualFilter: public PriceFilter{
public:
	PriceEqualFilter(const PriceT &val);
	virtual bool filter(const PriceT &val)const;
private:
	PriceT val_;
};

class PriceLessFilter: public PriceFilter{
public:
	PriceLessFilter(const PriceT &val);
	virtual bool filter(const PriceT &val)const;
private:
	PriceT val_;
};

class PriceGreaterFilter: public PriceFilter{
public:
	PriceGreaterFilter(const PriceT &val);
	virtual bool filter(const PriceT &val)const;
private:
	PriceT val_;
};

class PriceLessEqualFilter: public PriceFilter{
public:
	PriceLessEqualFilter(const PriceT &val);
	virtual bool filter(const PriceT &val)const;
private:
	PriceT val_;
};

class PriceGreaterEqualFilter: public PriceFilter{
public:
	PriceGreaterEqualFilter(const PriceT &val);
	virtual bool filter(const PriceT &val)const;
private:
	PriceT val_;
};

class PriceInRangeFilter: public PriceFilter{
public:
	PriceInRangeFilter(const PriceT &begin, const PriceT &end);
	virtual bool filter(const PriceT &val)const;
private:
	PriceT begin_;
	PriceT end_;
};

class IdTDateFilter{
public:
	virtual ~IdTDateFilter(){};
	virtual bool filter(const u32 &val)const = 0;
	virtual bool getVal(u32 *params)const = 0;
};

class IdTDateEqualFilter: public IdTDateFilter{
public:
	IdTDateEqualFilter(const u32 &val);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	u32 val_;
};

class IdTDateLessFilter: public IdTDateFilter{
public:
	IdTDateLessFilter(const u32 &val);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	u32 val_;
};

class IdTDateGreaterFilter: public IdTDateFilter{
public:
	IdTDateGreaterFilter(const u32 &val);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	u32 val_;
};

class IdTDateLessEqualFilter: public IdTDateFilter{
public:
	IdTDateLessEqualFilter(const u32 &val);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	u32 val_;
};

class IdTDateGreaterEqualFilter: public IdTDateFilter{
public:
	IdTDateGreaterEqualFilter(const u32 &val);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	u32 val_;
};

class IdTDateInRangeFilter: public IdTDateFilter{
public:
	IdTDateInRangeFilter(const u32 &begin, const u32 &end);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	u32 begin_;
	u32 end_;
};

class IdTDateMatchFilter: public IdTDateFilter{
public:
	IdTDateMatchFilter(const std::string &pattern);
	virtual bool filter(const u32 &val)const;
	virtual bool getVal(u32 *params)const;
private:
	std::string pattern_;
};

class IdTIdEqualFilter{
public:
	IdTIdEqualFilter(const IdT &val);
	bool filter(const IdT &val)const;
	bool getVal(IdT *val)const;
private:
	IdT val_;
};

class U64EqualFilter{
public:
	U64EqualFilter(const u64 &val);
	bool filter(const u64 &val)const;
private:
	u64 val_;
};

class U64LessFilter{
public:
	U64LessFilter(const u64 &val);
	bool filter(const u64 &val)const;
private:
	u64 val_;
};

class U64GreaterFilter{
public:
	U64GreaterFilter(const u64 &val);
	bool filter(const u64 &val)const;
private:
	u64 val_;
};

class U64LessEqualFilter{
public:
	U64LessEqualFilter(const u64 &val);
	bool filter(const u64 &val)const;
private:
	u64 val_;
};

class U64GreaterEqualFilter{
public:
	U64GreaterEqualFilter(const u64 &val);
	bool filter(const u64 &val)const;
private:
	u64 val_;
};

class U64InRangeFilter{
public:
	U64InRangeFilter(const u64 &begin, const u64 &end);
	bool filter(const u64 &val)const;
private:
	u64 begin_;
	u64 end_;
};

	class IdTFilter{
	public:
		IdTFilter();
		void setIdFilter(IdTIdEqualFilter *fltr);
		void addFilter(IdTDateFilter *);
		bool match(const IdT &params)const;
		bool getVal(IdT *params)const;
	private:
		typedef std::deque<IdTDateFilter *> FiltersT;
		FiltersT dateFilters_;//u32 date_;
		IdTIdEqualFilter *idFilter_;//u64 id_;	
	};

class QuantityFilter{
public:
	virtual ~QuantityFilter(){};
	virtual bool filter(const QuantityT &val)const = 0;
};

class QuantityEqualFilter: public QuantityFilter{
public:
	QuantityEqualFilter(const QuantityT &val);
	virtual bool filter(const QuantityT &val)const;
private:
	QuantityT val_;
};

class QuantityLessFilter: public QuantityFilter{
public:
	QuantityLessFilter(const QuantityT &val);
	bool filter(const QuantityT &val)const;
private:
	QuantityT val_;
};

class QuantityGreaterFilter: public QuantityFilter{
public:
	QuantityGreaterFilter(const QuantityT &val);
	bool filter(const QuantityT &val)const;
private:
	QuantityT val_;
};

class QuantityLessEqualFilter: public QuantityFilter{
public:
	QuantityLessEqualFilter(const QuantityT &val);
	bool filter(const QuantityT &val)const;
private:
	QuantityT val_;
};

class QuantityGreaterEqualFilter: public QuantityFilter{
public:
	QuantityGreaterEqualFilter(const QuantityT &val);
	bool filter(const QuantityT &val)const;
private:
	QuantityT val_;
};

class QuantityInRangeFilter: public QuantityFilter{
public:
	QuantityInRangeFilter(const QuantityT &begin, const QuantityT &end);
	bool filter(const QuantityT &val)const;
private:
	QuantityT begin_;
	QuantityT end_;
};


}}

