#include "ClientMessageParser.h"
#include "EnumStrings.h"
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

using namespace COP;
using namespace COP::App;
using json = nlohmann::json;

namespace
{

void markInvalid(ParsedClientMessage *msg, const std::string &reason)
{
    msg->type = "error";
    msg->error = reason;
}

bool requireObject(const json &data, ParsedClientMessage *msg)
{
    if (data.is_object())
    {
        return true;
    }
    markInvalid(msg, "data must be an object");
    return false;
}

// Numeric fields are checked before they are narrowed. nlohmann's value<T>() static_casts a JSON number to T,
// which is undefined behaviour for a floating-point value outside T's range. An absent field keeps its default of 0.
bool readUnsigned(const json &data, const char *key, u64 max, u64 *out, ParsedClientMessage *msg)
{
    json::const_iterator it = data.find(key);
    if (data.end() == it)
    {
        *out = 0;
        return true;
    }
    if (!it->is_number_unsigned() || (it->get<u64>() > max))
    {
        markInvalid(msg, std::string("invalid ") + key + ": expected an integer from 0 to " + std::to_string(max));
        return false;
    }
    *out = it->get<u64>();
    return true;
}

bool readPrice(const json &data, const char *key, double *out, ParsedClientMessage *msg)
{
    json::const_iterator it = data.find(key);
    if (data.end() == it)
    {
        *out = 0.0;
        return true;
    }
    if (!it->is_number() || !std::isfinite(it->get<double>()))
    {
        markInvalid(msg, std::string("invalid ") + key + ": expected a finite number");
        return false;
    }
    *out = it->get<double>();
    return true;
}

} // namespace

ParsedClientMessage App::parseClientMessage(const std::string &jsonStr)
{
    const u64 maxQty = std::numeric_limits<unsigned int>::max();
    const u64 maxId = std::numeric_limits<u64>::max();
    ParsedClientMessage msg{};
    try
    {
        auto j = json::parse(jsonStr);
        msg.type = j.value("type", "");

        if (msg.type == "error")
        {
            markInvalid(&msg, "unknown message type: error");
        }
        else if (msg.type == "new_order")
        {
            const json &d = j["data"];
            u64 orderQty = 0;
            u64 minQty = 0;
            if (requireObject(d, &msg) && readPrice(d, "price", &msg.newOrder.price, &msg) &&
                readPrice(d, "stopPx", &msg.newOrder.stopPx, &msg) &&
                readUnsigned(d, "orderQty", maxQty, &orderQty, &msg) &&
                readUnsigned(d, "minQty", maxQty, &minQty, &msg))
            {
                msg.newOrder.symbol = d.value("symbol", "");
                msg.newOrder.side = sideFromJson(d.value("side", ""));
                msg.newOrder.ordType = orderTypeFromJson(d.value("ordType", ""));
                msg.newOrder.orderQty = static_cast<unsigned int>(orderQty);
                msg.newOrder.minQty = static_cast<unsigned int>(minQty);
                msg.newOrder.tif = tifFromJson(d.value("tif", ""));
                msg.newOrder.account = d.value("account", "");
                msg.newOrder.currency = currencyFromJson(d.value("currency", ""));
                msg.newOrder.capacity = capacityFromJson(d.value("capacity", ""));
            }
        }
        else if (msg.type == "cancel_order")
        {
            const json &d = j["data"];
            if (requireObject(d, &msg) && readUnsigned(d, "orderId", maxId, &msg.cancelOrder.orderId, &msg))
            {
                msg.cancelOrder.clOrderId = d.value("clOrderId", "");
            }
        }
        else if (msg.type == "replace_order")
        {
            const json &d = j["data"];
            u64 orderQty = 0;
            if (requireObject(d, &msg) && readUnsigned(d, "orderId", maxId, &msg.replaceOrder.orderId, &msg) &&
                readPrice(d, "price", &msg.replaceOrder.price, &msg) &&
                readUnsigned(d, "orderQty", maxQty, &orderQty, &msg))
            {
                msg.replaceOrder.hasPrice = d.contains("price");
                msg.replaceOrder.hasQty = d.contains("orderQty");
                msg.replaceOrder.hasTif = d.contains("tif");
                msg.replaceOrder.orderQty = static_cast<unsigned int>(orderQty);
                msg.replaceOrder.tif = tifFromJson(d.value("tif", ""));
            }
        }
        else if (msg.type == "subscribe_book" || msg.type == "unsubscribe_book")
        {
            msg.symbol = j.value("symbol", "");
        }
    }
    catch (const json::exception &ex)
    {
        markInvalid(&msg, ex.what());
    }
    return msg;
}
