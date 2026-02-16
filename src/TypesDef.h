/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <deque>
#include <functional>
#include <cstdint>

namespace COP{

    using i32 = std::int32_t;
    using i64 = std::int64_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

	struct IdT{
		u64 id_;
		u32 date_;

		constexpr IdT(): id_(0), date_(0){}
		IdT(const u64 &id, const u32 &date);

		constexpr bool isValid()const{return (0 != date_)&&(0 != id_);}
		void reset();
		constexpr void clear(){id_ = 0; date_ = 0;}
		void toString(std::string &msg)const;
		void serialize(std::string &msg)const;
		char *serialize(char *buf)const;
		const char *restore(const char *buf, size_t size);
	};
	constexpr bool operator < (const IdT &lft, const IdT &rght){
		if (lft.id_ == rght.id_)
			return lft.date_ < rght.date_;
		else
			return lft.id_ < rght.id_;
	}

	constexpr bool operator == (const IdT &lft, const IdT &rght){
		return (lft.id_ == rght.id_)&&(lft.date_ == rght.date_);
	}

	constexpr bool operator != (const IdT &lft, const IdT &rght){
		return !operator==(lft, rght);
	}

	typedef u64 DateTimeT;

	typedef double PriceT;
	struct PriceTAscend {
		constexpr bool operator()(const PriceT& left, const PriceT& right) const {
			return left < right;
		}
	};
	struct PriceTDescend {
		constexpr bool operator()(const PriceT& left, const PriceT& right) const {
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