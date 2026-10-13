/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "RawDataCodec.h"
#include "StringTCodec.h"
#include "CodecChecks.h"

using namespace std;
using namespace COP;
using namespace COP::Codec;

namespace
{
const int BUFFER_SIZE = 128;
// The record holds these as their in-memory bytes (#149)
static_assert(sizeof(RawDataType) == 4, "raw data's type is stored as 4 bytes");
static_assert(sizeof(RawDataEntry::length_) == 4, "raw data's length is stored as 4 bytes");
// The type is stored as its value, so the values are pinned too (#150)
static_assert((INVALID_RAWDATATYPE == 0) && (STRING_RAWDATATYPE == 1) && (MESSAGE_RAWDATATYPE == 2) &&
                  (XML_RAWDATATYPE == 3) && (BINARY_RAWDATATYPE == 4),
              "RawDataType values are persisted");
} // namespace

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

RawDataCodec::RawDataCodec(void) {}

RawDataCodec::~RawDataCodec(void) {}

void RawDataCodec::encode(const RawDataEntry &val, std::string *buf, IdT *id, u32 *version)
{
    assert(nullptr != buf);
    assert(nullptr != id);
    assert(nullptr != version);

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

    if (0 < val.length_)
    {
        buf->append(val.data_, val.length_);
    }
}

void RawDataCodec::decode(const IdT &id, u32 /*formatVersion*/, const char *buf, size_t size, RawDataEntry *val)
{
    assert(nullptr != buf);
    assert(nullptr != val);
    expectNotEmpty(size, "RawData");
    const char *const end = buf + size;

    val->id_ = id;
    val->type_ = INVALID_RAWDATATYPE;
    if (size < sizeof(val->type_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded RawData - size less than required, unable decode type!");
    }
    memcpy(&(val->type_), buf, sizeof(val->type_));
    const char *p = buf + sizeof(val->type_);
    p = skipSeparator(p, end, "Invalid format of the encoded RawData - missed '.' after type!");

    val->length_ = 0;
    if (size < (p - buf) + sizeof(val->length_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded RawData - size less than required, unable decode length!");
    }
    memcpy(&(val->length_), p, sizeof(val->length_));
    p += sizeof(val->length_);
    p = skipSeparator(p, end, "Invalid format of the encoded RawData - missed '.' after length!");

    if (0 == val->length_)
    {
        expectConsumed(p, end, "RawData");
        return;
    }
    if (size < static_cast<size_t>(p - buf) + val->length_)
    {
        throw std::runtime_error(
            "Invalid format of the encoded RawData - size less than required, unable decode data!");
    }
    std::unique_ptr<char[]> arr(new char[val->length_]);
    memcpy(arr.get(), p, val->length_);
    expectConsumed(p + val->length_, end, "RawData");
    val->data_ = arr.release();
}