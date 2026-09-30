/**
 Concurrent Order Processor library - WebSocket session Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "MockStorage.h"
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

        endpoint_ = acceptor_.local_endpoint();
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    std::make_shared<App::WsSession>(std::move(socket), &sessionMgr_, WideDataStorage::instance(),
                                                     OrderStorage::instance(), &inQueues_, IdTGenerator::instance(),
                                                     nullptr)
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

} // namespace
