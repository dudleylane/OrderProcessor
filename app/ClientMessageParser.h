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
    double price;
    unsigned int orderQty;
    TimeInForce tif;
    bool hasPrice;
    bool hasQty;
    bool hasTif;
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
