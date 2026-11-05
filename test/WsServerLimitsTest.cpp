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
#include "InFlightGauge.h"
#include "IncomingQueues.h"
#include "MockTransaction.h"
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

/// A cancel of order number n, which no order has: the session hands it to the engine's queue as it is
std::string cancelOf(u64 n)
{
    return R"({"type":"cancel_order","data":{"orderId":)" + std::to_string(n) + "}}";
}

/// The work in flight as a test holds it, past the bound or under it (#519)
class HeldGauge : public App::InFlightGauge
{
public:
    bool overBound() override
    {
        ++asked;
        return over.load();
    }
    std::atomic<bool> over{ false };
    /// How often the sessions asked
    std::atomic<std::size_t> asked{ 0 };
};

/// The order numbers of the cancels taken from the engine's queue, in the order the sessions queued them
class CancelRecorder : public Queues::InQueueProcessor
{
public:
    bool process() override
    {
        return false;
    }
    void onEvent(const std::string &, const Queues::OrderEvent &) override {}
    void onEvent(const std::string &, const Queues::OrderCancelEvent &evnt) override
    {
        numbers.push_back(evnt.id_.id_);
    }
    void onEvent(const std::string &, const Queues::OrderReplaceEvent &) override {}
    void onEvent(const std::string &, const Queues::OrderChangeStateEvent &) override {}
    void onEvent(const std::string &, const Queues::ProcessEvent &) override {}
    void onEvent(const std::string &, const Queues::TimerEvent &) override {}

    std::vector<u64> numbers;
};

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

    void start(const App::WsLimits &limits = App::WsLimits(), App::InFlightGauge *inFlight = nullptr)
    {
        server_ = std::make_shared<App::WsServer>(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                                                  &sessionMgr_, WideDataStorage::instance(), OrderStorage::instance(),
                                                  &inQueues_, IdTGenerator::instance(), &orderBook_, SourceIdT(),
                                                  SourceIdT(), limits, App::WsAuth(), inFlight);
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

    /// Waits until done() holds: true, unless the time ran out first
    template <typename Done> static bool waitUntil(Done done, std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done() && (std::chrono::steady_clock::now() < deadline))
        {
            std::this_thread::sleep_for(1ms);
        }
        return done();
    }

    /// Waits until the engine's queue has held its size for a while: the sessions have stopped queueing. Its size.
    u32 waitForSteadyQueue(std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        u32 size = inQueues_.size();
        auto since = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(10ms);
            const u32 now = inQueues_.size();
            if (now != size)
            {
                size = now;
                since = std::chrono::steady_clock::now();
            }
            else if (std::chrono::steady_clock::now() - since >= 200ms)
            {
                break;
            }
        }
        return size;
    }

    /// Takes the engine's queue's events as the engine would, recording the cancels' numbers, until count have come
    bool takeCancels(CancelRecorder *recorder, std::size_t count, std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while ((recorder->numbers.size() < count) && (std::chrono::steady_clock::now() < deadline))
        {
            if (!inQueues_.pop(recorder))
            {
                std::this_thread::sleep_for(1ms);
            }
        }
        return recorder->numbers.size() == count;
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

    /// Stores orders numbered first to last, as restore does, each with a ClOrdID of its own, for a new client's order
    /// snapshot (#512)
    static void storeOrders(std::size_t first, std::size_t last)
    {
        const SourceIdT instrument = WideDataStorage::instance()->findInstrumentBySymbol("AAPL");
        for (std::size_t i = first; i <= last; ++i)
        {
            const std::string clOrdId = "CL-" + std::to_string(i);
            const SourceIdT clOrderId = WideDataStorage::instance()->add(
                new RawDataEntry(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size())));
            auto *order = new OrderEntry(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrument, SourceIdT(),
                                         SourceIdT(), SourceIdT());
            order->orderId_ = IdT(i, 1);
            OrderStorage::instance()->restore(order);
        }
    }

    net::io_context ioc_{ 1 };
    std::thread serverThread_;
    std::shared_ptr<App::WsServer> server_;
    tcp::endpoint endpoint_;
    App::SessionManager sessionMgr_;
    Queues::IncomingQueues inQueues_;
    OrderBookImpl orderBook_;
    /// What the tests hold the work in flight with (#519). Members, as the server's thread asks them until TearDown()
    /// stops it.
    HeldGauge heldGauge_;
    ::testing::NiceMock<test::MockTransactionManager> noTransactions_;
    std::unique_ptr<App::EngineInFlightGauge> engineGauge_;
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

TEST_F(WsServerLimitsTest, AnOrderSnapshotLargerThanTheLimitIsSentWhole)
{
    // The state a client gets as it connects was counted against the limit, and queued behind its first message, whose
    // write can't complete while the rest is queued: so an order snapshot larger than the limit dropped every client as
    // it connected, for good once the store had grown past it (#512). The limit counts what waits after that state now.
    // Here it is 1 MiB. A client that reads connects to 5,000 orders, a snapshot of about 2.3 MB, and gets it whole. One
    // that doesn't read connects to 10,000, about 4.6 MB, with a receive buffer of 4 KiB: the kernel then holds under
    // 2 MB of it on loopback, so the rest still waits when the broadcasts come. Building a snapshot takes seconds under
    // the sanitizers, on the server's one IO thread, so the waits for it are long.
    App::WsLimits limits;
    limits.maxQueuedBytes = 1 << 20;
    storeOrders(1, 5000);
    start(limits);

    Client reader;
    ASSERT_TRUE(connect(reader, 60s));
    auto snapshot = readUntil(reader, "order_snapshot", 60s);
    ASSERT_TRUE(snapshot.has_value()) << "the client was dropped as it connected";
    EXPECT_EQ(5000u, (*snapshot)["data"].size());
    ASSERT_LT(limits.maxQueuedBytes, snapshot->dump().size()) << "the snapshot isn't larger than the limit";
    storeOrders(5001, 10000);
    Client sleeper;
    beast::get_lowest_layer(sleeper.ws).socket().open(tcp::v4());
    beast::get_lowest_layer(sleeper.ws).socket().set_option(net::socket_base::receive_buffer_size(4096));
    ASSERT_TRUE(connect(sleeper, 60s));
    ASSERT_TRUE(waitForSessions(2));
    // Answered once the IO thread has built and queued the sleeper's initial state
    send(reader, R"({"type":"no_such_type"})");
    ASSERT_TRUE(readUntil(reader, "error", 60s).has_value()) << "the client that reads isn't answered";

    // 8 MiB of broadcasts in bursts of eight messages of 64 KiB, each burst read before the next. The client that reads
    // gets eight times the limit, but never has more than 512 KiB waiting, though a burst queues eight messages; the one
    // that doesn't is dropped once 1 MiB waits behind its snapshot.
    const std::string payload = R"({"type":"test","pad":")" + std::string(64 * 1024, 'x') + R"("})";
    const int bursts = 16;
    const int burst = 8;
    int received = 0;
    for (int b = 0; (b < bursts) && (received == b * burst); ++b)
    {
        for (int i = 0; i < burst; ++i)
        {
            sessionMgr_.broadcast(payload);
        }
        for (int i = 0; (i < burst) && readUntil(reader, "test"); ++i)
        {
            ++received;
        }
    }
    EXPECT_EQ(bursts * burst, received) << "the client that reads lost broadcasts";
    EXPECT_TRUE(waitForSessions(1, 10s)) << "the client that doesn't read is still connected";
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

TEST_F(WsServerLimitsTest, AFloodingClientIsReadOnlyWhileTheWorkInFlightIsUnderTheBound)
{
    // A client could send faster than the engine decides and LMDB commits, and the server read it all into the engine's
    // memory, without limit (#519). Here the bound is 50, and nothing takes the queue's events but the test: the session
    // stops reading once 50 wait, give or take a millisecond's reads, and reads the rest as the test takes them.
    engineGauge_ = std::make_unique<App::EngineInFlightGauge>(&inQueues_, &noTransactions_, 50); // none pending
    start(App::WsLimits(), engineGauge_.get());
    Client flooder;
    ASSERT_TRUE(connect(flooder));
    const std::size_t sent = 500;
    for (std::size_t i = 1; i <= sent; ++i)
    {
        send(flooder, cancelOf(i));
    }

    const u32 held = waitForSteadyQueue();
    EXPECT_LE(50u, held);
    EXPECT_GE(300u, held) << "the session read past the bound by more than a millisecond's reads";

    CancelRecorder recorder;
    ASSERT_TRUE(takeCancels(&recorder, sent)) << recorder.numbers.size() << " of " << sent << " cancels came";
    for (std::size_t i = 0; i < sent; ++i)
    {
        ASSERT_EQ(i + 1, recorder.numbers[i]);
    }
    EXPECT_TRUE(stillAnswers(flooder));
}

TEST_F(WsServerLimitsTest, AClientIsReadInItsTurnWhenTheWorkInFlightIsBackUnderTheBound)
{
    // Past the bound every session waits, from its first read on. Back under it, each reads one message a turn, so a
    // client's cancel isn't read after everything a flooding client sent first (#519).
    heldGauge_.over = true;
    start(App::WsLimits(), &heldGauge_);
    Client flooder;
    Client other;
    ASSERT_TRUE(connect(flooder));
    ASSERT_TRUE(connect(other));
    ASSERT_TRUE(waitForSessions(2));
    const std::size_t flood = 2000;
    for (std::size_t i = 1; i <= flood; ++i)
    {
        send(flooder, cancelOf(i));
    }
    const u64 its = 999999;
    send(other, cancelOf(its));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(0u, inQueues_.size()) << "a session read past the bound";

    heldGauge_.over = false;
    CancelRecorder recorder;
    ASSERT_TRUE(takeCancels(&recorder, flood + 1)) << recorder.numbers.size() << " of " << flood + 1 << " cancels came";
    const auto at = std::find(recorder.numbers.begin(), recorder.numbers.end(), its) - recorder.numbers.begin();
    EXPECT_GT(static_cast<std::ptrdiff_t>(flood), at) << "the other client's cancel came after the whole flood";
}

TEST_F(WsServerLimitsTest, TheIdleTimeoutStopsOnlyWhileASessionWaitsForTheWorkInFlight)
{
    // Each read starts Beast's idle timeout, which pings the client at half the time and ends the session at the end
    // unless something has come, the ping's answer included. A waiting session reads nothing, so the timeout would end
    // it, losing what its client had sent: the timeout doesn't run while the session waits (#519), and runs again once
    // it reads. Here it is 1 s, and the session waits 2 s after a read.
    App::WsLimits limits;
    limits.idleTimeout = std::chrono::seconds(1);
    start(limits, &heldGauge_);
    Client client;
    ASSERT_TRUE(connect(client));
    send(client, cancelOf(6));
    // Read, and the session reads on: it asked as it connected, and again after the read
    ASSERT_TRUE(waitUntil(
        [this]
        {
            return (1u == inQueues_.size()) && (2u <= heldGauge_.asked.load());
        }));

    heldGauge_.over = true;
    send(client, cancelOf(7));
    // Read, and the session waits
    ASSERT_TRUE(waitUntil(
        [this]
        {
            return (2u == inQueues_.size()) && (3u <= heldGauge_.asked.load());
        }));
    send(client, cancelOf(8));
    std::this_thread::sleep_for(2s);
    EXPECT_EQ(2u, inQueues_.size()) << "the session read past the bound";

    heldGauge_.over = false;
    CancelRecorder recorder;
    ASSERT_TRUE(takeCancels(&recorder, 3)) << "the session ended while it waited";
    EXPECT_EQ((std::vector<u64>{ 6, 7, 8 }), recorder.numbers);
    EXPECT_TRUE(stillAnswers(client));

    // The client reads nothing more, so it answers no ping
    EXPECT_TRUE(waitForSessions(0)) << "the idle timeout didn't run again once the session read";
}

TEST_F(WsServerLimitsTest, ASessionWaitingForTheWorkInFlightEndsWhenAWriteToItsClientFails)
{
    // A waiting session has no read to fail when its client goes: it ends at the first write that fails, and frees its
    // connection then, not once the work in flight is back under the bound (#519). Here one client may connect at once.
    App::WsLimits limits;
    limits.maxConnections = 1;
    heldGauge_.over = true;
    start(limits, &heldGauge_);
    {
        Client gone;
        ASSERT_TRUE(connect(gone));
        ASSERT_TRUE(waitForSessions(1));
        beast::get_lowest_layer(gone.ws).socket().close();
    }
    const auto deadline = std::chrono::steady_clock::now() + REPLY_TIMEOUT;
    while ((0 != sessionMgr_.sessionCount()) && (std::chrono::steady_clock::now() < deadline))
    {
        sessionMgr_.broadcast(R"({"type":"metrics_update","data":{}})");
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(waitForSessions(0));

    EXPECT_NE(nullptr, connectEventually()) << "the session that waited still holds its connection";
}

} // namespace
