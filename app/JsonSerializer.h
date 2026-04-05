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

struct SystemMetrics {
    int32_t eventsCreated;
    int32_t eventsProcessed;
    int32_t eventsFinished;
    int32_t transactionsCreated;
    int32_t transactionsProcessed;
    int32_t transactionsFinished;
    int32_t availableEventProcessors;
    int32_t totalEventProcessors;
    int32_t availableTransactProcessors;
    int32_t totalTransactProcessors;
    u32 queueDepth;
    size_t poolSize;
    size_t poolCacheMisses;
    size_t poolArenaSize;
    size_t activeSessions;
    size_t activeOrders;
    u64 timestamp;
};

std::string serializeMetricsUpdate(const SystemMetrics& metrics);

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
