#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
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
/// Every order the session may see, as its last report left it (#404), but one whose first report isn't out
std::string serializeOrderSnapshot(const Store::OrderDataStorage *storage,
                                   const AccountFilter &maySee = AccountFilter());
/// The order as it is now. Tests call it: a report's update gives the report's figures (#411).
std::string serializeOrderUpdate(const OrderEntry &order);
/// The order with the figures a report gives it (#411), in place of its own
std::string serializeOrderUpdate(const OrderEntry &order, const OrderFigures &figures);
std::string serializeExecReport(const ExecutionEntry *exec);
/// The REJECT report of a new order or a replacement the engine refused without storing it (#67), sent at
/// transactTime: orderId 0, as no order was stored, and an execution id of its own (#545), with the ClOrdIDs the event
/// names (a replacement's original too) and the client's requestId when it gave one (#683)
std::string serializeUnstoredReject(const Queues::OrderRejectEvent &evnt, DateTimeT transactTime);
std::string serializeBookUpdate(const std::string &symbol, const BookSnapshot &snap);
std::string serializeCancelReject(u64 orderId, const std::string &reason);
/// Why a cancel was refused, in words for a cancel_reject's reason (#73)
std::string cancelRejectReason(const Queues::CancelRejectEvent &evnt);
/// An error answering a client's message; requestId, when the request gave one, names the request it answers (#683)
std::string serializeError(const std::string &message, const std::string &requestId = std::string());
/// The message with requestId added at its top level, beside its type, as an error's is: an order's first order_update
/// to the session that took the order's request, when the request named itself (#790). The message is an object this
/// serializer wrote.
std::string stampRequestId(const std::string &message, const std::string &requestId);
/// Where clOrdId lies in a message this serializer wrote as the value of its clOrderId member, an order_update's or an
/// unstored REJECT's: its offset and length, or {0, 0} when the message doesn't hold it so. A session finds the request
/// it kept for an order by these bytes of the message it is handed, with no copy of the ClOrdID (#790).
std::pair<std::size_t, std::size_t> locateClOrderId(const std::string &message, std::string_view clOrdId);

struct SystemMetrics
{
    u64 eventsCreated; ///< since the server started: 64-bit, where int32_t wrapped (#537)
    u64 eventsProcessed;
    u64 eventsFinished;
    u64 transactionsCreated;
    u64 transactionsProcessed;
    u64 transactionsFinished;
    u64 transactionsFailed; ///< transactions that failed and were rolled back, each logged at error level too (#514)
    int32_t availableEventProcessors;
    int32_t totalEventProcessors;
    int32_t availableTransactProcessors;
    int32_t totalTransactProcessors;
    u32 queueDepth;
    size_t pendingTransactions; ///< transactions not yet run: with queueDepth, what --max-in-flight bounds (#519)
    size_t poolSize;
    size_t poolCacheMisses; ///< acquires that found every slot in use
    size_t poolAllocations; ///< scopes the pool allocated: one per transaction, and one per miss (#515)
    size_t poolArenaSize;
    size_t activeSessions;
    size_t storedOrders;   ///< every order the server holds, open or closed: the store's size (#543)
    size_t storageMapSize; ///< LMDB's map in bytes, which doubles when a write finds it full (#148)
    size_t storageMapUsed; ///< bytes of the map in use: the high-water mark of its pages
    /// When this server started, in ms since the epoch: the same in every sample one start sends, so a client can tell
    /// a restarted server's first sample, its counters from 0 again, from a counter that went down (#677)
    u64 serverStart;
    u64 timestamp;
};

std::string serializeMetricsUpdate(const SystemMetrics &metrics);

} // namespace App
} // namespace COP
