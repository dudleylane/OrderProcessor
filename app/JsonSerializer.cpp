#include "JsonSerializer.h"
#include "EnumStrings.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::App;
using json = nlohmann::json;

namespace {

json orderToJson(const OrderEntry& order) {
    json j;
    j["orderId"] = order.orderId_.id_;
    j["origOrderId"] = order.origOrderId_.id_;

    // clOrderId from RawDataEntry
    const auto& clOrd = order.clOrderId_.get();
    if (clOrd.data_ && clOrd.length_ > 0)
        j["clOrderId"] = std::string(clOrd.data_, clOrd.length_);
    else
        j["clOrderId"] = "";

    const auto& origClOrd = order.origClOrderId_.get();
    if (origClOrd.data_ && origClOrd.length_ > 0)
        j["origClOrderId"] = std::string(origClOrd.data_, origClOrd.length_);
    else
        j["origClOrderId"] = "";

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
    j["account"] = order.account_.get().account_;
    j["destination"] = order.destination_.get();
    j["source"] = order.source_.get();
    j["creationTime"] = order.creationTime_;
    j["lastUpdateTime"] = order.lastUpdateTime_;
    j["expireTime"] = order.expireTime_;
    return j;
}

json execToJson(const ExecutionEntry* exec) {
    json j;
    j["execId"] = exec->execId_.id_;
    j["orderId"] = exec->orderId_.id_;
    j["type"] = toJsonString(exec->type_);
    j["orderStatus"] = toJsonString(exec->orderStatus_);
    j["market"] = exec->market_;
    j["transactTime"] = exec->transactTime_;

    if (exec->type_ == TRADE_EXECTYPE) {
        auto* trade = static_cast<const TradeExecEntry*>(exec);
        j["lastQty"] = trade->lastQty_;
        j["lastPx"] = trade->lastPx_;
        j["currency"] = toJsonString(trade->currency_);
        j["tradeDate"] = trade->tradeDate_;
    } else if (exec->type_ == REJECT_EXECTYPE) {
        auto* reject = static_cast<const RejectExecEntry*>(exec);
        j["rejectReason"] = reject->rejectReason_;
    } else if (exec->type_ == REPLACE_EXECTYPE) {
        auto* replace = static_cast<const ReplaceExecEntry*>(exec);
        j["origOrderId"] = replace->origOrderId_.id_;
    } else if (exec->type_ == CORRECT_EXECTYPE) {
        auto* correct = static_cast<const ExecCorrectExecEntry*>(exec);
        j["cumQty"] = correct->cumQty_;
        j["leavesQty"] = correct->leavesQty_;
        j["lastQty"] = correct->lastQty_;
        j["lastPx"] = correct->lastPx_;
        j["currency"] = toJsonString(correct->currency_);
        j["tradeDate"] = correct->tradeDate_;
        j["origOrderId"] = correct->origOrderId_.id_;
        j["execRefId"] = correct->execRefId_.id_;
    } else if (exec->type_ == CANCEL_EXECTYPE) {
        auto* cancel = static_cast<const TradeCancelExecEntry*>(exec);
        j["execRefId"] = cancel->execRefId_.id_;
    }

    return j;
}

}

std::string App::serializeConnected() {
    json j;
    j["type"] = "connected";
    return j.dump();
}

std::string App::serializeInstrumentList(const Store::WideParamsDataStorage* wds) {
    json j;
    j["type"] = "instrument_list";
    j["data"] = json::array();
    wds->forEachInstrument([&](const SourceIdT& id, const InstrumentEntry& instr) {
        json item;
        item["id"] = id.id_;
        item["symbol"] = instr.symbol_;
        item["securityId"] = instr.securityId_;
        item["securityIdSource"] = instr.securityIdSource_;
        j["data"].push_back(item);
    });
    return j.dump();
}

std::string App::serializeAccountList(const Store::WideParamsDataStorage* wds) {
    json j;
    j["type"] = "account_list";
    j["data"] = json::array();
    wds->forEachAccount([&](const SourceIdT& id, const AccountEntry& acct) {
        json item;
        item["id"] = id.id_;
        item["account"] = acct.account_;
        item["firm"] = acct.firm_;
        item["type"] = toJsonString(acct.type_);
        j["data"].push_back(item);
    });
    return j.dump();
}

std::string App::serializeOrderSnapshot(const Store::OrderDataStorage* storage) {
    json j;
    j["type"] = "order_snapshot";
    j["data"] = json::array();
    storage->forEachOrder([&](const IdT& /*id*/, const OrderEntry& order) {
        j["data"].push_back(orderToJson(order));
    });
    return j.dump();
}

std::string App::serializeOrderUpdate(const OrderEntry& order) {
    json j;
    j["type"] = "order_update";
    j["data"] = orderToJson(order);
    return j.dump();
}

std::string App::serializeExecReport(const ExecutionEntry* exec) {
    json j;
    j["type"] = "execution_report";
    j["data"] = execToJson(exec);
    return j.dump();
}

std::string App::serializeBookUpdate(const std::string& symbol, const BookSnapshot& snap) {
    json j;
    j["type"] = "book_update";
    json data;
    data["symbol"] = symbol;
    data["bids"] = json::array();
    for (const auto& lvl : snap.bids) {
        data["bids"].push_back({{"price", lvl.price}, {"qty", lvl.totalQty}, {"orderCount", lvl.orderCount}});
    }
    data["asks"] = json::array();
    for (const auto& lvl : snap.asks) {
        data["asks"].push_back({{"price", lvl.price}, {"qty", lvl.totalQty}, {"orderCount", lvl.orderCount}});
    }
    data["timestamp"] = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    j["data"] = data;
    return j.dump();
}

std::string App::serializeCancelReject(u64 orderId, const std::string& reason) {
    json j;
    j["type"] = "cancel_reject";
    j["data"] = {{"orderId", orderId}, {"reason", reason}};
    return j.dump();
}

std::string App::serializeBusinessReject(u64 refId, const std::string& reason) {
    json j;
    j["type"] = "business_reject";
    j["data"] = {{"refId", refId}, {"reason", reason}};
    return j.dump();
}

std::string App::serializeError(const std::string& message) {
    json j;
    j["type"] = "error";
    j["message"] = message;
    return j.dump();
}

ParsedClientMessage App::parseClientMessage(const std::string& jsonStr) {
    ParsedClientMessage msg;
    try {
        auto j = json::parse(jsonStr);
        msg.type = j.value("type", "");

        if (msg.type == "new_order") {
            auto d = j["data"];
            msg.newOrder.symbol = d.value("symbol", "");
            msg.newOrder.side = sideFromJson(d.value("side", ""));
            msg.newOrder.ordType = orderTypeFromJson(d.value("ordType", ""));
            msg.newOrder.price = d.value("price", 0.0);
            msg.newOrder.stopPx = d.value("stopPx", 0.0);
            msg.newOrder.orderQty = d.value("orderQty", 0u);
            msg.newOrder.minQty = d.value("minQty", 0u);
            msg.newOrder.tif = tifFromJson(d.value("tif", ""));
            msg.newOrder.account = d.value("account", "");
            msg.newOrder.currency = currencyFromJson(d.value("currency", ""));
            msg.newOrder.capacity = capacityFromJson(d.value("capacity", ""));
        } else if (msg.type == "cancel_order") {
            auto d = j["data"];
            msg.cancelOrder.orderId = d.value("orderId", (u64)0);
            msg.cancelOrder.clOrderId = d.value("clOrderId", "");
        } else if (msg.type == "replace_order") {
            auto d = j["data"];
            msg.replaceOrder.orderId = d.value("orderId", (u64)0);
            msg.replaceOrder.hasPrice = d.contains("price");
            msg.replaceOrder.hasQty = d.contains("orderQty");
            msg.replaceOrder.hasTif = d.contains("tif");
            msg.replaceOrder.price = d.value("price", 0.0);
            msg.replaceOrder.orderQty = d.value("orderQty", 0u);
            msg.replaceOrder.tif = tifFromJson(d.value("tif", ""));
        } else if (msg.type == "subscribe_book" || msg.type == "unsubscribe_book") {
            msg.symbol = j.value("symbol", "");
        }
    } catch (const json::exception&) {
        msg.type = "error";
    }
    return msg;
}
