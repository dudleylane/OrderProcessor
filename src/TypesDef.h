/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <deque>
#include <functional>

namespace COP{

    typedef signed      __int32 i32;
    typedef signed      __int64 i64;
	typedef unsigned    __int32 u32;
    typedef unsigned    __int64 u64;

	struct IdT{
		u64 id_;
		u32 date_;

		IdT(): id_(0), date_(0){}
		IdT(const u64 &id, const u32 &date);

		inline bool isValid()const{return (0 != date_)&&(0 != id_);}
		void reset();
		inline void clear(){reset();}
		void toString(std::string &msg)const;
		void serialize(std::string &msg)const;
		char *serialize(char *buf)const;
		const char *restore(const char *buf, size_t size);
	};
	inline bool operator < (const IdT &lft, const IdT &rght){
		if (lft.id_ == rght.id_)
			return lft.date_ < rght.date_;
		else
			return lft.id_ < rght.id_;
	}

	inline bool operator == (const IdT &lft, const IdT &rght){
		return (lft.id_ == rght.id_)&&(lft.date_ == rght.date_);
	}

	inline bool operator != (const IdT &lft, const IdT &rght){
		return !operator==(lft, rght);
	}

	typedef u64 DateTimeT;

	typedef double PriceT;
	struct PriceTAscend : public std::binary_function <PriceT, PriceT, bool> 
	{
		bool operator()(const PriceT& left, const PriceT& right) const{
			return left < right;
		}
	};
	struct PriceTDescend : public std::binary_function <PriceT, PriceT, bool> 
	{
		bool operator()(const PriceT& left, const PriceT& right) const{
			return left > right;
		}
	};


	typedef unsigned int QuantityT;

	typedef IdT SourceIdT;

	typedef IdT SubscriberIdT;

	typedef std::string StringT;

	enum RawDataType{
		INVALID_RAWDATATYPE = 0,
		STRING_RAWDATATYPE,
		MESSAGE_RAWDATATYPE,
		XML_RAWDATATYPE,
		BINARY_RAWDATATYPE
	};

	struct RawDataEntry{
		IdT id_;
		RawDataType type_;
		char *data_;
		u32 length_;

		RawDataEntry();
		RawDataEntry(RawDataType type, const char *data, u32 len);
	};
	bool operator < (const RawDataEntry &lft, const RawDataEntry &rght);

	struct EventData{
		IdT eventId_;

		explicit EventData(const IdT &eventId);
	};

	typedef std::deque<EventData> ExecutionsT;

}