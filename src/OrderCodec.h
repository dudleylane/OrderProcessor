/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <optional>
#include "TypesDef.h"
#include "DataModelDef.h"

namespace COP
{
namespace Codec
{

/// The strings an order's record carries since format version 3 (#192): the values of the entries its ClOrdID, its
/// original's ClOrdID, its source and its destination name. Carried, they need no records, and no commits, of their
/// own. A field whose value the record doesn't carry is restored from a record of its own, as before version 3. A value
/// is carried only for a field whose id is set; its id is the field's.
/// Whether an order field names an entry, so that its record may carry its value (#192). An unset field's id is all
/// zero. A string entry's id has date 0, which IdT::isValid() refuses, so isValid() would leave out every source and
/// destination.
inline bool namesEntry(const SourceIdT &id)
{
    return 0 != id.id_;
}

struct OrderStrings
{
    std::optional<RawDataEntry> clOrderId;
    std::optional<RawDataEntry> origClOrderId;
    std::optional<StringT> source;
    std::optional<StringT> destination;
};

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
    /// 3 since #192: the order's strings follow (OrderStrings), each a byte, 0 or 1, saying whether the record carries
    /// it, and then, if it does, its value: a ClOrdID's type, length and bytes, a string's length and bytes.
    /// 4 since #244: '.' and the start of the business day the day figures are for (dayStart_) follow them. An order
    /// from an older version is decoded with 0, figures for no day: its first restatement begins them.
    static constexpr u32 FORMAT_VERSION = 4;

    /// Writes the order and the strings given. Throws std::invalid_argument for a string given for a field whose id
    /// isn't set: nothing could restore it.
    static void encode(const OrderEntry &val, std::string *buf, IdT *id, u32 *version,
                       const OrderStrings &strings = OrderStrings());
    static char *encode(const OrderEntry &val, char *buf, IdT *id, u32 *version,
                        const OrderStrings &strings = OrderStrings());
    /// Reads an order, and from format version 3 the strings its record carries into strings, if given
    static OrderEntry *decode(const IdT &id, u32 formatVersion, const char *buf, size_t size,
                              OrderStrings *strings = nullptr);
};

} // namespace Codec
} // namespace COP