#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "DataModelDef.h"
#include "OrderBookImpl.h"

namespace COP {

namespace Store {
    class WideParamsDataStorage;
    class OrderDataStorage;
}

namespace App {

std::string serializeConnected();
std::string serializeInstrumentList(const Store::WideParamsDataStorage* wds);
std::string serializeAccountList(const Store::WideParamsDataStorage* wds);
std::string serializeOrderSnapshot(const Store::OrderDataStorage* storage);
std::string serializeOrderUpdate(const OrderEntry& order);
std::string serializeExecReport(const ExecutionEntry* exec);
std::string serializeBookUpdate(const std::string& symbol, const BookSnapshot& snap);
std::string serializeCancelReject(u64 orderId, const std::string& reason);
std::string serializeBusinessReject(u64 refId, const std::string& reason);
std::string serializeError(const std::string& message);

struct ParsedNewOrder {
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

struct ParsedCancelOrder {
    u64 orderId;
    std::string clOrderId;
};

struct ParsedReplaceOrder {
    u64 orderId;
    double price;
    unsigned int orderQty;
    TimeInForce tif;
    bool hasPrice;
    bool hasQty;
    bool hasTif;
};

struct ParsedClientMessage {
    std::string type;
    // For subscribe/unsubscribe
    std::string symbol;
    // Parsed data (only one is valid depending on type)
    ParsedNewOrder newOrder;
    ParsedCancelOrder cancelOrder;
    ParsedReplaceOrder replaceOrder;
};

ParsedClientMessage parseClientMessage(const std::string& json);

}}
