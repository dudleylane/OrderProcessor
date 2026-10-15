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

class OrderCodec
{
public:
    OrderCodec(void);
    ~OrderCodec(void);

public:
    /// The layout encode() writes, carried in the record's first 4 bytes (#149). decode() reads it and every older
    /// one: to change the layout, bump this, keep decoding the old version, and add golden records for the new one.
    /// 1 since #172: the execution instructions follow the state machine's ids. 0 had no field for them.
    /// 2 since #223: whether a stop order's stop was reached follows them, a byte, 0 or 1. An order from version 0 or 1
    /// is decoded as not reached: a stop order from them is held, as its type asks, where a released STOP would trade
    /// at once as a market order.
    static constexpr u32 FORMAT_VERSION = 2;

    static void encode(const OrderEntry &val, std::string *buf, IdT *id, u32 *version);
    static char *encode(const OrderEntry &val, char *buf, IdT *id, u32 *version);
    static OrderEntry *decode(const IdT &id, u32 formatVersion, const char *buf, size_t size);
};

} // namespace Codec
} // namespace COP