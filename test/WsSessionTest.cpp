/**
 Concurrent Order Processor library - WebSocket session Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>

#include "MockQueues.h"
#include "MockStorage.h"
#include "TestAux.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "IncomingQueues.h"
#include "OrderStorage.h"
#include "SessionManager.h"
#include "WideDataStorage.h"
#include "WsSession.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace COP;
using namespace COP::Store;

namespace
{

const std::chrono::seconds REPLY_TIMEOUT(5);

/// A real WsSession on loopback, served by its own io_context thread as in app/main.cpp. The thread
/// records an exception that escapes io_context::run(), which in the server would terminate the process.
class WsSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();

        auto *instr = new InstrumentEntry();
        instr->symbol_ = "AAPL";
        instr->securityId_ = "US0378331005";
        instr->securityIdSource_ = "ISIN";
        WideDataStorage::instance()->add(instr);
        accountId_ = test::addAccount("TRADING-1");
        otherAccountId_ = test::addAccount("CLIENT-A");
        clearingId_ = test::addClearing("HOUSE-CLEARING");

        endpoint_ = acceptor_.local_endpoint();
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    std::make_shared<App::WsSession>(std::move(socket), &sessionMgr_, WideDataStorage::instance(),
                                                     OrderStorage::instance(), &inQueues_, IdTGenerator::instance(),
                                                     nullptr, useDefaultClearing_ ? clearingId_ : SourceIdT(),
                                                     useDefaultAccount_ ? accountId_ : SourceIdT())
                        ->run();
                }
            });
        serverThread_ = std::thread(
            [this]
            {
                try
                {
                    serverIoc_.run();
                }
                catch (const std::exception &ex)
                {
                    std::lock_guard<std::mutex> lock(escapedLock_);
                    escaped_ = ex.what();
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(escapedLock_);
                    escaped_ = "unknown exception";
                }
            });
    }

    void TearDown() override
    {
        serverIoc_.stop();
        serverThread_.join();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    std::string escaped()
    {
        std::lock_guard<std::mutex> lock(escapedLock_);
        return escaped_;
    }

    struct Client
    {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ ioc };
        beast::flat_buffer buffer;
    };

    void connect(Client &client)
    {
        client.ws.next_layer().connect(endpoint_);
        client.ws.handshake("127.0.0.1", "/ws");
    }

    static void send(Client &client, const std::string &text)
    {
        client.ws.write(net::buffer(text));
    }

    /// Reads messages until one of the given type arrives. A dead server sends nothing more, so every read
    /// is bounded rather than blocking.
    static std::optional<nlohmann::json> readUntil(Client &client, const std::string &type)
    {
        const auto deadline = std::chrono::steady_clock::now() + REPLY_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::optional<std::string> text;
            client.buffer.clear();
            client.ws.async_read(client.buffer,
                                 [&](beast::error_code ec, std::size_t)
                                 {
                                     if (!ec)
                                     {
                                         text = beast::buffers_to_string(client.buffer.data());
                                     }
                                 });
            client.ioc.restart();
            client.ioc.run_for(deadline - std::chrono::steady_clock::now());
            if (!text)
            {
                return std::nullopt;
            }
            nlohmann::json msg = nlohmann::json::parse(*text);
            if (msg.value("type", "") == type)
            {
                return msg;
            }
        }
        return std::nullopt;
    }

    /// Takes a copy of the order the session queued, polling until the timeout, since the session runs on its own
    /// thread. A zero timeout polls once. Returns null when nothing was queued.
    std::unique_ptr<OrderEntry> takeQueuedOrder(std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        std::unique_ptr<OrderEntry> order;
        testing::NiceMock<test::MockInQueueProcessor> processor;
        // pop() deletes the event's order after dispatch, so keep a copy
        ON_CALL(processor, onEvent(testing::_, testing::An<const Queues::OrderEvent &>()))
            .WillByDefault(
                [&order](const std::string &, const Queues::OrderEvent &evnt)
                {
                    order.reset(evnt.order_->clone());
                });
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!inQueues_.pop(&processor) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return order;
    }

    /// Takes the cancel the session queued, polling as takeQueuedOrder() does. Empty when nothing was queued.
    std::optional<Queues::OrderCancelEvent> takeQueuedCancel(std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        std::optional<Queues::OrderCancelEvent> cancel;
        testing::NiceMock<test::MockInQueueProcessor> processor;
        ON_CALL(processor, onEvent(testing::_, testing::An<const Queues::OrderCancelEvent &>()))
            .WillByDefault(
                [&cancel](const std::string &, const Queues::OrderCancelEvent &evnt)
                {
                    cancel = evnt;
                });
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!inQueues_.pop(&processor) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return cancel;
    }

    /// Restores an order with this id and ClOrdID, as a load from persistence does. Its fields are set before it is
    /// stored, so the storage's lock orders them before the session reads them.
    static OrderEntry *restoreOrder(const IdT &id, const std::string &clOrdId, Side side = BUY_SIDE,
                                    PriceT price = 150.0, QuantityT qty = 100)
    {
        SourceIdT clOrderId = WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size())));
        SourceIdT instrument = WideDataStorage::instance()->findInstrumentBySymbol("AAPL");
        auto *order = new OrderEntry(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrument, SourceIdT(),
                                     SourceIdT(), SourceIdT());
        order->orderId_ = id;
        order->side_ = side;
        order->price_ = price;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;
        OrderStorage::instance()->restore(order);
        return order;
    }

    /// Takes the replace the session queued, with a copy of its replacement, polling as takeQueuedOrder() does. Empty
    /// when nothing was queued.
    std::optional<std::pair<IdT, std::unique_ptr<OrderEntry>>>
    takeQueuedReplace(std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        std::optional<std::pair<IdT, std::unique_ptr<OrderEntry>>> replace;
        testing::NiceMock<test::MockInQueueProcessor> processor;
        // pop() frees the event's replacement after dispatch, so keep a copy (#74)
        ON_CALL(processor, onEvent(testing::_, testing::An<const Queues::OrderReplaceEvent &>()))
            .WillByDefault(
                [&replace](const std::string &, const Queues::OrderReplaceEvent &evnt)
                {
                    replace.emplace(evnt.id_, std::unique_ptr<OrderEntry>(nullptr != evnt.replacementOrder_
                                                                              ? evnt.replacementOrder_->clone()
                                                                              : nullptr));
                });
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!inQueues_.pop(&processor) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return replace;
    }

    /// The session still reads and answers: an unknown type gets the ordinary error reply.
    static bool stillAnswers(Client &client)
    {
        send(client, R"({"type":"no_such_type"})");
        auto reply = readUntil(client, "error");
        return reply.has_value() && std::string::npos != reply->value("message", "").find("Unknown message type");
    }

    net::io_context serverIoc_{ 1 };
    tcp::acceptor acceptor_{ serverIoc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0) };
    tcp::endpoint endpoint_;
    std::thread serverThread_;
    App::SessionManager sessionMgr_;
    Queues::IncomingQueues inQueues_;
    testing::NiceMock<test::MockDataSaver> saver_;

    SourceIdT accountId_;
    SourceIdT otherAccountId_;
    SourceIdT clearingId_;
    // What the next session gets as the server's defaults. Set before connecting; read on the server thread.
    std::atomic<bool> useDefaultClearing_{ true };
    std::atomic<bool> useDefaultAccount_{ true };

    std::mutex escapedLock_;
    std::string escaped_;
};

TEST_F(WsSessionTest, RequestThatThrowsIsAnsweredAndTheSessionKeepsReading)
{
    // The bug this covers: an exception while handling one message left io_context::run() and terminated
    // the server (#36). Storage failing on the order's first write stands in for any such exception.
    ON_CALL(saver_, save(testing::An<const RawDataEntry &>()))
        .WillByDefault(testing::Throw(std::runtime_error("injected storage failure")));
    WideDataStorage::instance()->bindStorage(&saver_);

    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                 R"("price":150.25,"orderQty":100,"tif":"DAY","currency":"USD","capacity":"AGENCY"}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "no error reply; escaped: " << escaped();
    EXPECT_EQ("Internal error: request not processed", reply->value("message", ""));
    EXPECT_EQ("", escaped());
    EXPECT_TRUE(stillAnswers(client));
}

TEST_F(WsSessionTest, InitialStateThatThrowsIsAnsweredAndTheSessionKeepsReading)
{
    // Serializing a new connection's order snapshot can throw (#35 is one cause), and that terminated the
    // server too. An order whose account cannot be resolved makes the snapshot throw.
    SourceIdT clOrderId = WideDataStorage::instance()->add(new RawDataEntry(STRING_RAWDATATYPE, "CL-1", 4));
    SourceIdT instrument = WideDataStorage::instance()->findInstrumentBySymbol("AAPL");
    auto *order = new OrderEntry(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrument, SourceIdT(99999, 1),
                                 SourceIdT(), SourceIdT());
    order->orderId_ = IdT(1, 1);
    OrderStorage::instance()->restore(order);

    Client client;
    connect(client);
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "no error reply; escaped: " << escaped();
    EXPECT_EQ("Internal error: initial state is incomplete", reply->value("message", ""));
    EXPECT_EQ("", escaped());
    EXPECT_TRUE(stillAnswers(client));
}

TEST_F(WsSessionTest, SnapshotIncludesAnOrderWithUnsetReferences)
{
    // The live form of #35: once any order existed, a new connection's snapshot threw, because a new order's
    // origClOrderId (and an account the client left out) is unset. Before #36 that terminated the server.
    SourceIdT clOrderId = WideDataStorage::instance()->add(new RawDataEntry(STRING_RAWDATATYPE, "CL-7", 4));
    SourceIdT instrument = WideDataStorage::instance()->findInstrumentBySymbol("AAPL");
    auto *order = new OrderEntry(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrument, SourceIdT(), SourceIdT(),
                                 SourceIdT());
    order->orderId_ = IdT(7, 1);
    OrderStorage::instance()->restore(order);

    Client client;
    connect(client);
    auto snapshot = readUntil(client, "order_snapshot");
    ASSERT_TRUE(snapshot.has_value()) << "no snapshot; escaped: " << escaped();
    ASSERT_EQ(1u, (*snapshot)["data"].size());
    EXPECT_EQ("CL-7", (*snapshot)["data"][0].value("clOrderId", "?"));
    EXPECT_TRUE(stillAnswers(client));
}

TEST_F(WsSessionTest, OrderThatNamesNoAccountGetsTheServerDefaults)
{
    // The bug this covers: every order the session built failed OrderEntry::isValid(), so the engine rejected it. It
    // had no clearing firm, no account when the client named none, no settlement type and no update time (#34).
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                 R"("price":150.25,"orderQty":100,"tif":"DAY","currency":"USD","capacity":"AGENCY"}})");
    std::unique_ptr<OrderEntry> order = takeQueuedOrder();
    ASSERT_NE(nullptr, order) << "no order queued; escaped: " << escaped();
    EXPECT_EQ(clearingId_, order->clearing_.getId());
    EXPECT_EQ(accountId_, order->account_.getId());
    std::string invalid;
    EXPECT_TRUE(order->isValid(&invalid)) << invalid;
}

TEST_F(WsSessionTest, OrderThatNamesAnAccountKeepsIt)
{
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_order","data":{"symbol":"AAPL","side":"SELL","ordType":"LIMIT","price":151.5,)"
                 R"("orderQty":50,"tif":"DAY","account":"CLIENT-A","currency":"USD","capacity":"AGENCY"}})");
    std::unique_ptr<OrderEntry> order = takeQueuedOrder();
    ASSERT_NE(nullptr, order) << "no order queued; escaped: " << escaped();
    EXPECT_EQ(otherAccountId_, order->account_.getId());
    EXPECT_EQ(clearingId_, order->clearing_.getId());
    std::string invalid;
    EXPECT_TRUE(order->isValid(&invalid)) << invalid;
}

TEST_F(WsSessionTest, OrderIsRefusedWhenTheServerHasNoClearingFirm)
{
    useDefaultClearing_ = false;
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":150.25,)"
                 R"("orderQty":100,"tif":"DAY","account":"TRADING-1","currency":"USD","capacity":"AGENCY"}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "no error reply; escaped: " << escaped();
    EXPECT_EQ("Order refused: the server has no clearing firm", reply->value("message", ""));
    EXPECT_EQ(nullptr, takeQueuedOrder(std::chrono::milliseconds(0)));
}

TEST_F(WsSessionTest, OrderThatNamesNoAccountIsRefusedWhenTheServerHasNoDefaultAccount)
{
    useDefaultAccount_ = false;
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                 R"("price":150.25,"orderQty":100,"tif":"DAY","currency":"USD","capacity":"AGENCY"}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "no error reply; escaped: " << escaped();
    EXPECT_EQ("Order refused: it names no account, and the server has no default account", reply->value("message", ""));
    EXPECT_EQ(nullptr, takeQueuedOrder(std::chrono::milliseconds(0)));
}

TEST_F(WsSessionTest, SwapOrderThatNamesNoAccountGetsTheServerDefaults)
{
    // On this branch new_swap_order builds orders too, so it needs the same defaults as new_order (#34).
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_swap_order","data":{"symbol":"AAPL","side":"BUY","nearPrice":1.085,"farPrice":1.087,)"
                 R"("settlDate":1000,"farSettlDate":2000,"orderQty":1000000,"currency":"USD","capacity":"AGENCY"}})");
    std::unique_ptr<OrderEntry> order = takeQueuedOrder();
    ASSERT_NE(nullptr, order) << "no order queued; escaped: " << escaped();
    EXPECT_EQ(FXSWAP_ORDERTYPE, order->ordType_);
    EXPECT_EQ(accountId_, order->account_.getId());
    EXPECT_EQ(clearingId_, order->clearing_.getId());
    std::string invalid;
    EXPECT_TRUE(order->isValid(&invalid)) << invalid;
}

TEST_F(WsSessionTest, SwapOrderIsRefusedWhenTheServerHasNoClearingFirm)
{
    useDefaultClearing_ = false;
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_swap_order","data":{"symbol":"AAPL","side":"BUY","nearPrice":1.085,"farPrice":1.087,)"
                 R"("settlDate":1000,"farSettlDate":2000,"orderQty":1000000,"account":"TRADING-1","currency":"USD",)"
                 R"("capacity":"AGENCY"}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "no error reply; escaped: " << escaped();
    EXPECT_EQ("Order refused: the server has no clearing firm", reply->value("message", ""));
    EXPECT_EQ(nullptr, takeQueuedOrder(std::chrono::milliseconds(0)));
}

TEST_F(WsSessionTest, ClOrdIdsCarryASequenceNumber)
{
    // The server makes up each WebSocket order's ClOrdID. From the time in microseconds alone, two orders in the same
    // microsecond got the same one, and the second is refused as a duplicate (#67); a sequence number keeps them apart.
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    const std::string order = R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                              R"("price":150.25,"orderQty":100,"tif":"DAY","currency":"USD","capacity":"AGENCY"}})";
    send(client, order);
    send(client, order);
    std::unique_ptr<OrderEntry> first = takeQueuedOrder();
    std::unique_ptr<OrderEntry> second = takeQueuedOrder();
    ASSERT_NE(nullptr, first) << "escaped: " << escaped();
    ASSERT_NE(nullptr, second) << "escaped: " << escaped();
    const std::regex format("WS-[0-9]+-([0-9]+)");
    const RawDataEntry &a = first->clOrderId_.get();
    const RawDataEntry &b = second->clOrderId_.get();
    const std::string firstId(a.data_, a.length_), secondId(b.data_, b.length_);
    std::smatch firstMatch, secondMatch;
    ASSERT_TRUE(std::regex_match(firstId, firstMatch, format)) << firstId;
    ASSERT_TRUE(std::regex_match(secondId, secondMatch, format)) << secondId;
    EXPECT_EQ(std::stoull(firstMatch[1]) + 1, std::stoull(secondMatch[1]));
}

TEST_F(WsSessionTest, SwapClOrdIdsCarryASequenceNumber)
{
    // new_swap_order makes up ClOrdIDs too, so it needs the same sequence number (#67)
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    const std::string swap = R"({"type":"new_swap_order","data":{"symbol":"AAPL","side":"BUY","nearPrice":1.085,)"
                             R"("farPrice":1.087,"settlDate":1000,"farSettlDate":2000,"orderQty":1000000,)"
                             R"("currency":"USD","capacity":"AGENCY"}})";
    send(client, swap);
    send(client, swap);
    std::unique_ptr<OrderEntry> first = takeQueuedOrder();
    std::unique_ptr<OrderEntry> second = takeQueuedOrder();
    ASSERT_NE(nullptr, first) << "escaped: " << escaped();
    ASSERT_NE(nullptr, second) << "escaped: " << escaped();
    const std::regex format("WS-[0-9]+-([0-9]+)");
    const RawDataEntry &a = first->clOrderId_.get();
    const RawDataEntry &b = second->clOrderId_.get();
    const std::string firstId(a.data_, a.length_), secondId(b.data_, b.length_);
    std::smatch firstMatch, secondMatch;
    ASSERT_TRUE(std::regex_match(firstId, firstMatch, format)) << firstId;
    ASSERT_TRUE(std::regex_match(secondId, secondMatch, format)) << secondId;
    EXPECT_EQ(std::stoull(firstMatch[1]) + 1, std::stoull(secondMatch[1]));
}

// =============================================================================
// Naming orders by number (#58)
// =============================================================================

TEST_F(WsSessionTest, CancelNamesTheOrderWithThatNumber)
{
    // The bug this covers: the session turned the client's number into the id (number, 1). No order has that id, since
    // an id's second half is its creation time, so no cancel ever found its order (#58).
    restoreOrder(IdT(3, 1790000000), "CL-3");
    OrderEntry *seven = restoreOrder(IdT(7, 1790000100), "CL-7");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"cancel_order","data":{"orderId":7}})");
    auto cancel = takeQueuedCancel();
    ASSERT_TRUE(cancel.has_value()) << "no cancel queued; escaped: " << escaped();
    EXPECT_EQ(seven->orderId_, cancel->id_);
}

TEST_F(WsSessionTest, CancelOfAnUnknownNumberGoesToTheEngineAsUnknown)
{
    // Under an invalid id that keeps the number, which the engine answers with a cancel_reject naming it (#57, #73)
    restoreOrder(IdT(7, 1790000100), "CL-7");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"cancel_order","data":{"orderId":999}})");
    auto cancel = takeQueuedCancel();
    ASSERT_TRUE(cancel.has_value()) << "no cancel queued; escaped: " << escaped();
    EXPECT_EQ(999u, cancel->id_.id_);
    EXPECT_FALSE(cancel->id_.isValid());
}

TEST_F(WsSessionTest, CancelOfANumberTwoOrdersShareIsRefused)
{
    // A data directory written before #58 can hold two orders with one number, and neither is a safe guess
    restoreOrder(IdT(7, 1790000100), "CL-7A");
    restoreOrder(IdT(7, 1790009999), "CL-7B");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"cancel_order","data":{"orderId":7}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("More than one order has number 7: name it by clOrderId as well", reply->value("message", ""));
    EXPECT_EQ(0u, inQueues_.size());
}

TEST_F(WsSessionTest, CancelByClOrdIdNamesTheOrderEvenWhenItsNumberIsShared)
{
    restoreOrder(IdT(7, 1790000100), "CL-7A");
    OrderEntry *second = restoreOrder(IdT(7, 1790009999), "CL-7B");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"cancel_order","data":{"orderId":7,"clOrderId":"CL-7B"}})");
    auto cancel = takeQueuedCancel();
    ASSERT_TRUE(cancel.has_value()) << "no cancel queued; escaped: " << escaped();
    EXPECT_EQ(second->orderId_, cancel->id_);
}

TEST_F(WsSessionTest, CancelWhoseClOrdIdBelongsToAnotherOrderIsRefused)
{
    restoreOrder(IdT(3, 1790000000), "CL-3");
    restoreOrder(IdT(7, 1790000100), "CL-7");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"cancel_order","data":{"orderId":3,"clOrderId":"CL-7"}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("Order 3 does not have ClOrdID CL-7", reply->value("message", ""));
    EXPECT_EQ(0u, inQueues_.size());
}

TEST_F(WsSessionTest, CancelByAnUnknownClOrdIdGoesToTheEngineAsUnknown)
{
    // The ClOrdID decides when given, so an unknown one is not looked up by number instead
    restoreOrder(IdT(7, 1790000100), "CL-7");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"cancel_order","data":{"orderId":7,"clOrderId":"NOPE"}})");
    auto cancel = takeQueuedCancel();
    ASSERT_TRUE(cancel.has_value()) << "no cancel queued; escaped: " << escaped();
    EXPECT_EQ(7u, cancel->id_.id_);
    EXPECT_FALSE(cancel->id_.isValid());
}

// =============================================================================
// Replacing orders (#74)
// =============================================================================

TEST_F(WsSessionTest, ReplaceBuildsTheReplacementFromTheOriginal)
{
    // The replacement is the original with the client's changes, a fresh id and a ClOrdID of its own, naming the
    // original (#74). Before, the session answered every replace with an error.
    OrderEntry *original = restoreOrder(IdT(7, 1790000100), "CL-7", BUY_SIDE, 150.0, 100);
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"replace_order","data":{"orderId":7,"price":151.0,"orderQty":80}})");
    auto replace = takeQueuedReplace();
    ASSERT_TRUE(replace.has_value()) << "no replace queued; escaped: " << escaped();
    EXPECT_EQ(original->orderId_, replace->first);
    ASSERT_NE(nullptr, replace->second);
    const OrderEntry &replacement = *replace->second;
    EXPECT_FALSE(replacement.orderId_.isValid());
    const RawDataEntry &clOrd = replacement.clOrderId_.get();
    const std::string clOrdId(clOrd.data_, clOrd.length_);
    EXPECT_TRUE(std::regex_match(clOrdId, std::regex("WS-[0-9]+-[0-9]+"))) << clOrdId;
    const RawDataEntry &origClOrd = replacement.origClOrderId_.get();
    EXPECT_EQ("CL-7", std::string(origClOrd.data_, origClOrd.length_));
    EXPECT_DOUBLE_EQ(151.0, replacement.price_);
    EXPECT_EQ(80u, replacement.orderQty_);
    EXPECT_EQ(BUY_SIDE, replacement.side_);
    EXPECT_EQ(original->instrument_.getId(), replacement.instrument_.getId());
}

TEST_F(WsSessionTest, OrdersWithAQuantityOutOfRangeAreRefused)
{
    // #86: on this branch "orderQty": -1 rested an order for 4294967295, and 1e20 one for nothing (an undefined
    // conversion). The parser now checks numbers before narrowing them, as master's does since #16.
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","price":150.0,)"
                 R"("orderQty":-1,"tif":"DAY","currency":"USD","capacity":"AGENCY"}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("Invalid message: invalid orderQty: expected an integer from 0 to 4294967295",
              reply->value("message", ""));

    send(client, R"({"type":"new_swap_order","data":{"symbol":"AAPL","side":"BUY","nearPrice":150.0,)"
                 R"("farPrice":151.0,"orderQty":1e20}})");
    reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("Invalid message: invalid orderQty: expected an integer from 0 to 4294967295",
              reply->value("message", ""));
    EXPECT_EQ(0u, inQueues_.size());
}

TEST_F(WsSessionTest, ReplaceThatChangesNothingIsRefused)
{
    // The frontend's Replace button sends no changes, until it has a form for them
    restoreOrder(IdT(7, 1790000100), "CL-7");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"replace_order","data":{"orderId":7}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("replace_order changes nothing: give price, orderQty or tif", reply->value("message", ""));
    EXPECT_EQ(0u, inQueues_.size());
}

TEST_F(WsSessionTest, ReplaceOfAnUnknownOrderIsRefused)
{
    restoreOrder(IdT(7, 1790000100), "CL-7");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"replace_order","data":{"orderId":999,"price":151.0}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("Order not found for replace: 999", reply->value("message", ""));
    EXPECT_EQ(0u, inQueues_.size());
}

TEST_F(WsSessionTest, ReplaceOfANumberTwoOrdersShareIsRefused)
{
    restoreOrder(IdT(7, 1790000100), "CL-7A");
    restoreOrder(IdT(7, 1790009999), "CL-7B");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"replace_order","data":{"orderId":7,"price":151.0}})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "escaped: " << escaped();
    EXPECT_EQ("More than one order has number 7: name it by clOrderId as well", reply->value("message", ""));
    EXPECT_EQ(0u, inQueues_.size());
}

TEST_F(WsSessionTest, ReplaceByClOrdIdNamesTheOrderEvenWhenItsNumberIsShared)
{
    restoreOrder(IdT(7, 1790000100), "CL-7A");
    OrderEntry *second = restoreOrder(IdT(7, 1790009999), "CL-7B");
    Client client;
    connect(client);
    ASSERT_TRUE(readUntil(client, "order_snapshot").has_value());

    send(client, R"({"type":"replace_order","data":{"orderId":7,"clOrderId":"CL-7B","price":151.0}})");
    auto replace = takeQueuedReplace();
    ASSERT_TRUE(replace.has_value()) << "no replace queued; escaped: " << escaped();
    EXPECT_EQ(second->orderId_, replace->first);
}

} // namespace
