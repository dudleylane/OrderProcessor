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

class StringTCodec
{
public:
	StringTCodec(void);
	~StringTCodec(void);

public:
	static void encode(const StringT &val, std::string *buf);
	static void decode(const char *buf, size_t size, StringT *val);

	static void serialize(const StringT &val, std::string *buf);
	static const char *restore(const char *buf, size_t size, StringT *val);

};

	}
}