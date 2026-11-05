/**
 Concurrent Order Processor library - WebSocket session read Benchmarks

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "IdTGenerator.h"
#include "InFlightGauge.h"
#include "IncomingQueues.h"
#include "Logger.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "SessionManager.h"
#include "TransactionMgr.h"
#include "WideDataStorage.h"
#include "WsServer.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace COP;

namespace
{

/// The messages a flooding client sends at once. The IO thread then reads most of them from what it has already
/// received, so the work around each message is nearly all it does, and a check made after each costs the largest share.
const u32 BURST = 64;

/// A cancel of an order number no order has: the lightest message that reaches the engine's queue (the parse, a lookup
/// in an empty store, the push), and one that leaves nothing behind, where a new order's strings and execution list
/// stay in WideDataStorage. The session answers it with nothing: the engine's cancel_reject would.
const std::string CANCEL = R"({"type":"cancel_order","data":{"orderId":987654321}})";

/// How long a step may wait before the benchmark gives up, so a broken session fails it instead of hanging
const std::chrono::seconds STEP_TIMEOUT(5);

/// The singletons a session reaches. They are made before the members that log as they are made, IncomingQueues among
/// them, and go after them. Notes, warnings and debug lines are off, as in the other benchmarks that log.
struct Singletons
{
    Singletons()
    {
        aux::ExchLogger::create();
        aux::ExchLogger::instance()->setNoteOn(false);
        aux::ExchLogger::instance()->setWarnOn(false);
        aux::ExchLogger::instance()->setDebugOn(false);
        Store::WideDataStorage::create();
        IdTGenerator::create();
        Store::OrderStorage::create();
    }
    ~Singletons()
    {
        Store::OrderStorage::destroy();
        IdTGenerator::destroy();
        Store::WideDataStorage::destroy();
        aux::ExchLogger::destroy();
    }
};

/// A real WsServer on loopback, as app/main.cpp serves it, with one client connected, both on the benchmark's thread.
/// The server's io_context runs only when the benchmark serves: the client writes with blocking calls before that, and
/// a burst fits in the sockets' buffers. The sessions check the engine's work in flight, the queue's events and a
/// transaction manager's transactions, none, against the server's default bound (#519).
class ServedClient
{
public:
    ServedClient()
    {
        book_.init(OrderBookImpl::InstrumentsT());
        transactMgr_.init(ACID::TransactionMgrParams(IdTGenerator::instance()));

        server_ = std::make_shared<App::WsServer>(
            serverIoc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), &sessions_,
            Store::WideDataStorage::instance(), Store::OrderStorage::instance(), &inQueues_, IdTGenerator::instance(),
            &book_, SourceIdT(), SourceIdT(), App::WsLimits(), App::WsAuth(), &inFlight_);
        server_->run();
        connected_ = connect();
    }
    ~ServedClient()
    {
        beast::error_code ec;
        beast::get_lowest_layer(client_).socket().close(ec);
        serverIoc_.stop();
        server_.reset();
        transactMgr_.stop();
    }

    bool connected() const
    {
        return connected_;
    }

    /// Sends a burst of cancels, each a message of its own
    void sendBurst()
    {
        client_.text(true);
        for (u32 i = 0; i < BURST; ++i)
        {
            client_.write(net::buffer(CANCEL));
        }
    }

    /// Runs the server until the session has queued a burst's events: true, unless it stopped first
    bool serveBurst()
    {
        while (inQueues_.size() < BURST)
        {
            if (0 == serverIoc_.run_one_for(STEP_TIMEOUT))
            {
                return false;
            }
        }
        return true;
    }

    /// How many events wait in the engine's queue
    u32 queued() const
    {
        return inQueues_.size();
    }

    /// Empties the engine's queue
    void drain()
    {
        while (inQueues_.pop())
        {
        }
    }

private:
    /// Connects the client and completes its handshake, then lets the server send the state a client gets as it
    /// connects, so that none of the session's own writes is left for the timed spans
    bool connect()
    {
        beast::error_code ec;
        beast::get_lowest_layer(client_).socket().connect(server_->localEndpoint(), ec);
        if (ec)
        {
            return false;
        }
        std::optional<beast::error_code> handshake;
        client_.async_handshake("127.0.0.1", "/ws",
                                [&handshake](beast::error_code result)
                                {
                                    handshake = result;
                                });
        const auto deadline = std::chrono::steady_clock::now() + STEP_TIMEOUT;
        while (!handshake && (std::chrono::steady_clock::now() < deadline))
        {
            serverIoc_.poll();
            clientIoc_.poll();
        }
        if (!handshake || *handshake)
        {
            return false;
        }
        while (0 != serverIoc_.poll())
        {
        }
        return true;
    }

    Singletons singletons_;
    net::io_context serverIoc_;
    net::io_context clientIoc_;
    App::SessionManager sessions_;
    Queues::IncomingQueues inQueues_;
    ACID::TransactionMgr transactMgr_;
    App::EngineInFlightGauge inFlight_{ &inQueues_, &transactMgr_, App::DEFAULT_MAX_IN_FLIGHT };
    OrderBookImpl book_;
    std::shared_ptr<App::WsServer> server_;
    websocket::stream<beast::tcp_stream> client_{ clientIoc_ };
    bool connected_ = false;
};

} // namespace

/// The IO thread's work for each message a flooding client sends: a WebSocket session's read of a burst of cancels, each
/// read from the socket or Beast's buffer, parsed, handed to the engine's queue, and followed by the next read. The span
/// is one burst's (manual time): the client's writes come before it, and the queue is emptied after it. Nothing timed a
/// session's read until #519, which checks the engine's work in flight before each next read.
static void BM_WsSessionReadFlood(benchmark::State &state)
{
    ServedClient served;
    if (!served.connected())
    {
        state.SkipWithError("the client couldn't connect");
        return;
    }
    // Each cancel must reach the queue: a message the session answered itself would time another path
    served.sendBurst();
    if (!served.serveBurst() || (BURST != served.queued()))
    {
        state.SkipWithError("the session didn't queue each cancel of a burst");
        return;
    }
    served.drain();

    for (auto _ : state)
    {
        served.sendBurst();
        const auto start = std::chrono::steady_clock::now();
        const bool ok = served.serveBurst();
        const auto end = std::chrono::steady_clock::now();
        if (!ok)
        {
            state.SkipWithError("the session stopped queueing the burst's cancels");
            break;
        }
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
        served.drain();
    }
    state.SetItemsProcessed(state.iterations() * BURST);
}
BENCHMARK(BM_WsSessionReadFlood)->UseManualTime();
