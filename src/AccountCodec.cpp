/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "AccountCodec.h"
#include "StringTCodec.h"

using namespace COP;
using namespace COP::Codec;

namespace{
	const int BUFFER_SIZE = 128;
}

/*	
	struct AccountEntry{
		StringT account_;
		StringT firm_;
		IdT id_;
		AccountType type_;

		bool isValid(std::string *invalid)const;
	};
*/

AccountCodec::AccountCodec(void)
{
}

AccountCodec::~AccountCodec(void)
{
}

void AccountCodec::encode(const AccountEntry &val, std::string *buf, IdT *id, u32 *version)
{
	assert(NULL != buf);
	assert(NULL != id);
	assert(NULL != version);

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

void AccountCodec::decode(const IdT& id, u32 /*version*/, const char *buf, size_t size, AccountEntry *val)
{
	assert(NULL != buf);
	assert(NULL != val);
	assert(0 < size);

	val->id_ = id;
	const char *p = StringTCodec::restore(buf, size, &(val->account_));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded Account - missed '.' after account!");
	++p;
	p = StringTCodec::restore(p, size - (p - buf), &(val->firm_));
	if('.' != *p)
		throw std::runtime_error("Invalid format of the encoded Account - missed '.' after firm!");
	++p;

	val->type_ = INVALID_ACCOUNTTYPE;
	if(size < (p - buf) + sizeof(val->type_))
		throw std::runtime_error("Invalid format of the encoded Account - size less than required, unable decode type!");
	memcpy(&(val->type_), p, sizeof(val->type_));
}