/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "InstrumentCodec.h"
#include "StringTCodec.h"

using namespace COP;
using namespace COP::Codec;

namespace{
	const int BUFFER_SIZE = 128;
}

/*	struct InstrumentEntry{
		StringT symbol_;
		StringT securityId_;
		StringT securityIdSource_;
		IdT id_;

		bool isValid(std::string *invalid)const;
	};*/

InstrumentCodec::InstrumentCodec(void)
{
}

InstrumentCodec::~InstrumentCodec(void)
{
}

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
}

void InstrumentCodec::decode(const IdT& id, u32 /*version*/, const char *buf, size_t size, InstrumentEntry *instr)
{
	assert(nullptr != buf);
	assert(nullptr != instr);
	assert(0 < size);

	instr->id_ = id;
	const char *p = StringTCodec::restore(buf, size, &(instr->symbol_));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded Instrument - missed '.' after symbol!");
	++p;
	p = StringTCodec::restore(p, size - (p - buf), &(instr->securityId_));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded Instrument - missed '.' after securityId!");
	++p;
	p = StringTCodec::restore(p, size - (p - buf), &(instr->securityIdSource_));
}