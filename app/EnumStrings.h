#pragma once

#include <string_view>
#include <stdexcept>
#include "DataModelDef.h"

namespace COP {

inline std::string_view toJsonString(Side s) {
    if (s == BUY_SIDE)        return "BUY";
    if (s == SELL_SIDE)       return "SELL";
    if (s == BUY_MINUS_SIDE)  return "BUY_MINUS";
    if (s == SELL_PLUS_SIDE)  return "SELL_PLUS";
    if (s == SELL_SHORT_SIDE) return "SELL_SHORT";
    if (s == CROSS_SIDE)      return "CROSS";
    return "INVALID";
}

inline Side sideFromJson(std::string_view sv) {
    if (sv == "BUY")        return BUY_SIDE;
    if (sv == "SELL")       return SELL_SIDE;
    if (sv == "BUY_MINUS")  return BUY_MINUS_SIDE;
    if (sv == "SELL_PLUS")  return SELL_PLUS_SIDE;
    if (sv == "SELL_SHORT") return SELL_SHORT_SIDE;
    if (sv == "CROSS")      return CROSS_SIDE;
    return INVALID_SIDE;
}

inline std::string_view toJsonString(OrderType t) {
    if (t == MARKET_ORDERTYPE)    return "MARKET";
    if (t == LIMIT_ORDERTYPE)     return "LIMIT";
    if (t == STOP_ORDERTYPE)      return "STOP";
    if (t == STOPLIMIT_ORDERTYPE) return "STOPLIMIT";
    return "INVALID";
}

inline OrderType orderTypeFromJson(std::string_view sv) {
    if (sv == "MARKET")    return MARKET_ORDERTYPE;
    if (sv == "LIMIT")     return LIMIT_ORDERTYPE;
    if (sv == "STOP")      return STOP_ORDERTYPE;
    if (sv == "STOPLIMIT") return STOPLIMIT_ORDERTYPE;
    return INVALID_ORDERTYPE;
}

inline std::string_view toJsonString(TimeInForce t) {
    if (t == DAY_TIF)     return "DAY";
    if (t == GTD_TIF)     return "GTD";
    if (t == GTC_TIF)     return "GTC";
    if (t == FOK_TIF)     return "FOK";
    if (t == IOC_TIF)     return "IOC";
    if (t == OPG_TIF)     return "OPG";
    if (t == ATCLOSE_TIF) return "ATCLOSE";
    return "INVALID";
}

inline TimeInForce tifFromJson(std::string_view sv) {
    if (sv == "DAY")     return DAY_TIF;
    if (sv == "GTD")     return GTD_TIF;
    if (sv == "GTC")     return GTC_TIF;
    if (sv == "FOK")     return FOK_TIF;
    if (sv == "IOC")     return IOC_TIF;
    if (sv == "OPG")     return OPG_TIF;
    if (sv == "ATCLOSE") return ATCLOSE_TIF;
    return INVALID_TIF;
}

inline std::string_view toJsonString(Currency c) {
    if (c == USD_CURRENCY) return "USD";
    if (c == EUR_CURRENCY) return "EUR";
    return "INVALID";
}

inline Currency currencyFromJson(std::string_view sv) {
    if (sv == "USD") return USD_CURRENCY;
    if (sv == "EUR") return EUR_CURRENCY;
    return INVALID_CURRENCY;
}

inline std::string_view toJsonString(Capacity c) {
    if (c == AGENCY_CAPACITY)                    return "AGENCY";
    if (c == PRINCIPAL_CAPACITY)                 return "PRINCIPAL";
    if (c == PROPRIETARY_CAPACITY)               return "PROPRIETARY";
    if (c == INDIVIDUAL_CAPACITY)                return "INDIVIDUAL";
    if (c == RISKLESS_PRINCIPAL_CAPACITY)         return "RISKLESS_PRINCIPAL";
    if (c == AGENT_FOR_ANOTHER_MEMBER_CAPACITY)  return "AGENT_FOR_ANOTHER_MEMBER";
    return "INVALID";
}

inline Capacity capacityFromJson(std::string_view sv) {
    if (sv == "AGENCY")                    return AGENCY_CAPACITY;
    if (sv == "PRINCIPAL")                 return PRINCIPAL_CAPACITY;
    if (sv == "PROPRIETARY")               return PROPRIETARY_CAPACITY;
    if (sv == "INDIVIDUAL")                return INDIVIDUAL_CAPACITY;
    if (sv == "RISKLESS_PRINCIPAL")         return RISKLESS_PRINCIPAL_CAPACITY;
    if (sv == "AGENT_FOR_ANOTHER_MEMBER")  return AGENT_FOR_ANOTHER_MEMBER_CAPACITY;
    return INVALID_CAPACITY;
}

inline std::string_view toJsonString(OrderStatus s) {
    if (s == RECEIVEDNEW_ORDSTATUS)    return "RECEIVED_NEW";
    if (s == REJECTED_ORDSTATUS)       return "REJECTED";
    if (s == PENDINGNEW_ORDSTATUS)     return "PENDING_NEW";
    if (s == PENDINGREPLACE_ORDSTATUS) return "PENDING_REPLACE";
    if (s == NEW_ORDSTATUS)            return "NEW";
    if (s == PARTFILL_ORDSTATUS)       return "PARTIAL_FILL";
    if (s == FILLED_ORDSTATUS)         return "FILLED";
    if (s == EXPIRED_ORDSTATUS)        return "EXPIRED";
    if (s == DFD_ORDSTATUS)            return "DONE_FOR_DAY";
    if (s == SUSPENDED_ORDSTATUS)      return "SUSPENDED";
    if (s == REPLACED_ORDSTATUS)       return "REPLACED";
    if (s == CANCELED_ORDSTATUS)       return "CANCELED";
    return "INVALID";
}

inline std::string_view toJsonString(ExecType t) {
    if (t == NEW_EXECTYPE)          return "NEW";
    if (t == TRADE_EXECTYPE)        return "TRADE";
    if (t == DFD_EXECTYPE)          return "DFD";
    if (t == CORRECT_EXECTYPE)      return "CORRECT";
    if (t == CANCEL_EXECTYPE)       return "CANCEL";
    if (t == REJECT_EXECTYPE)       return "REJECT";
    if (t == REPLACE_EXECTYPE)      return "REPLACE";
    if (t == EXPIRED_EXECTYPE)      return "EXPIRED";
    if (t == SUSPENDED_EXECTYPE)    return "SUSPENDED";
    if (t == STATUS_EXECTYPE)       return "STATUS";
    if (t == RESTATED_EXECTYPE)     return "RESTATED";
    if (t == PEND_CANCEL_EXECTYPE)  return "PEND_CANCEL";
    if (t == PEND_REPLACE_EXECTYPE) return "PEND_REPLACE";
    return "INVALID";
}

inline std::string_view toJsonString(AccountType t) {
    if (t == PRINCIPAL_ACCOUNTTYPE) return "PRINCIPAL";
    if (t == AGENCY_ACCOUNTTYPE)   return "AGENCY";
    return "INVALID";
}

}
