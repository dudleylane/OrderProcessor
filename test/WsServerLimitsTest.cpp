/**
 Concurrent Order Processor library - WebSocket server limits Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <sys/resource.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "IncomingQueues.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "ServerDefaults.h"
#include "SessionManager.h"
#include "WideDataStorage.h"
#include "WsServer.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

using namespace COP;
using namespace COP::Store;

namespace
{

const std::chrono::milliseconds REPLY_TIMEOUT(5000);

/// A real WsServer on loopback, served by its own io_context thread as app/main.cpp serves it, with the limits a test
/// sets (#159, #202)
class WsServerLimitsTest : public ::testing::Test
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
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(WideDataStorage::instance()->add(instr));
        orderBook_.init(instruments);
    }

    void TearDown() override
    {
        ioc_.stop();
        if (serverThread_.joinable())
        {
            serverThread_.join();
        }
        server_.reset();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    void start(const App::WsLimits &limits = App::WsLimits())
    {
        server_ =
            std::make_shared<App::WsServer>(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), &sessionMgr_,
                                            WideDataStorage::instance(), OrderStorage::instance(), &inQueues_,
                                            IdTGenerator::instance(), &orderBook_, SourceIdT(), SourceIdT(), limits);
        endpoint_ = server_->localEndpoint();
        server_->run();
        serverThread_ = std::thread(
            [this]
            {
                ioc_.run();
            });
    }

    struct Client
    {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ ioc };
        beast::flat_buffer buffer;
    };

    /// Connects, and is true once the server has answered the handshake and sent its first message. The kernel
    /// completes the TCP connection whether or not the server accepts it, so a server that refuses it, or never
    /// accepts it, makes this false.
    bool connect(Client &client, std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        beast::error_code ec;
        beast::get_lowest_layer(client.ws).socket().connect(endpoint_, ec);
        if (ec)
        {
            return false;
        }
        std::optional<beast::error_code> handshake;
        client.ws.async_handshake("127.0.0.1", "/ws",
                                  [&handshake](beast::error_code result)
                                  {
                                      handshake = result;
                                  });
        client.ioc.restart();
        client.ioc.run_for(timeout);
        return handshake && !*handshake && readUntil(client, "connected", timeout).has_value();
    }

    /// A new client, connected; retries until the timeout, for a server that frees room for it asynchronously
    std::unique_ptr<Client> connectEventually(std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto client = std::make_unique<Client>();
            if (connect(*client, 500ms))
            {
                return client;
            }
        }
        return nullptr;
    }

    static void send(Client &client, const std::string &text)
    {
        client.ws.write(net::buffer(text));
    }

    /// Reads messages until one of the given type arrives. Bounded: a server that has stopped sends nothing more.
    static std::optional<nlohmann::json> readUntil(Client &client, const std::string &type,
                                                   std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
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

    /// True once a read fails: the server ended the session. Messages it sent before that are skipped.
    static bool closedByServer(Client &client, std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::optional<beast::error_code> result;
            client.buffer.clear();
            client.ws.async_read(client.buffer,
                                 [&result](beast::error_code ec, std::size_t)
                                 {
                                     result = ec;
                                 });
            client.ioc.restart();
            client.ioc.run_for(deadline - std::chrono::steady_clock::now());
            if (!result)
            {
                return false; // still open: nothing arrived before the timeout
            }
            if (*result)
            {
                return true;
            }
        }
        return false;
    }

    /// The session still reads and answers: an unknown type gets the ordinary error reply
    static bool stillAnswers(Client &client)
    {
        send(client, R"({"type":"no_such_type"})");
        auto reply = readUntil(client, "error");
        return reply.has_value() && std::string::npos != reply->value("message", "").find("Unknown message type");
    }

    bool waitForSessions(std::size_t count, std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (sessionMgr_.sessionCount() != count && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(10ms);
        }
        return sessionMgr_.sessionCount() == count;
    }

    net::io_context ioc_{ 1 };
    std::thread serverThread_;
    std::shared_ptr<App::WsServer> server_;
    tcp::endpoint endpoint_;
    App::SessionManager sessionMgr_;
    Queues::IncomingQueues inQueues_;
    OrderBookImpl orderBook_;
};

TEST_F(WsServerLimitsTest, AClientThatDoesNotReadIsDroppedOnceItsQueueIsFull)
{
    // Every broadcast goes to every client, so one that connects and stops reading used to make the server keep all of
    // them, without limit (#159). Here the limit is 1 MiB, and 32 MiB is broadcast, paced to a client that reads: no
    // more than four messages, 256 KiB, wait for it. (A burst larger than the limit drops a client that reads too: a
    // byte limit can't tell the two apart. It reconnects and gets the current state again.)
    App::WsLimits limits;
    limits.maxQueuedBytes = 1 << 20;
    start(limits);
    Client reader;
    Client sleeper;
    ASSERT_TRUE(connect(reader));
    ASSERT_TRUE(connect(sleeper));
    ASSERT_TRUE(waitForSessions(2));

    const int broadcasts = 512;
    std::atomic<int> received{ 0 };
    std::thread readerThread(
        [&]
        {
            while (received < broadcasts && readUntil(reader, "test", 10s))
            {
                ++received;
            }
        });
    const std::string payload = R"({"type":"test","pad":")" + std::string(64 * 1024, 'x') + R"("})";
    for (int i = 0; i < broadcasts; ++i)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (received + 4 < i && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(100us);
        }
        sessionMgr_.broadcast(payload);
    }

    EXPECT_TRUE(waitForSessions(1, 10s)) << "the client that doesn't read is still connected";
    readerThread.join();
    EXPECT_EQ(broadcasts, received.load()) << "the client that reads lost broadcasts";
}

TEST_F(WsServerLimitsTest, AMessageLargerThanTheLimitEndsItsSession)
{
    // A client could send one message of up to 16 MiB, Beast's default, which the server read into memory whole (#159)
    start();
    Client client;
    ASSERT_TRUE(connect(client));

    const std::string under = R"({"type":"no_such_type","pad":")" + std::string(60 * 1024, 'x') + R"("})";
    send(client, under);
    EXPECT_TRUE(readUntil(client, "error").has_value()) << "a message under the limit wasn't answered";

    const std::string over =
        R"({"type":"no_such_type","pad":")" + std::string(App::MAX_CLIENT_MESSAGE_BYTES, 'x') + R"("})";
    beast::error_code ec;
    client.ws.write(net::buffer(over), ec); // the server may close the connection before it has read it all
    EXPECT_TRUE(closedByServer(client)) << "a message over the limit was taken";
}

TEST_F(WsServerLimitsTest, SubscribingToAnUnknownSymbolIsRefused)
{
    // Any string was kept as a subscription, so a client could grow its set without limit (#159)
    start();
    Client client;
    ASSERT_TRUE(connect(client));

    send(client, R"({"type":"subscribe_book","symbol":"NOPE"})");
    auto reply = readUntil(client, "error");
    ASSERT_TRUE(reply.has_value()) << "a subscription to an unknown symbol got no answer";
    EXPECT_EQ("Unknown instrument: NOPE", reply->value("message", ""));

    send(client, R"({"type":"subscribe_book","symbol":"AAPL"})");
    EXPECT_TRUE(readUntil(client, "book_update").has_value()) << "a known symbol's book wasn't sent";
}

TEST_F(WsServerLimitsTest, AConnectionBeyondTheLimitIsClosed)
{
    // Nothing limited connections (#159)
    App::WsLimits limits;
    limits.maxConnections = 2;
    start(limits);
    Client first;
    Client second;
    ASSERT_TRUE(connect(first));
    ASSERT_TRUE(connect(second));

    Client third;
    EXPECT_FALSE(connect(third, 2s)) << "a connection beyond the limit was served";

    // Once one closes, there is room again
    first.ws.close(websocket::close_code::normal);
    EXPECT_NE(nullptr, connectEventually()) << "a connection within the limit wasn't served";
}

TEST_F(WsServerLimitsTest, BookUpdatesGoOutWhileAClientSubscribes)
{
    // broadcastBookUpdate, on a transaction worker, read a session's subscriptions while the session's own thread
    // changed them: a data race, which ThreadSanitizer reports (#159). The test passes in any other build.
    start();
    Client client;
    ASSERT_TRUE(connect(client));

    std::atomic<bool> stop{ false };
    std::thread broadcaster(
        [&]
        {
            for (int i = 0; i < 20000 && !stop; ++i)
            {
                sessionMgr_.broadcastBookUpdate("AAPL", R"({"type":"book_update","symbol":"AAPL"})");
                std::this_thread::sleep_for(20us);
            }
        });
    for (int i = 0; i < 200; ++i)
    {
        send(client, R"({"type":"subscribe_book","symbol":"AAPL"})");
        send(client, R"({"type":"unsubscribe_book","symbol":"AAPL"})");
    }
    stop = true;
    broadcaster.join();
    EXPECT_TRUE(stillAnswers(client));
}

TEST_F(WsServerLimitsTest, AcceptingGoesOnAfterTheProcessRunsOutOfDescriptors)
{
    // One failed accept stopped the server accepting for good: onAccept returned without accepting again (#202). Here
    // the process has no descriptor free while a client connects, so the server's accept fails with EMFILE.
    start();
    Client starved;
    Client later;
    beast::get_lowest_layer(starved.ws).socket().open(tcp::v4()); // its descriptor, before there is none

    rlimit saved{};
    ASSERT_EQ(0, ::getrlimit(RLIMIT_NOFILE, &saved));
    const int lowestFree = ::dup(0);
    ASSERT_GE(lowestFree, 0);
    ::close(lowestFree);
    rlimit none = saved;
    none.rlim_cur = static_cast<rlim_t>(lowestFree);
    ASSERT_EQ(0, ::setrlimit(RLIMIT_NOFILE, &none));

    beast::error_code ec;
    beast::get_lowest_layer(starved.ws).socket().connect(endpoint_, ec); // the kernel completes it
    std::this_thread::sleep_for(300ms);                                  // the server's accept fails meanwhile
    ASSERT_EQ(0, ::setrlimit(RLIMIT_NOFILE, &saved));
    beast::get_lowest_layer(starved.ws).socket().close();
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_TRUE(connect(later)) << "the server stopped accepting after running out of descriptors";
}

} // namespace
