/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "ClearingCodec.h"
#include "StringTCodec.h"

using namespace COP;
using namespace COP::Codec;

namespace{
	const int BUFFER_SIZE = 128;
}

/*	
	struct ClearingEntry{
		StringT firm_;
		IdT id_;

		bool isValid(std::string *invalid)const;
	};
*/

ClearingCodec::ClearingCodec(void)
{
}

ClearingCodec::~ClearingCodec(void)
{
}

void ClearingCodec::encode(const ClearingEntry &val, std::string *buf, IdT *id, u32 *version)
{
	assert(NULL != buf);
	assert(NULL != id);
	assert(NULL != version);

	*id = val.id_;
	*version = 0;
	buf->reserve(BUFFER_SIZE);
	StringTCodec::serialize(val.firm_, buf);
}

void ClearingCodec::decode(const IdT& id, u32 /*version*/, const char *buf, size_t size, ClearingEntry *val)
{
	assert(NULL != buf);
	assert(NULL != val);
	assert(0 < size);

	val->id_ = id;
	/*const char *p = */StringTCodec::restore(buf, size, &(val->firm_));
}