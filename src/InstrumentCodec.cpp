/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "InstrumentCodec.h"
#include "StringTCodec.h"
#include "CodecChecks.h"

using namespace COP;
using namespace COP::Codec;

// The record holds each enum's value as its 4 bytes, so the values are pinned (#150): renumbering or reordering an
// enumerator misreads every instrument already written. A new enumerator goes at the end, here too, since decode()
// refuses a value past the last one it knows. OrderCodec.cpp pins Currency's values, which orders store as well.
static_assert(sizeof(InstrumentType) == 4, "an instrument's type is stored as 4 bytes");
static_assert(sizeof(Currency) == 4, "an instrument's currencies are stored as 4 bytes");
static_assert((INVALID_INSTRUMENTTYPE == 0) && (EQUITY_INSTRUMENTTYPE == 1) && (FX_SPOT_INSTRUMENTTYPE == 2) &&
                  (FX_SWAP_INSTRUMENTTYPE == 3),
              "InstrumentType values are persisted (#172)");
static_assert(NZD_CURRENCY == 8, "the last Currency, the largest an instrument's record may hold (#172)");

namespace
{
const int BUFFER_SIZE = 128;

/// Format version 1's fields (#172): '.', then an enum's 4 bytes
template <typename E> void appendEnum(std::string *buf, E value)
{
    buf->append(1, '.');
    buf->append(reinterpret_cast<const char *>(&value), sizeof(value));
}

/// Reads an enum appendEnum() wrote, after its '.'. A value past `last` is one this build doesn't know, from a newer
/// build or not a record at all, and is refused rather than restored.
template <typename E>
const char *readEnum(const char *p, const char *end, E last, E *value, const char *missed, const char *invalid)
{
    static_assert(sizeof(E) == sizeof(u32), "an enum is stored as 4 bytes");
    p = skipSeparator(p, end, missed);
    u32 raw = 0;
    if (static_cast<size_t>(end - p) < sizeof(raw))
    {
        throw std::runtime_error(invalid);
    }
    std::memcpy(&raw, p, sizeof(raw));
    if (static_cast<u32>(last) < raw)
    {
        throw std::runtime_error(invalid);
    }
    *value = static_cast<E>(raw);
    return p + sizeof(raw);
}
} // namespace

/*	struct InstrumentEntry{
		StringT symbol_;
		StringT securityId_;
		StringT securityIdSource_;
		IdT id_;
		InstrumentType instrumentType_;
		Currency baseCurrency_;
		Currency termCurrency_;

		bool isValid(std::string *invalid)const;
	};*/

InstrumentCodec::InstrumentCodec(void) {}

InstrumentCodec::~InstrumentCodec(void) {}

void InstrumentCodec::encode(const InstrumentEntry &instr, std::string *buf, IdT *id, u32 *version)
{
    assert(nullptr != buf);
    assert(nullptr != id);
    assert(nullptr != version);

    *id = instr.id_;
    *version = 0;
    buf->reserve(BUFFER_SIZE);
    StringTCodec::serialize(instr.symbol_, buf);
    buf->append(1, '.');
    StringTCodec::serialize(instr.securityId_, buf);
    buf->append(1, '.');
    StringTCodec::serialize(instr.securityIdSource_, buf);
    // Format version 1 (#172)
    appendEnum(buf, instr.instrumentType_);
    appendEnum(buf, instr.baseCurrency_);
    appendEnum(buf, instr.termCurrency_);
}

void InstrumentCodec::decode(const IdT &id, u32 formatVersion, const char *buf, size_t size, InstrumentEntry *instr)
{
    assert(nullptr != buf);
    assert(nullptr != instr);
    expectNotEmpty(size, "Instrument");
    const char *const end = buf + size;

    instr->id_ = id;
    const char *p = StringTCodec::restore(buf, size, &(instr->symbol_));
    p = skipSeparator(p, end, "Invalid format of the encoded Instrument - missed '.' after symbol!");
    p = StringTCodec::restore(p, size - (p - buf), &(instr->securityId_));
    p = skipSeparator(p, end, "Invalid format of the encoded Instrument - missed '.' after securityId!");
    p = StringTCodec::restore(p, size - (p - buf), &(instr->securityIdSource_));
    // Format version 1 (#172): the type, and an FX pair's base and term currencies. A version 0 record has none of
    // them, so the instrument comes back without them, as it was written.
    instr->instrumentType_ = INVALID_INSTRUMENTTYPE;
    instr->baseCurrency_ = INVALID_CURRENCY;
    instr->termCurrency_ = INVALID_CURRENCY;
    if (1 <= formatVersion)
    {
        p = readEnum(p, end, FX_SWAP_INSTRUMENTTYPE, &(instr->instrumentType_),
                     "Invalid format of the encoded Instrument - missed '.' before instrumentType!",
                     "Invalid format of the encoded Instrument - instrumentType cut short or unknown!");
        p = readEnum(p, end, NZD_CURRENCY, &(instr->baseCurrency_),
                     "Invalid format of the encoded Instrument - missed '.' before baseCurrency!",
                     "Invalid format of the encoded Instrument - baseCurrency cut short or unknown!");
        p = readEnum(p, end, NZD_CURRENCY, &(instr->termCurrency_),
                     "Invalid format of the encoded Instrument - missed '.' before termCurrency!",
                     "Invalid format of the encoded Instrument - termCurrency cut short or unknown!");
    }
    expectConsumed(p, end, "Instrument");
}