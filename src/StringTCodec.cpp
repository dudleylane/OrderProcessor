/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "StringTCodec.h"
#include "CodecChecks.h"

using namespace COP;
using namespace COP::Codec;

namespace
{
// Records hold a string as its length, a size_t, then its bytes; a different size_t would misread every one (#149)
static_assert(sizeof(size_t) == 8, "a string's length is stored as an 8-byte size_t");
} // namespace

/*	
	typedef std::string StringT;
*/

StringTCodec::StringTCodec(void) {}

StringTCodec::~StringTCodec(void) {}

void StringTCodec::serialize(const StringT &val, std::string *tgtbuf)
{
    char buf[36];
    size_t size = val.size();
    memcpy(buf, &size, sizeof(size));
    tgtbuf->reserve(tgtbuf->size() + size + sizeof(size));
    tgtbuf->append(buf, sizeof(size));
    if (0 < size)
    {
        tgtbuf->append(val.c_str(), size);
    }
}

const char *StringTCodec::restore(const char *buf, size_t size, StringT *val)
{
    assert(nullptr != val);
    assert(nullptr != buf);

    size_t valSize = 0;
    // Checked in every build, not only by an assert: a record cut short would otherwise be read past its end (#149)
    if (size < sizeof(valSize))
    {
        throw std::runtime_error("restore(StringT): unable to restore value, buffer too small for its length!");
    }
    memcpy(&valSize, buf, sizeof(valSize));
    if (valSize > size - sizeof(valSize))
    {
        throw std::runtime_error("restore(StringT): unable to restore value, buffer size too small!");
    }
    if (0 == valSize)
    {
        return buf + sizeof(valSize);
    }
    val->reserve(valSize);
    val->append(buf + sizeof(valSize), valSize);
    return buf + sizeof(valSize) + valSize;
}

void StringTCodec::encode(const StringT &val, std::string *buf)
{
    assert(nullptr != buf);
    serialize(val, buf);
}

void StringTCodec::decode(const char *buf, size_t size, StringT *val)
{
    assert(nullptr != buf);
    assert(nullptr != val);
    expectNotEmpty(size, "String");

    expectConsumed(restore(buf, size, val), buf + size, "String");
}
