#include "JsonSerializer.h"
#include "EnumStrings.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::App;
using json = nlohmann::json;

namespace
{

// An order holds an unset id for each reference it does not have: a new order's origClOrderId, an account the
// client left out. Resolving an unset id throws, so these are read only when set (#35). Only clOrderId and the
// instrument always resolve: OrderDataStorage requires the first and the OrderEntry constructor loads the second.

/// Set means not the default id. Not IdT::isValid(): WideDataStorage gives string ids a zero date, which
/// isValid() rejects, so a set source or destination would read as unset.
template <typename T> bool isSet(const WideDataLazyRef<T> &ref)
{
    return SourceIdT() != ref.getId();
}

std::string rawDataText(const WideDataLazyRef<RawDataEntry> &ref)
{
    if (!isSet(ref))
    {
        return std::string();
    }
    const RawDataEntry &raw = ref.get();
    return (raw.data_ && raw.length_ > 0) ? std::string(raw.data_, raw.length_) : std::string();
}

StringT stringOrEmpty(const WideDataLazyRef<StringT> &ref)
{
    return isSet(ref) ? ref.get() : StringT();
}

json orderToJson(const OrderEntry &order)
{
    json j;
    j["orderId"] = order.orderId_.id_;
    j["origOrderId"] = order.origOrderId_.id_;
    j["clOrderId"] = rawDataText(order.clOrderId_);
    j["origClOrderId"] = rawDataText(order.origClOrderId_);
    j["symbol"] = order.instrument_.get().symbol_;
    j["side"] = toJsonString(order.side_);
    j["ordType"] = toJsonString(order.ordType_);
    j["price"] = order.price_;
    j["stopPx"] = order.stopPx_;
    j["avgPx"] = order.avgPx_;
    j["orderQty"] = order.orderQty_;
    j["cumQty"] = order.cumQty_;
    j["leavesQty"] = order.leavesQty_;
    j["minQty"] = order.minQty_;
    j["status"] = toJsonString(order.status_);
    j["tif"] = toJsonString(order.tif_);
    j["capacity"] = toJsonString(order.capacity_);
    j["currency"] = toJsonString(order.currency_);
    j["account"] = isSet(order.account_) ? order.account_.get().account_ : StringT();
    j["destination"] = stringOrEmpty(order.destination_);
    j["source"] = stringOrEmpty(order.source_);
    j["creationTime"] = order.creationTime_;
    j["lastUpdateTime"] = order.lastUpdateTime_;
    j["expireTime"] = order.expireTime_;
    if (FXSWAP_ORDERTYPE == order.ordType_)
    {
        j["farPrice"] = order.farPrice_;
        j["farSettlDate"] = order.farSettlDate_;
        j["settlDate"] = order.settlDate_;
    }
    return j;
}

json execToJson(const ExecutionEntry *exec)
{
    json j;
    j["execId"] = exec->execId_.id_;
    j["orderId"] = exec->orderId_.id_;
    j["type"] = toJsonString(exec->type_);
    j["orderStatus"] = toJsonString(exec->orderStatus_);
    j["market"] = exec->market_;
    j["transactTime"] = exec->transactTime_;
    if (exec->execLegType_ != SINGLE_LEG)
    {
        j["execLegType"] = (exec->execLegType_ == NEAR_LEG) ? "NEAR" : "FAR";
    }

    // The extra fields follow the report's class, not type_: an order cancel is a plain ExecutionEntry with type CANCEL,
    // and a rejected replace is a RejectExecEntry with type REPLACE (#56)
    if (auto *trade = dynamic_cast<const TradeExecEntry *>(exec))
    {
        j["lastQty"] = trade->lastQty_;
        j["lastPx"] = trade->lastPx_;
        j["currency"] = toJsonString(trade->currency_);
        j["tradeDate"] = trade->tradeDate_;
    }
    else if (auto *reject = dynamic_cast<const RejectExecEntry *>(exec))
    {
        j["rejectReason"] = reject->rejectReason_;
    }
    else if (auto *replace = dynamic_cast<const ReplaceExecEntry *>(exec))
    {
        j["origOrderId"] = replace->origOrderId_.id_;
    }
    else if (auto *correct = dynamic_cast<const ExecCorrectExecEntry *>(exec))
    {
        j["cumQty"] = correct->cumQty_;
        j["leavesQty"] = correct->leavesQty_;
        j["lastQty"] = correct->lastQty_;
        j["lastPx"] = correct->lastPx_;
        j["currency"] = toJsonString(correct->currency_);
        j["tradeDate"] = correct->tradeDate_;
        j["origOrderId"] = correct->origOrderId_.id_;
        j["execRefId"] = correct->execRefId_.id_;
    }
    else if (auto *cancel = dynamic_cast<const TradeCancelExecEntry *>(exec))
    {
        j["execRefId"] = cancel->execRefId_.id_;
    }

    return j;
}

} // namespace

std::string App::serializeConnected()
{
    json j;
    j["type"] = "connected";
    return j.dump();
}

std::string App::serializeInstrumentList(const Store::WideParamsDataStorage *wds)
{
    json j;
    j["type"] = "instrument_list";
    j["data"] = json::array();
    wds->forEachInstrument(
        [&](const SourceIdT &id, const InstrumentEntry &instr)
        {
            json item;
            item["id"] = id.id_;
            item["symbol"] = instr.symbol_;
            item["securityId"] = instr.securityId_;
            item["securityIdSource"] = instr.securityIdSource_;
            j["data"].push_back(item);
        });
    return j.dump();
}

std::string App::serializeAccountList(const Store::WideParamsDataStorage *wds)
{
    json j;
    j["type"] = "account_list";
    j["data"] = json::array();
    wds->forEachAccount(
        [&](const SourceIdT &id, const AccountEntry &acct)
        {
            json item;
            item["id"] = id.id_;
            item["account"] = acct.account_;
            item["firm"] = acct.firm_;
            item["type"] = toJsonString(acct.type_);
            j["data"].push_back(item);
        });
    return j.dump();
}

std::string App::serializeOrderSnapshot(const Store::OrderDataStorage *storage)
{
    json j;
    j["type"] = "order_snapshot";
    j["data"] = json::array();
    storage->forEachOrder(
        [&](const IdT & /*id*/, const OrderEntry &order)
        {
            j["data"].push_back(orderToJson(order));
        });
    return j.dump();
}

std::string App::serializeOrderUpdate(const OrderEntry &order)
{
    json j;
    j["type"] = "order_update";
    j["data"] = orderToJson(order);
    return j.dump();
}

std::string App::serializeExecReport(const ExecutionEntry *exec)
{
    json j;
    j["type"] = "execution_report";
    j["data"] = execToJson(exec);
    return j.dump();
}

std::string App::serializeBookUpdate(const std::string &symbol, const BookSnapshot &snap)
{
    json j;
    j["type"] = "book_update";
    json data;
    data["symbol"] = symbol;
    data["bids"] = json::array();
    for (const auto &lvl : snap.bids)
    {
        data["bids"].push_back({ { "price", lvl.price }, { "qty", lvl.totalQty }, { "orderCount", lvl.orderCount } });
    }
    data["asks"] = json::array();
    for (const auto &lvl : snap.asks)
    {
        data["asks"].push_back({ { "price", lvl.price }, { "qty", lvl.totalQty }, { "orderCount", lvl.orderCount } });
    }
    data["timestamp"] = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    j["data"] = data;
    return j.dump();
}

std::string App::serializeCancelReject(u64 orderId, const std::string &reason)
{
    json j;
    j["type"] = "cancel_reject";
    j["data"] = { { "orderId", orderId }, { "reason", reason } };
    return j.dump();
}

std::string App::cancelRejectReason(const Queues::CancelRejectEvent &evnt)
{
    switch (evnt.reason_)
    {
    case Queues::CancelRejectEvent::UNKNOWN_ORDER:
        return "Cancel rejected: unknown order";
    case Queues::CancelRejectEvent::TOO_LATE:
        return "Cancel rejected: too late, the order is " + std::string(toJsonString(evnt.ordStatus_));
    case Queues::CancelRejectEvent::PENDING:
        return "Cancel rejected: a replace of the order is pending";
    default:
        return "Cancel rejected";
    }
}

std::string App::serializeBusinessReject(u64 refId, const std::string &reason)
{
    json j;
    j["type"] = "business_reject";
    j["data"] = { { "refId", refId }, { "reason", reason } };
    return j.dump();
}

std::string App::serializeError(const std::string &message)
{
    json j;
    j["type"] = "error";
    j["message"] = message;
    return j.dump();
}

std::string App::serializeMetricsUpdate(const SystemMetrics &m)
{
    json j;
    j["type"] = "metrics_update";
    json data;
    data["eventsCreated"] = m.eventsCreated;
    data["eventsProcessed"] = m.eventsProcessed;
    data["eventsFinished"] = m.eventsFinished;
    data["transactionsCreated"] = m.transactionsCreated;
    data["transactionsProcessed"] = m.transactionsProcessed;
    data["transactionsFinished"] = m.transactionsFinished;
    data["availableEventProcessors"] = m.availableEventProcessors;
    data["totalEventProcessors"] = m.totalEventProcessors;
    data["availableTransactProcessors"] = m.availableTransactProcessors;
    data["totalTransactProcessors"] = m.totalTransactProcessors;
    data["queueDepth"] = m.queueDepth;
    data["poolSize"] = m.poolSize;
    data["poolCacheMisses"] = m.poolCacheMisses;
    data["poolArenaSize"] = m.poolArenaSize;
    data["activeSessions"] = m.activeSessions;
    data["activeOrders"] = m.activeOrders;
    data["timestamp"] = m.timestamp;
    j["data"] = data;
    return j.dump();
}
