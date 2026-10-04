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
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","orderQty":)" +
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
    EXPECT_FALSE(msg.replaceOrder.hasQty);
    EXPECT_FALSE(msg.replaceOrder.hasTif);
    EXPECT_DOUBLE_EQ(10.5, msg.replaceOrder.price);
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
    auto msg = parse(R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","minQty":1e30}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("minQty"));
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

// --- FX swap orders (feature/fx-swap-support only, #86) ---

TEST(ClientMessageParserTest, NewSwapOrderParsesFields)
{
    auto msg = parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","side":"BUY","nearPrice":1.085,)"
                     R"("farPrice":1.0875,"settlDate":20261006,"farSettlDate":20261106,"orderQty":1000000,)"
                     R"("account":"ACC1","tif":"DAY"}})");

    EXPECT_EQ("new_swap_order", msg.type);
    EXPECT_TRUE(msg.error.empty());
    EXPECT_EQ("EURUSD", msg.swapOrder.symbol);
    EXPECT_EQ(BUY_SIDE, msg.swapOrder.side);
    EXPECT_DOUBLE_EQ(1.085, msg.swapOrder.nearPrice);
    EXPECT_DOUBLE_EQ(1.0875, msg.swapOrder.farPrice);
    EXPECT_EQ(20261006u, msg.swapOrder.settlDate);
    EXPECT_EQ(20261106u, msg.swapOrder.farSettlDate);
    EXPECT_EQ(1000000u, msg.swapOrder.orderQty);
    EXPECT_EQ("ACC1", msg.swapOrder.account);
    EXPECT_EQ(DAY_TIF, msg.swapOrder.tif);
}

TEST(ClientMessageParserTest, NewSwapOrderTifDefaultsToGtc)
{
    auto msg = parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","side":"SELL","orderQty":5}})");

    EXPECT_EQ("new_swap_order", msg.type);
    EXPECT_EQ(GTC_TIF, msg.swapOrder.tif);
    EXPECT_DOUBLE_EQ(0.0, msg.swapOrder.nearPrice);
    EXPECT_EQ(0u, msg.swapOrder.settlDate);
}

TEST(ClientMessageParserTest, NewSwapOrderNegativeQuantityIsRejected)
{
    // Before #86 this rested a swap for 4294967295
    auto msg = parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","side":"BUY","orderQty":-1}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty"));
}

TEST(ClientMessageParserTest, NewSwapOrderQuantityAboveUnsignedRangeIsRejected)
{
    // Before #86 this was an undefined float-to-integer conversion
    auto msg = parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","side":"BUY","orderQty":1e20}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("orderQty"));
}

TEST(ClientMessageParserTest, NewSwapOrderSettlementDatesMustBeNonNegativeIntegers)
{
    auto negative = parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","settlDate":-5,"orderQty":1}})");
    EXPECT_EQ("error", negative.type);
    EXPECT_NE(std::string::npos, negative.error.find("settlDate"));

    auto fractional =
        parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","farSettlDate":20261106.5,"orderQty":1}})");
    EXPECT_EQ("error", fractional.type);
    EXPECT_NE(std::string::npos, fractional.error.find("farSettlDate"));
}

TEST(ClientMessageParserTest, NewSwapOrderPricesMustBeNumbers)
{
    auto msg = parse(R"({"type":"new_swap_order","data":{"symbol":"EURUSD","farPrice":"1.0875","orderQty":1}})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("farPrice"));
}

TEST(ClientMessageParserTest, NewSwapOrderDataMustBeAnObject)
{
    auto msg = parse(R"({"type":"new_swap_order","data":"EURUSD"})");

    EXPECT_EQ("error", msg.type);
    EXPECT_NE(std::string::npos, msg.error.find("data"));
}

} // namespace
