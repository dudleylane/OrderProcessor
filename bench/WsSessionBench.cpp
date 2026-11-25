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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
/// received, so the work around each message is nearly all it does, and a check made after each costs the largest
/// share.
const u32 BURST = 64;

/// A cancel of an order number no order has: the lightest message that reaches the engine's queue (the parse, a lookup
/// in an empty store, the push), and one that leaves nothing behind, where a new order's strings and execution list
/// stay in WideDataStorage. The session answers it with nothing: the engine's cancel_reject would.
const std::string CANCEL = R"({"type":"cancel_order","data":{"orderId":987654321}})";

/// A message of a type the protocol doesn't have, which the session answers itself, with an error, and doesn't hand to
/// the engine: the lightest message whose answer goes through the session's send() and its write's completion
const std::string UNKNOWN = R"({"type":"no_such_request","data":{}})";

/// A limit order as the order entry panel sends one, with the id it gives each request (#683): one the session takes,
/// whose ClOrdID, source, destination and execution list stay in WideDataStorage, so the store grows with the
/// iterations, as the engine benchmarks' stores do
const std::string NEW_ORDER = R"({"type":"new_order","data":{"symbol":"BENCHEQ","side":"BUY","ordType":"LIMIT",)"
                              R"("price":150.25,"orderQty":100,"tif":"DAY","currency":"USD","capacity":"AGENCY",)"
                              R"("requestId":"r3Kf9_x-Lq2A"}})";
/// The same order without a request id, as a client that names no request sends it
const std::string NEW_ORDER_WITHOUT_ID = R"({"type":"new_order","data":{"symbol":"BENCHEQ","side":"BUY",)"
                                         R"("ordType":"LIMIT","price":150.25,"orderQty":100,"tif":"DAY",)"
                                         R"("currency":"USD","capacity":"AGENCY"}})";

/// What a client's bursts hold: cancels, or new orders, which need an instrument, an account and a clearing firm
enum class Burst
{
    CANCELS,
    ORDERS
};

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
/// transaction manager's transactions, none, against the server's default bound (#519). A client that sends new orders
/// is served with what an order needs: its instrument, and the server's default account and clearing firm.
class ServedClient
{
public:
    explicit ServedClient(Burst burst = Burst::CANCELS)
    {
        book_.init(OrderBookImpl::InstrumentsT());
        transactMgr_.init(ACID::TransactionMgrParams(IdTGenerator::instance()));

        SourceIdT accountId;
        SourceIdT clearingId;
        if (Burst::ORDERS == burst)
        {
            auto *instr = new InstrumentEntry();
            instr->symbol_ = "BENCHEQ";
            instr->securityId_ = "SEC1";
            instr->securityIdSource_ = "SRC1";
            Store::WideDataStorage::instance()->add(instr);
            auto *acct = new AccountEntry();
            acct->account_ = "BENCHACCT";
            acct->firm_ = "BENCHFIRM";
            acct->type_ = PRINCIPAL_ACCOUNTTYPE;
            accountId = Store::WideDataStorage::instance()->add(acct);
            auto *clr = new ClearingEntry();
            clr->firm_ = "BENCHCLR";
            clearingId = Store::WideDataStorage::instance()->add(clr);
        }

        server_ = std::make_shared<App::WsServer>(
            serverIoc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), &sessions_,
            Store::WideDataStorage::instance(), Store::OrderStorage::instance(), &inQueues_, IdTGenerator::instance(),
            &book_, accountId, clearingId, App::WsLimits(), App::WsAuth(), &inFlight_);
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

    /// Sends a burst of the message, cancels unless told otherwise, each a message of its own
    void sendBurst(const std::string &message = CANCEL)
    {
        client_.text(true);
        for (u32 i = 0; i < BURST; ++i)
        {
            client_.write(net::buffer(message));
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

    /// Runs the server, and the client's reads, until the client has read an answer to each message of a burst: true,
    /// unless a read failed or the step's time ran out first
    bool serveAnswers()
    {
        answers_ = 0;
        readFailed_ = false;
        // The client's context ran out of work once its handshake was done, and a context out of work is stopped: it
        // runs nothing more until it is restarted
        clientIoc_.restart();
        readAnswer();
        const auto deadline = std::chrono::steady_clock::now() + STEP_TIMEOUT;
        while ((answers_ < BURST) && !readFailed_ && (std::chrono::steady_clock::now() < deadline))
        {
            serverIoc_.poll();
            clientIoc_.poll();
        }
        return (BURST == answers_) && !readFailed_;
    }

    /// Has the client read, and drop, whatever the session sent it, up to its answer to one message it answers itself,
    /// sent after the burst: so that no answer of a burst waits into the next burst's span. Untimed, after a span.
    /// False unless that answer came.
    bool skipAnswers()
    {
        return skipUpToAnswer(false);
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
    /// Reads the client's next answer, and once it has, the one after, until a burst's are read
    void readAnswer()
    {
        client_.async_read(answer_,
                           [this](beast::error_code ec, std::size_t)
                           {
                               if (ec)
                               {
                                   readFailed_ = true;
                                   return;
                               }
                               answer_.clear();
                               if (++answers_ < BURST)
                               {
                                   readAnswer();
                               }
                           });
    }

    /// Connects the client and completes its handshake, then lets the server send the state a client gets as it
    /// connects, and has the client read it, so that none of the session's own writes is left for the timed spans and
    /// the first answer a span reads is its own burst's
    bool connect()
    {
        beast::error_code ec;
        beast::get_lowest_layer(client_).socket().connect(server_->localEndpoint(), ec);
        if (ec)
        {
            return false;
        }
        // Each message of a burst goes out as it is written, as the server's own writes do (#653). With Nagle's
        // algorithm the messages after a burst's first waited for its ACK, which the server's kernel delays, up to
        // 40 ms, while the session sends nothing back: BM_WsSessionOrderFlood then timed 41 ms a burst, 1.5 ms of it
        // CPU, and a session that answered each order released the burst early and looked 97% faster.
        beast::get_lowest_layer(client_).socket().set_option(tcp::no_delay(true), ec);
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
        return skipConnectState();
    }

    /// Reads, and drops, the state the session sends a client as it connects (WsSession::onAccept()), and then the
    /// answer to one message the session answers itself, which comes after it. Unread, that state's messages would be
    /// counted as the first burst's answers, and every span would then end that many answers before its own burst's
    /// last. False unless the state ended with the order snapshot, so that one cut short by an error fails here.
    bool skipConnectState()
    {
        return skipUpToAnswer(true);
    }

    /// Sends one message the session answers itself, and reads, and drops, everything up to its answer: true once the
    /// answer came, and, when asked, an order snapshot before it
    bool skipUpToAnswer(bool needSnapshot)
    {
        client_.text(true);
        client_.write(net::buffer(UNKNOWN));
        bool snapshot = false;
        bool answered = false;
        bool failed = false;
        std::function<void()> next = [&]
        {
            client_.async_read(answer_,
                               [&](beast::error_code ec, std::size_t)
                               {
                                   if (ec)
                                   {
                                       failed = true;
                                       return;
                                   }
                                   const std::string_view text(static_cast<const char *>(answer_.data().data()),
                                                               answer_.size());
                                   if (std::string_view::npos != text.find(R"("type":"order_snapshot")"))
                                   {
                                       snapshot = true;
                                   }
                                   else if (std::string_view::npos != text.find(R"("type":"error")"))
                                   {
                                       answered = true;
                                   }
                                   answer_.clear();
                                   if (!answered)
                                   {
                                       next();
                                   }
                               });
        };
        // The handshake's poll left the client's context out of work, and so stopped
        clientIoc_.restart();
        next();
        const auto deadline = std::chrono::steady_clock::now() + STEP_TIMEOUT;
        while (!answered && !failed && (std::chrono::steady_clock::now() < deadline))
        {
            serverIoc_.poll();
            clientIoc_.poll();
        }
        return (snapshot || !needSnapshot) && answered && !failed;
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
    beast::flat_buffer answer_;
    u32 answers_ = 0;
    bool readFailed_ = false;
    bool connected_ = false;
};

} // namespace

/// The IO thread's work for each message a flooding client sends: a WebSocket session's read of a burst of cancels,
/// each read from the socket or Beast's buffer, parsed, handed to the engine's queue, and followed by the next read.
/// The span is one burst's (manual time): the client's writes come before it, and the queue is emptied after it.
/// Nothing timed a session's read until #519, which checks the engine's work in flight before each next read.
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

/// The IO thread's work for each message a client sends that the session answers itself: the read and the parse, the
/// answer's serialization and send(), and its write, whose completion, onWrite(), starts the next. The span is one
/// burst's (manual time), until the client has read every answer, so the client's reads are in it, the same in any
/// build. Nothing ran a session's send() in a benchmark until #668 needed one for #512's checks there.
static void BM_WsSessionAnswerFlood(benchmark::State &state)
{
    ServedClient served;
    if (!served.connected())
    {
        state.SkipWithError("the client couldn't connect");
        return;
    }
    // Each message must be answered by the session itself: one that reached the engine's queue would time another path
    served.sendBurst(UNKNOWN);
    if (!served.serveAnswers() || (0 != served.queued()))
    {
        state.SkipWithError("the session didn't answer each message of a burst itself");
        return;
    }

    for (auto _ : state)
    {
        served.sendBurst(UNKNOWN);
        const auto start = std::chrono::steady_clock::now();
        const bool ok = served.serveAnswers();
        const auto end = std::chrono::steady_clock::now();
        if (!ok)
        {
            state.SkipWithError("the session stopped answering the burst");
            break;
        }
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    }
    state.SetItemsProcessed(state.iterations() * BURST);
}
BENCHMARK(BM_WsSessionAnswerFlood)->UseManualTime();

/// The IO thread's work for each new order a client sends: a session's read of a burst of 64, each parsed, checked,
/// made into an order and handed to the engine's queue. The span is one burst's (manual time), until the queue holds
/// the burst. Then, untimed, the client reads whatever the session answered, up to its answer to one message sent after
/// the burst, and the queue is emptied, which frees each order. Until #790 no benchmark took a new order through a
/// session.
static void orderFlood(benchmark::State &state, const std::string &order)
{
    ServedClient served(Burst::ORDERS);
    if (!served.connected())
    {
        state.SkipWithError("the client couldn't connect");
        return;
    }
    // Each order must reach the queue: one the session refused would time its refusal
    served.sendBurst(order);
    if (!served.serveBurst() || (BURST != served.queued()))
    {
        state.SkipWithError("the session didn't queue each order of a burst");
        return;
    }
    served.drain();
    if (!served.skipAnswers())
    {
        state.SkipWithError("the session didn't answer after a burst");
        return;
    }

    for (auto _ : state)
    {
        served.sendBurst(order);
        const auto start = std::chrono::steady_clock::now();
        const bool ok = served.serveBurst();
        const auto end = std::chrono::steady_clock::now();
        if (!ok)
        {
            state.SkipWithError("the session stopped queueing the burst's orders");
            break;
        }
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
        served.drain();
        if (!served.skipAnswers())
        {
            state.SkipWithError("the session stopped answering after a burst");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * BURST);
}

/// Orders as the order entry panel sends them, each naming its request, which the session keeps until the order's first
/// update (#790)
static void BM_WsSessionOrderFlood(benchmark::State &state)
{
    orderFlood(state, NEW_ORDER);
}
BENCHMARK(BM_WsSessionOrderFlood)->UseManualTime();

/// The same orders naming no request, as a client that names none sends them
static void BM_WsSessionOrderFloodWithoutIds(benchmark::State &state)
{
    orderFlood(state, NEW_ORDER_WITHOUT_ID);
}
BENCHMARK(BM_WsSessionOrderFloodWithoutIds)->UseManualTime();
