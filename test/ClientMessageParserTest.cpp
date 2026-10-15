/**
 Concurrent Order Processor library - WebSocket client message parser Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <limits>
#include "ClientMessageParser.h"

using namespace COP;
using namespace COP::App;

namespace
{

ParsedClientMessage parse(const std::string &json)
{
    return parseClientMessage(json);
}

// --- Accepted messages ---

TEST(ClientMessageParserTest, NewOrderParsesFields)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                     R"("price":150.25,"stopPx":0,"orderQty":100,"minQty":5,"account":"ACC1"}})");

    EXPECT_EQ("new_order", msg.type);
    EXPECT_TRUE(msg.error.empty());
    EXPECT_EQ("AAPL", msg.newOrder.symbol);
    EXPECT_EQ(BUY_SIDE, msg.newOrder.side);
    EXPECT_EQ(LIMIT_ORDERTYPE, msg.newOrder.ordType);
    EXPECT_DOUBLE_EQ(150.25, msg.newOrder.price);
    EXPECT_EQ(100u, msg.newOrder.orderQty);
    EXPECT_EQ(5u, msg.newOrder.minQty);
    EXPECT_EQ("ACC1", msg.newOrder.account);
}

TEST(ClientMessageParserTest, AbsentOptionalNumbersDefaultToZero)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"SELL","ordType":"MARKET","orderQty":50}})");

    EXPECT_EQ("new_order", msg.type);
    EXPECT_EQ(50u, msg.newOrder.orderQty);
    EXPECT_EQ(0u, msg.newOrder.minQty);
    EXPECT_DOUBLE_EQ(0.0, msg.newOrder.price);
    EXPECT_DOUBLE_EQ(0.0, msg.newOrder.stopPx);
}

TEST(ClientMessageParserTest, LargestQuantityIsAccepted)
{
    const unsigned int maxQty = std::numeric_limits<unsigned int>::max();
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":10,)"
                     R"("orderQty":)" +
                     std::to_string(maxQty) + "}}");

    EXPECT_EQ("new_order", msg.type);
    EXPECT_EQ(maxQty, msg.newOrder.orderQty);
}

TEST(ClientMessageParserTest, CancelOrderParsesId)
{
    auto msg = parse(R"({"type":"cancel_order","data":{"orderId":12345,"clOrderId":"WS-1"}})");

    EXPECT_EQ("cancel_order", msg.type);
    EXPECT_EQ(12345u, msg.cancelOrder.orderId);
    EXPECT_EQ("WS-1", msg.cancelOrder.clOrderId);
}

TEST(ClientMessageParserTest, ReplaceOrderReportsWhichFieldsArePresent)
{
    auto msg = parse(R"({"type":"replace_order","data":{"orderId":7,"price":10.5}})");

    EXPECT_EQ("replace_order", msg.type);
    EXPECT_EQ(7u, msg.replaceOrder.orderId);
    EXPECT_TRUE(msg.replaceOrder.hasPrice);
    EXPECT_FALSE(msg.replaceOrder.hasStopPx);
    EXPECT_FALSE(msg.replaceOrder.hasQty);
    EXPECT_FALSE(msg.replaceOrder.hasTif);
    EXPECT_DOUBLE_EQ(10.5, msg.replaceOrder.price);
}

TEST(ClientMessageParserTest, AGtdOrderNeedsAnExpireTimeAndNoOtherOrderHasOne)
{
    // A GTD order expires at its expireTime, in milliseconds since the epoch (#240)
    const std::string base = R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":10,)"
                             R"("orderQty":5,"currency":"USD","capacity":"AGENCY",)";
    auto msg = parse(base + R"("tif":"GTD","expireTime":1792108800000}})");
    EXPECT_EQ("new_order", msg.type) << msg.error;
    EXPECT_EQ(1792108800000ULL, msg.newOrder.expireTime);

    msg = parse(base + R"("tif":"GTD"}})");
    EXPECT_EQ("error", msg.type);
    EXPECT_EQ("invalid expireTime: a GTD order needs one", msg.error);

    msg = parse(base + R"("tif":"DAY","expireTime":1792108800000}})");
    EXPECT_EQ("error", msg.type);
    EXPECT_EQ("invalid expireTime: only a GTD order has one", msg.error);

    msg = parse(base + R"("tif":"GTD","expireTime":-5}})");
    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("expireTime")) << msg.error;

    msg = parse(R"({"type":"replace_order","data":{"orderId":7,"expireTime":1792108800000}})");
    EXPECT_EQ("replace_order", msg.type) << msg.error;
    EXPECT_TRUE(msg.replaceOrder.hasExpireTime);
    EXPECT_EQ(1792108800000ULL, msg.replaceOrder.expireTime);
}

TEST(ClientMessageParserTest, ReplaceOrderTakesAStopPxAboveZero)
{
    // A replace moves a stop order's stop (#238), as a new order gives it one: above 0
    auto msg = parse(R"({"type":"replace_order","data":{"orderId":7,"stopPx":9.75}})");
    EXPECT_EQ("replace_order", msg.type);
    EXPECT_TRUE(msg.replaceOrder.hasStopPx);
    EXPECT_FALSE(msg.replaceOrder.hasPrice);
    EXPECT_DOUBLE_EQ(9.75, msg.replaceOrder.stopPx);

    for (const char *stopPx : { "0", "-1.5" })
    {
        msg = parse(std::string(R"({"type":"replace_order","data":{"orderId":7,"stopPx":)") + stopPx + "}}");
        EXPECT_EQ("error", msg.type) << stopPx;
        EXPECT_EQ("invalid stopPx: expected a number above 0", msg.error) << stopPx;
    }
    msg = parse(R"({"type":"replace_order","data":{"orderId":7,"stopPx":"9"}})");
    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("stopPx")) << msg.error;
}

TEST(ClientMessageParserTest, UnknownTypeIsPassedThroughForTheCaller)
{
    auto msg = parse(R"({"type":"ping"})");

    EXPECT_EQ("ping", msg.type);
    EXPECT_TRUE(msg.error.empty());
}

// --- Rejected numeric fields (issue #16: these narrowings were undefined behaviour) ---

TEST(ClientMessageParserTest, QuantityAboveUnsignedRangeIsRejected)
{
    auto msg = parse(
        R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","orderQty":5,"minQty":1e30}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("minQty"));
}

// --- A limit order's price (issue #188: an absent one was 0, and a limit sell at 0 sold at any bid) ---

TEST(ClientMessageParserTest, LimitOrderWithoutAPriceIsRejected)
{
    for (const char *ordType : { "LIMIT", "STOPLIMIT" })
    {
        auto msg = parse(std::string(R"({"type":"new_order","data":{"symbol":"AAPL","side":"SELL","ordType":")") +
                         ordType + R"(","orderQty":100}})");

        EXPECT_EQ("error", msg.type) << ordType;
        EXPECT_NE(std::string::npos, msg.error.find("needs one above 0")) << ordType << ": " << msg.error;
    }
}

TEST(ClientMessageParserTest, LimitPriceNotAboveZeroIsRejected)
{
    for (const char *price : { "0", "-5", "-0.0001" })
    {
        auto msg = parse(std::string(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)") +
                         R"("orderQty":100,"price":)" + price + "}}");

        EXPECT_EQ("error", msg.type) << price;
        EXPECT_NE(std::string::npos, msg.error.find("needs one above 0")) << price << ": " << msg.error;
    }
}

TEST(ClientMessageParserTest, ReplaceToAPriceNotAboveZeroIsRejected)
{
    auto msg = parse(R"({"type":"replace_order","data":{"orderId":7,"price":0}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("price: expected a number above 0")) << msg.error;
}

// --- A stop order's stop price (issue #223: it releases the order, and at 0 a buy stop was released by any trade) ---

TEST(ClientMessageParserTest, StopOrderWithoutAStopPriceIsRejected)
{
    for (const char *ordType : { "STOP", "STOPLIMIT" })
    {
        for (const char *stopPx : { "", R"(,"stopPx":0)", R"(,"stopPx":-2.5)" })
        {
            auto msg = parse(std::string(R"({"type":"new_order","data":{"symbol":"AAPL","side":"SELL","ordType":")") +
                             ordType + R"(","price":10,"orderQty":100)" + stopPx + "}}");

            EXPECT_EQ("error", msg.type) << ordType << stopPx;
            EXPECT_NE(std::string::npos, msg.error.find("stopPx: a STOP or STOPLIMIT order needs one above 0"))
                << ordType << stopPx << ": " << msg.error;
        }
    }
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"STOP","orderQty":100,)"
                     R"("stopPx":10.5}})");
    EXPECT_EQ("new_order", msg.type) << msg.error;
    EXPECT_DOUBLE_EQ(10.5, msg.newOrder.stopPx);
}

// --- An order for nothing (issue #187: it was accepted, and traded for 0) ---

TEST(ClientMessageParserTest, NewOrderWithoutAQuantityIsRejected)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":10}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty: expected an integer from 1")) << msg.error;
}

TEST(ClientMessageParserTest, NewOrderForZeroIsRejected)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","orderQty":0}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty: expected an integer from 1")) << msg.error;
}

TEST(ClientMessageParserTest, ReplaceToZeroIsRejected)
{
    // A replace without orderQty keeps the quantity (ReplaceOrderReportsWhichFieldsArePresent); one to 0 is refused
    auto msg = parse(R"({"type":"replace_order","data":{"orderId":7,"orderQty":0}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty: expected an integer from 1")) << msg.error;
}

TEST(ClientMessageParserTest, NegativeQuantityIsRejected)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","orderQty":-5}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty"));
}

TEST(ClientMessageParserTest, FractionalQuantityIsRejected)
{
    auto msg =
        parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","orderQty":100.5}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty"));
}

TEST(ClientMessageParserTest, QuantityOneAboveTheLimitIsRejected)
{
    auto msg =
        parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","orderQty":4294967296}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty"));
}

TEST(ClientMessageParserTest, OrderIdBeyondSixtyFourBitsIsRejected)
{
    auto msg = parse(R"({"type":"cancel_order","data":{"orderId":18446744073709551616,"clOrderId":"x"}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderId"));
}

TEST(ClientMessageParserTest, NegativeOrderIdIsRejected)
{
    auto msg = parse(R"({"type":"cancel_order","data":{"orderId":-1}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderId"));
}

TEST(ClientMessageParserTest, NonNumericPriceIsRejected)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":"abc"}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("price"));
}

// --- Prices beyond double's range (issue #142) ---
// Release builds compile with -ffast-math. Until #142 its -ffinite-math-only let the compiler delete both nlohmann's
// overflow check and readPrice's std::isfinite, so these parsed as +-inf in Release while Debug rejected them. These
// tests matter in the Release ctest (CI's Build & Test job and the pre-push hook).

TEST(ClientMessageParserTest, PriceBeyondDoubleRangeIsRejected)
{
    auto msg = parse(
        R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":1e400,"orderQty":5}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("overflow"));
}

TEST(ClientMessageParserTest, NegativePriceBeyondDoubleRangeIsRejected)
{
    auto msg = parse(
        R"({"type":"new_order","data":{"symbol":"AAPL","side":"SELL","ordType":"LIMIT","price":-1e400,"orderQty":5}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("overflow"));
}

TEST(ClientMessageParserTest, StopPriceBeyondDoubleRangeIsRejected)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"STOPLIMIT","price":10,)"
                     R"("stopPx":1e400,"orderQty":5}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("overflow"));
}

TEST(ClientMessageParserTest, ReplacePriceBeyondDoubleRangeIsRejected)
{
    auto msg = parse(R"({"type":"replace_order","data":{"orderId":7,"price":1e400}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("overflow"));
}

// --- Rejected message shapes ---

TEST(ClientMessageParserTest, DataMustBeAnObject)
{
    auto msg = parse(R"({"type":"new_order","data":[1,2,3]})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("data"));
}

TEST(ClientMessageParserTest, MalformedJsonIsRejectedWithAReason)
{
    auto msg = parse(R"({"type":"new_order","data":{"symbol":)");

    EXPECT_EQ("error", msg.type);
    EXPECT_FALSE(msg.error.empty());
}

TEST(ClientMessageParserTest, EmptyInputIsRejected)
{
    auto msg = parse("");

    EXPECT_EQ("error", msg.type);
    EXPECT_FALSE(msg.error.empty());
}

TEST(ClientMessageParserTest, ClientSentErrorTypeIsRejected)
{
    auto msg = parse(R"({"type":"error"})");

    EXPECT_EQ("error", msg.type);
    EXPECT_FALSE(msg.error.empty());
}

} // namespace
