/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"
#include "DataModelDef.h"

namespace COP
{
namespace Codec
{

class AccountCodec
{
public:
    AccountCodec(void);
    ~AccountCodec(void);

public:
    /// The layout encode() writes, carried in the record's first 4 bytes (#149). decode() reads it and every older
    /// one: to change the layout, bump this, keep decoding the old version, and add golden records for the new one.
    static constexpr u32 FORMAT_VERSION = 0;

    static void encode(const AccountEntry &val, std::string *buf, IdT *id, u32 *version);
    static void decode(const IdT &id, u32 formatVersion, const char *buf, size_t size, AccountEntry *val);
};

} // namespace Codec
} // namespace COP