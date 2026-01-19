/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"
#include "DataModelDef.h"

namespace COP{
	namespace Codec{

class InstrumentCodec
{
public:
	InstrumentCodec(void);
	~InstrumentCodec(void);

public:
	static void encode(const InstrumentEntry &instr, std::string *buf, IdT *id, u32 *version);
	static void decode(const IdT& id, u32 version, const char *buf, size_t size, InstrumentEntry *instr);
};

	}
}