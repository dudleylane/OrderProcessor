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
