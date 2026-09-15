#include "ClientMessageParser.h"
#include "EnumStrings.h"
#include <nlohmann/json.hpp>

using namespace COP;
using namespace COP::App;
using json = nlohmann::json;

ParsedClientMessage App::parseClientMessage(const std::string &jsonStr)
{
    ParsedClientMessage msg;
    try
    {
        auto j = json::parse(jsonStr);
        msg.type = j.value("type", "");

        if (msg.type == "new_order")
        {
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
        }
        else if (msg.type == "new_swap_order")
        {
            auto d = j["data"];
            msg.swapOrder.symbol = d.value("symbol", "");
            msg.swapOrder.side = sideFromJson(d.value("side", ""));
            msg.swapOrder.nearPrice = d.value("nearPrice", 0.0);
            msg.swapOrder.farPrice = d.value("farPrice", 0.0);
            msg.swapOrder.settlDate = d.value("settlDate", (u64)0);
            msg.swapOrder.farSettlDate = d.value("farSettlDate", (u64)0);
            msg.swapOrder.orderQty = d.value("orderQty", 0u);
            msg.swapOrder.account = d.value("account", "");
            msg.swapOrder.currency = currencyFromJson(d.value("currency", ""));
            msg.swapOrder.capacity = capacityFromJson(d.value("capacity", ""));
            msg.swapOrder.tif = tifFromJson(d.value("tif", "GTC"));
        }
        else if (msg.type == "cancel_order")
        {
            auto d = j["data"];
            msg.cancelOrder.orderId = d.value("orderId", (u64)0);
            msg.cancelOrder.clOrderId = d.value("clOrderId", "");
        }
        else if (msg.type == "replace_order")
        {
            auto d = j["data"];
            msg.replaceOrder.orderId = d.value("orderId", (u64)0);
            msg.replaceOrder.clOrderId = d.value("clOrderId", "");
            msg.replaceOrder.hasPrice = d.contains("price");
            msg.replaceOrder.hasQty = d.contains("orderQty");
            msg.replaceOrder.hasTif = d.contains("tif");
            msg.replaceOrder.price = d.value("price", 0.0);
            msg.replaceOrder.orderQty = d.value("orderQty", 0u);
            msg.replaceOrder.tif = tifFromJson(d.value("tif", ""));
        }
        else if (msg.type == "subscribe_book" || msg.type == "unsubscribe_book")
        {
            msg.symbol = j.value("symbol", "");
        }
    }
    catch (const json::exception &)
    {
        msg.type = "error";
    }
    return msg;
}
