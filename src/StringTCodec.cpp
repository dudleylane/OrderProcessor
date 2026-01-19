/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "StringTCodec.h"

using namespace COP;
using namespace COP::Codec;

namespace{
}

/*	
	typedef std::string StringT;
*/

StringTCodec::StringTCodec(void)
{
}

StringTCodec::~StringTCodec(void)
{
}

void StringTCodec::serialize(const StringT &val, std::string *tgtbuf)
{
	char buf[36];
	size_t size = val.size();
	memcpy(buf, &size, sizeof(size));
	tgtbuf->reserve(tgtbuf->size() + size + sizeof(size));
	tgtbuf->append(buf, sizeof(size));
	if(0 < size)
		tgtbuf->append(val.c_str(), size);
}

const char *StringTCodec::restore(const char *buf, size_t size, StringT *val)
{
	assert(NULL != val);
	assert(NULL != buf);

	size_t valSize = 0;
	assert(sizeof(valSize) <= size);
	memcpy(&valSize, buf, sizeof(valSize));
	if(valSize > size - sizeof(valSize))
		throw std::exception("restore(StringT): unable to restore value, buffer size too small!");
	if(0 == valSize)
		return buf + sizeof(valSize);
	val->reserve(valSize);
	val->append(buf + sizeof(valSize), valSize);
	return buf + sizeof(valSize) + valSize;
}

void StringTCodec::encode(const StringT &val, std::string *buf)
{
	assert(NULL != buf);
	serialize(val, buf);
}

void StringTCodec::decode(const char *buf, size_t size, StringT *val)
{
	assert(NULL != buf);
	assert(NULL != val);
	assert(0 < size);

	restore(buf, size, val);
}
