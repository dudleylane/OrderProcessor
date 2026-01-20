/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"
#include "DataModelDef.h"

namespace COP{
	namespace Codec{

class OrderCodec
{
public:
	OrderCodec(void);
	~OrderCodec(void);

public:
	static void encode(const OrderEntry &val, std::string *buf, IdT *id, u32 *version);
	static char *encode(const OrderEntry &val, char *buf, IdT *id, u32 *version);
	static OrderEntry *decode(const IdT& id, u32 version, const char *buf, size_t size);
};

	}
}