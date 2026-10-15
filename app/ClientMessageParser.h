// Parser for the JSON messages clients send over the WebSocket.
// Deliberately free of engine dependencies (enums and nlohmann::json only) so it
// builds and fuzzes on its own: see test/fuzz/fuzz_client_message.cpp.

#pragma once

#include <string>
#include "DataModelDef.h"

namespace COP
{
namespace App
{

struct ParsedNewOrder
{
    std::string symbol;
    Side side;
    OrderType ordType;
    double price;
    double stopPx;
    unsigned int orderQty;
    unsigned int minQty;
    TimeInForce tif;
    u64 expireTime; // when a GTD order expires, in milliseconds since the epoch (#240); 0 when not given
    std::string account;
    Currency currency;
    Capacity capacity;
};

struct ParsedCancelOrder
{
    u64 orderId;
    std::string clOrderId;
};

struct ParsedReplaceOrder
{
    u64 orderId;
    std::string clOrderId; // optional; names the order when given, as for a cancel (#58, #74)
    double price;
    double stopPx; // moves a held stop order's stop (#238)
    unsigned int orderQty;
    TimeInForce tif;
    u64 expireTime; // a GTD order's new expiry (#240)
    bool hasPrice;
    bool hasStopPx;
    bool hasQty;
    bool hasTif;
    bool hasExpireTime;
};

struct ParsedClientMessage
{
    std::string type;
    // Why the message was rejected; set only when type == "error"
    std::string error;
    // For subscribe/unsubscribe
    std::string symbol;
    // Parsed data (only one is valid depending on type)
    ParsedNewOrder newOrder;
    ParsedCancelOrder cancelOrder;
    ParsedReplaceOrder replaceOrder;
};

/// Never throws. Malformed or mistyped JSON, or a numeric field outside its type's range, yields
/// type == "error" with the reason in error.
ParsedClientMessage parseClientMessage(const std::string &json);

} // namespace App
} // namespace COP
