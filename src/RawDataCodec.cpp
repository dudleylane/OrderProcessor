/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "RawDataCodec.h"
#include "StringTCodec.h"

using namespace std;
using namespace COP;
using namespace COP::Codec;

namespace{
	const int BUFFER_SIZE = 128;
}

/*	
	struct RawDataEntry{
		IdT id_;
		RawDataType type_;
		char *data_;
		u32 length_;

		RawDataEntry();
		RawDataEntry(RawDataType type, const char *data, u32 len);
	};
*/

RawDataCodec::RawDataCodec(void)
{
}

RawDataCodec::~RawDataCodec(void)
{
}

void RawDataCodec::encode(const RawDataEntry &val, std::string *buf, IdT *id, u32 *version)
{
	assert(NULL != buf);
	assert(NULL != id);
	assert(NULL != version);

	*id = val.id_;
	*version = 0;
	buf->reserve(buf->size() + BUFFER_SIZE);

	char typebuf[36];
	memcpy(typebuf, &(val.type_), sizeof(val.type_));
	buf->append(typebuf, sizeof(val.type_));
	buf->append(1, '.');

	memcpy(typebuf, &(val.length_), sizeof(val.length_));
	buf->append(typebuf, sizeof(val.length_));
	buf->append(1, '.');

	if(0 < val.length_)
		buf->append(val.data_, val.length_);
}

void RawDataCodec::decode(const IdT& id, u32 /*version*/, const char *buf, size_t size, RawDataEntry *val)
{
	assert(NULL != buf);
	assert(NULL != val);
	assert(0 < size);

	val->id_ = id;
	val->type_ = INVALID_RAWDATATYPE;
	if(size < sizeof(val->type_))
		throw std::exception("Invalid format of the encoded RawData - size less than required, unable decode type!");
	memcpy(&(val->type_), buf, sizeof(val->type_));
	const char *p = buf + sizeof(val->type_);
	if('.' != *p)
		throw std::exception("Invalid format of the encoded RawData - missed '.' after type!");
	++p;

	val->length_ = 0;
	if(size < (p - buf) + sizeof(val->length_))
		throw std::exception("Invalid format of the encoded RawData - size less than required, unable decode length!");
	memcpy(&(val->length_), p, sizeof(val->length_));
	p += sizeof(val->length_);
	if('.' != *p)
		throw std::exception("Invalid format of the encoded RawData - missed '.' after length!");
	++p;

	if(0 == val->length_)
		return;
	auto_ptr<char> arr(new char[val->length_]);
	memcpy(arr.get(), p, val->length_);
	val->data_ = arr.release();
}