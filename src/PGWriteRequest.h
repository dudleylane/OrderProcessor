/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <variant>
#include "TypesDef.h"

namespace COP::PG
{

struct InstrumentWrite
{
    std::string symbol;
    std::string securityId;
    std::string securityIdSource;
};

struct AccountWrite
{
    std::string account;
    std::string firm;
    std::string type; // PG enum string
};

struct ClearingWrite
{
    std::string firm;
};

struct OrderWrite
{
    u64 orderId;
    u32 orderDate; // IdT::date_: Unix seconds when the id was drawn; the row's order_date is its day in UTC (#151)
    std::string clOrderId;
    std::string origClOrderId;
    std::string source;
    std::string destination;
    std::string instrumentSymbol;
    std::string accountName;
    std::string clearingFirm;

    std::string side;      // PG enum string
    std::string ordType;   // PG enum string
    std::string status;    // PG enum string
    std::string tif;       // PG enum string
    std::string capacity;  // PG enum string
    std::string currency;  // PG enum string
    std::string settlType; // PG enum string

    double price;
    double stopPx;
    double avgPx;
    double dayAvgPx;
    bool stopTriggered = false; // a stop order's stop was reached, and the order released (#236)

    unsigned int minQty;
    unsigned int orderQty;
    unsigned int leavesQty;
    unsigned int cumQty;
    unsigned int dayOrderQty;
    unsigned int dayCumQty;

    u64 expireTime;   // DateTimeT: milliseconds since the epoch, 0 for none
    u64 settlDate;    // DateTimeT: milliseconds since the epoch at midnight UTC of the day, 0 for none
    u64 dayStart = 0; // DateTimeT: when the business day the day figures are for began (#244), 0 for none

    // An FX swap's far leg, 0 for any other order (#151's FX pick): its price, and its settlDate
    double farPrice = 0.0;
    u64 farSettlDate = 0;
};

/// Removes an order's row: a rollback erased the only version LMDB had of it (#151)
struct DeleteOrderWrite
{
    u64 orderId;
    u32 orderDate; // as OrderWrite's
};

/// A write for PGWriteBehind. Each one sets a whole row or deletes one, so writing it twice leaves what writing it once
/// does: the writer writes a batch again when its connection is lost, and a commit cut off may have landed. A write
/// that adds to what is there, such as a row per execution, would need a key that makes a second write a no-op.
using PGWriteRequest = std::variant<InstrumentWrite, AccountWrite, ClearingWrite, OrderWrite, DeleteOrderWrite>;

} // namespace COP::PG
