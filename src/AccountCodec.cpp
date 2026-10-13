/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "AccountCodec.h"
#include "StringTCodec.h"
#include "CodecChecks.h"

using namespace COP;
using namespace COP::Codec;

namespace
{
const int BUFFER_SIZE = 128;
// The record holds the type as its in-memory bytes (#149)
static_assert(sizeof(AccountType) == 4, "an account's type is stored as 4 bytes");
} // namespace

/*	
	struct AccountEntry{
		StringT account_;
		StringT firm_;
		IdT id_;
		AccountType type_;

		bool isValid(std::string *invalid)const;
	};
*/

AccountCodec::AccountCodec(void) {}

AccountCodec::~AccountCodec(void) {}

void AccountCodec::encode(const AccountEntry &val, std::string *buf, IdT *id, u32 *version)
{
    assert(nullptr != buf);
    assert(nullptr != id);
    assert(nullptr != version);

    *id = val.id_;
    *version = 0;
    buf->reserve(BUFFER_SIZE);
    StringTCodec::serialize(val.account_, buf);
    buf->append(1, '.');

    StringTCodec::serialize(val.firm_, buf);
    buf->append(1, '.');

    char typebuf[36];
    memcpy(typebuf, &(val.type_), sizeof(val.type_));
    buf->reserve(buf->size() + sizeof(val.type_));
    buf->append(typebuf, sizeof(val.type_));
}

void AccountCodec::decode(const IdT &id, u32 /*formatVersion*/, const char *buf, size_t size, AccountEntry *val)
{
    assert(nullptr != buf);
    assert(nullptr != val);
    expectNotEmpty(size, "Account");
    const char *const end = buf + size;

    val->id_ = id;
    const char *p = StringTCodec::restore(buf, size, &(val->account_));
    p = skipSeparator(p, end, "Invalid format of the encoded Account - missed '.' after account!");
    p = StringTCodec::restore(p, size - (p - buf), &(val->firm_));
    p = skipSeparator(p, end, "Invalid format of the encoded Account - missed '.' after firm!");

    val->type_ = INVALID_ACCOUNTTYPE;
    if (size < (p - buf) + sizeof(val->type_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded Account - size less than required, unable decode type!");
    }
    memcpy(&(val->type_), p, sizeof(val->type_));
    p += sizeof(val->type_);
    expectConsumed(p, end, "Account");
}