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

} // namespace
