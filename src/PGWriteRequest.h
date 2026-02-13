/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <variant>
#include "TypesDef.h"

namespace COP::PG {

struct InstrumentWrite {
    std::string symbol;
    std::string securityId;
    std::string securityIdSource;
};

struct AccountWrite {
    std::string account;
    std::string firm;
    std::string type;  // PG enum string
};

struct ClearingWrite {
    std::string firm;
};

struct OrderWrite {
    u64 orderId;
    u32 orderDate;
    std::string clOrderId;
    std::string origClOrderId;
    std::string source;
    std::string destination;
    std::string instrumentSymbol;
    std::string accountName;
    std::string clearingFirm;

    std::string side;       // PG enum string
    std::string ordType;    // PG enum string
    std::string status;     // PG enum string
    std::string tif;        // PG enum string
    std::string capacity;   // PG enum string
    std::string currency;   // PG enum string
    std::string settlType;  // PG enum string

    double price;
    double stopPx;
    double avgPx;
    double dayAvgPx;

    unsigned int minQty;
    unsigned int orderQty;
    unsigned int leavesQty;
    unsigned int cumQty;
    unsigned int dayOrderQty;
    unsigned int dayCumQty;

    u64 expireTime;
    u64 settlDate;
};

using PGWriteRequest = std::variant<InstrumentWrite, AccountWrite, ClearingWrite, OrderWrite>;

} // namespace COP::PG
