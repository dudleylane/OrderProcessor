#pragma once

#include <functional>
#include <string>
#include <nlohmann/json.hpp>
#include "DataModelDef.h"
#include "OrderBookImpl.h"
#include "ClientMessageParser.h"
#include "QueuesDef.h"

namespace COP
{

namespace Store
{
class WideParamsDataStorage;
class OrderDataStorage;
} // namespace Store

namespace App
{

std::string serializeConnected();
std::string serializeInstrumentList(const Store::WideParamsDataStorage *wds);
/// Which accounts a session may see (#225); empty, every account
using AccountFilter = std::function<bool(const SourceIdT &account)>;
std::string serializeAccountList(const Store::WideParamsDataStorage *wds,
                                 const AccountFilter &maySee = AccountFilter());
std::string serializeOrderSnapshot(const Store::OrderDataStorage *storage,
                                   const AccountFilter &maySee = AccountFilter());
std::string serializeOrderUpdate(const OrderEntry &order);
std::string serializeExecReport(const ExecutionEntry *exec);
std::string serializeBookUpdate(const std::string &symbol, const BookSnapshot &snap);
std::string serializeCancelReject(u64 orderId, const std::string &reason);
/// Why a cancel was refused, in words for a cancel_reject's reason (#73)
std::string cancelRejectReason(const Queues::CancelRejectEvent &evnt);
std::string serializeBusinessReject(u64 refId, const std::string &reason);
std::string serializeError(const std::string &message);

struct SystemMetrics
{
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
    size_t storageMapSize; ///< LMDB's map in bytes, which doubles when a write finds it full (#148)
    size_t storageMapUsed; ///< bytes of the map in use: the high-water mark of its pages
    u64 timestamp;
};

std::string serializeMetricsUpdate(const SystemMetrics &metrics);

} // namespace App
} // namespace COP
