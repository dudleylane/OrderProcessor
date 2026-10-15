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

// An order's quantity: required, and from 1 (#187). An absent one used to become 0, an order for nothing.
bool readQuantity(const json &data, const char *key, u64 max, u64 *out, ParsedClientMessage *msg)
{
    json::const_iterator it = data.find(key);
    if ((data.end() == it) || !it->is_number_unsigned() || (it->get<u64>() < 1) || (it->get<u64>() > max))
    {
        markInvalid(msg, std::string("invalid ") + key + ": expected an integer from 1 to " + std::to_string(max));
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
                readQuantity(d, "orderQty", maxQty, &orderQty, &msg) &&
                readUnsigned(d, "minQty", maxQty, &minQty, &msg) &&
                readUnsigned(d, "expireTime", maxId, &msg.newOrder.expireTime, &msg))
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
                // A limit price is required, and above 0 (#188): an absent one was 0, and a limit sell at 0 sold at any
                // bid
                const bool limit =
                    (LIMIT_ORDERTYPE == msg.newOrder.ordType) || (STOPLIMIT_ORDERTYPE == msg.newOrder.ordType);
                if (limit && !(msg.newOrder.price > 0))
                {
                    markInvalid(&msg, "invalid price: a LIMIT or STOPLIMIT order needs one above 0");
                }
                // A stop price is required, and above 0 (#223): it releases the order, and at 0 a buy stop was released
                // by any trade
                const bool stop =
                    (STOP_ORDERTYPE == msg.newOrder.ordType) || (STOPLIMIT_ORDERTYPE == msg.newOrder.ordType);
                if (stop && !(msg.newOrder.stopPx > 0))
                {
                    markInvalid(&msg, "invalid stopPx: a STOP or STOPLIMIT order needs one above 0");
                }
                // A GTD order expires at its expireTime, and an order of any other time in force has none (#240)
                else if ((GTD_TIF == msg.newOrder.tif) && (0 == msg.newOrder.expireTime))
                {
                    markInvalid(&msg, "invalid expireTime: a GTD order needs one");
                }
                else if ((GTD_TIF != msg.newOrder.tif) && (0 != msg.newOrder.expireTime))
                {
                    markInvalid(&msg, "invalid expireTime: only a GTD order has one");
                }
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
                readPrice(d, "stopPx", &msg.replaceOrder.stopPx, &msg) &&
                readUnsigned(d, "expireTime", maxId, &msg.replaceOrder.expireTime, &msg) &&
                (!d.contains("orderQty") || readQuantity(d, "orderQty", maxQty, &orderQty, &msg)))
            {
                msg.replaceOrder.clOrderId = d.value("clOrderId", "");
                msg.replaceOrder.hasPrice = d.contains("price");
                msg.replaceOrder.hasStopPx = d.contains("stopPx");
                msg.replaceOrder.hasQty = d.contains("orderQty");
                msg.replaceOrder.hasTif = d.contains("tif");
                msg.replaceOrder.hasExpireTime = d.contains("expireTime");
                msg.replaceOrder.orderQty = static_cast<unsigned int>(orderQty);
                msg.replaceOrder.tif = tifFromJson(d.value("tif", ""));
                // A new price is above 0 (#188), and so is a new stopPx, as a stop order's is when it arrives (#238)
                if (msg.replaceOrder.hasPrice && !(msg.replaceOrder.price > 0))
                {
                    markInvalid(&msg, "invalid price: expected a number above 0");
                }
                else if (msg.replaceOrder.hasStopPx && !(msg.replaceOrder.stopPx > 0))
                {
                    markInvalid(&msg, "invalid stopPx: expected a number above 0");
                }
                else if (msg.replaceOrder.hasExpireTime && (0 == msg.replaceOrder.expireTime))
                {
                    markInvalid(&msg, "invalid expireTime: expected milliseconds since the epoch, above 0");
                }
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
