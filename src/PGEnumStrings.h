/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "DataModelDef.h"

namespace COP::PG {

constexpr const char* toSQL(Side v) {
    switch (v) {
        case BUY_SIDE:        return "BUY";
        case SELL_SIDE:       return "SELL";
        case BUY_MINUS_SIDE:  return "BUY_MINUS";
        case SELL_PLUS_SIDE:  return "SELL_PLUS";
        case SELL_SHORT_SIDE: return "SELL_SHORT";
        case CROSS_SIDE:      return "CROSS";
        default:              return nullptr;
    }
}

constexpr const char* toSQL(OrderType v) {
    switch (v) {
        case MARKET_ORDERTYPE:    return "MARKET";
        case LIMIT_ORDERTYPE:     return "LIMIT";
        case STOP_ORDERTYPE:      return "STOP";
        case STOPLIMIT_ORDERTYPE: return "STOPLIMIT";
        default:                  return nullptr;
    }
}

constexpr const char* toSQL(OrderStatus v) {
    switch (v) {
        case RECEIVEDNEW_ORDSTATUS:    return "RECEIVED_NEW";
        case REJECTED_ORDSTATUS:       return "REJECTED";
        case PENDINGNEW_ORDSTATUS:     return "PENDING_NEW";
        case PENDINGREPLACE_ORDSTATUS: return "PENDING_REPLACE";
        case NEW_ORDSTATUS:            return "NEW";
        case PARTFILL_ORDSTATUS:       return "PARTIAL_FILL";
        case FILLED_ORDSTATUS:         return "FILLED";
        case EXPIRED_ORDSTATUS:        return "EXPIRED";
        case DFD_ORDSTATUS:            return "DONE_FOR_DAY";
        case SUSPENDED_ORDSTATUS:      return "SUSPENDED";
        case REPLACED_ORDSTATUS:       return "REPLACED";
        case CANCELED_ORDSTATUS:       return "CANCELED";
        default:                       return nullptr;
    }
}

constexpr const char* toSQL(TimeInForce v) {
    switch (v) {
        case DAY_TIF:     return "DAY";
        case GTD_TIF:     return "GTD";
        case GTC_TIF:     return "GTC";
        case FOK_TIF:     return "FOK";
        case IOC_TIF:     return "IOC";
        case OPG_TIF:     return "OPG";
        case ATCLOSE_TIF: return "ATCLOSE";
        default:          return nullptr;
    }
}

constexpr const char* toSQL(Capacity v) {
    switch (v) {
        case AGENCY_CAPACITY:                    return "AGENCY";
        case PRINCIPAL_CAPACITY:                 return "PRINCIPAL";
        case PROPRIETARY_CAPACITY:               return "PROPRIETARY";
        case INDIVIDUAL_CAPACITY:                return "INDIVIDUAL";
        case RISKLESS_PRINCIPAL_CAPACITY:         return "RISKLESS_PRINCIPAL";
        case AGENT_FOR_ANOTHER_MEMBER_CAPACITY:  return "AGENT_FOR_ANOTHER_MEMBER";
        default:                                 return nullptr;
    }
}

constexpr const char* toSQL(Currency v) {
    switch (v) {
        case USD_CURRENCY: return "USD";
        case EUR_CURRENCY: return "EUR";
        default:           return nullptr;
    }
}

constexpr const char* toSQL(SettlTypeBase v) {
    switch (v) {
        case _0_SETTLTYPE:    return "REGULAR";
        case _1_SETTLTYPE:    return "CASH";
        case _2_SETTLTYPE:    return "NEXT_DAY";
        case _3_SETTLTYPE:    return "T_PLUS_2";
        case _4_SETTLTYPE:    return "T_PLUS_3";
        case _5_SETTLTYPE:    return "T_PLUS_4";
        case _6_SETTLTYPE:    return "T_PLUS_5";
        case _7_SETTLTYPE:    return "SELLERS_OPTION";
        case _8_SETTLTYPE:    return "WHEN_ISSUED";
        case _9_SETTLTYPE:    return "T_PLUS_1";
        case _B_SETTLTYPE:    return "BUYERS_OPTION";
        case _C_SETTLTYPE:    return "SPECIAL_TRADE";
        case TENOR_SETTLTYPE: return "TENOR";
        default:              return nullptr;
    }
}

constexpr const char* toSQL(AccountType v) {
    switch (v) {
        case PRINCIPAL_ACCOUNTTYPE: return "PRINCIPAL";
        case AGENCY_ACCOUNTTYPE:    return "AGENCY";
        default:                    return nullptr;
    }
}

} // namespace COP::PG
