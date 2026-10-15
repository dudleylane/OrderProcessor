/**
 Concurrent Order Processor library - WebSocket broadcast Benchmarks

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>
#include <vector>

#include "SessionManager.h"
#include "WsCredentials.h"
#include "WsSession.h"

using namespace COP;

namespace
{

/// The account the benchmarks' messages are about, and the identities of clients that may and may not see it (#225)
const SourceIdT ACCOUNT(1, 1);
std::shared_ptr<const App::WsIdentity> identityWith(const SourceIdT &account)
{
    auto identity = std::make_shared<App::WsIdentity>();
    identity->name = "desk-" + std::to_string(account.id_);
    identity->accounts = { account };
    identity->defaultAccount = account;
    return identity;
}

/// K clients, each with a session in a SessionManager, as the server's WsServer makes them. Their io_context never
/// runs: a benchmark times what the broadcasting thread does, and the handlers it posts wait until reset() drops them.
/// With identities, every other client's may see ACCOUNT, and the rest's may not.
class Clients
{
public:
    explicit Clients(int count, bool withIdentities = false) : count_(count), withIdentities_(withIdentities)
    {
        attach();
    }
    ~Clients()
    {
        detach();
    }

    App::SessionManager &manager()
    {
        return manager_;
    }

    /// Drops what the broadcasts posted, and the sessions with it, then makes the clients again
    void reset()
    {
        detach();
        ioc_ = std::make_unique<boost::asio::io_context>();
        attach();
    }

private:
    void attach()
    {
        for (int i = 0; i < count_; ++i)
        {
            auto session =
                std::make_shared<App::WsSession>(boost::asio::ip::tcp::socket(*ioc_), &manager_, nullptr, nullptr,
                                                 nullptr, nullptr, nullptr, SourceIdT(), SourceIdT());
            if (withIdentities_)
            {
                session->setIdentityForBenchmark((0 == (i % 2)) ? maySee_ : mayNotSee_);
            }
            manager_.addSession(session);
            sessions_.push_back(std::move(session));
        }
    }
    void detach()
    {
        for (auto &session : sessions_)
        {
            manager_.removeSession(session);
        }
        sessions_.clear();
    }

    const int count_;
    const bool withIdentities_;
    const std::shared_ptr<const App::WsIdentity> maySee_ = identityWith(ACCOUNT);
    const std::shared_ptr<const App::WsIdentity> mayNotSee_ = identityWith(SourceIdT(2, 1));
    std::unique_ptr<boost::asio::io_context> ioc_ = std::make_unique<boost::asio::io_context>();
    App::SessionManager manager_;
    std::vector<std::shared_ptr<App::WsSession>> sessions_;
};

/// The posted handlers are dropped every so often, outside the timing, so memory stays flat
const std::int64_t RESET_EVERY = 4096;

/// A message the size of an execution report's JSON
const std::string MESSAGE(300, 'x');

} // namespace

// SessionManager::broadcast as a transaction worker calls it, through WsOutQueues, for every execution report and order
// update: the message to each of K clients (#159)
static void BM_SessionManagerBroadcast(benchmark::State &state)
{
    Clients clients(static_cast<int>(state.range(0)));
    std::int64_t sinceReset = 0;
    for (auto _ : state)
    {
        clients.manager().broadcast(MESSAGE);
        if (++sinceReset == RESET_EVERY)
        {
            state.PauseTiming();
            clients.reset();
            sinceReset = 0;
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SessionManagerBroadcast)->Arg(1)->Arg(8)->Arg(64);

// SessionManager::broadcastToAccount, as WsOutQueues calls it since #225 for every execution report and order update:
// the message to each of K clients that may see the order's account. Without identities, all of them (the server's
// default); with them, every other one.
static void BM_SessionManagerBroadcastToAccount(benchmark::State &state)
{
    Clients clients(static_cast<int>(state.range(0)), 0 != state.range(1));
    std::int64_t sinceReset = 0;
    for (auto _ : state)
    {
        clients.manager().broadcastToAccount(ACCOUNT, MESSAGE);
        if (++sinceReset == RESET_EVERY)
        {
            state.PauseTiming();
            clients.reset();
            sinceReset = 0;
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SessionManagerBroadcastToAccount)->ArgsProduct({ { 1, 8, 64 }, { 0, 1 } });

// SessionManager::broadcastBookUpdate as a transaction worker calls it for every book change: each of K clients is
// asked whether it subscribes, and none does (#159)
static void BM_SessionManagerBookUpdate(benchmark::State &state)
{
    Clients clients(static_cast<int>(state.range(0)));
    for (auto _ : state)
    {
        clients.manager().broadcastBookUpdate("AAPL", MESSAGE);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SessionManagerBookUpdate)->Arg(1)->Arg(8)->Arg(64);
