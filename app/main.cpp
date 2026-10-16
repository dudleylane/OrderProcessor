#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <csignal>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "Logger.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "IncomingQueues.h"
#include "TransactionMgr.h"
#include "TransactionScope.h"
#include "Processor.h"
#include "ExpiryScheduler.h"
#include "ExchUtils.h"
#include "TaskManager.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"

#include "SessionManager.h"
#include "WsServer.h"
#include "WsOutQueues.h"
#include "MetricsPublisher.h"
#include "CpuAffinity.h"
#include "HugePages.h"
#include "ServerDefaults.h"
#include "ServerArgs.h"
#include "ServerData.h"
#ifdef BUILD_PG
#include "PGWriteBehind.h"
#endif

using namespace COP;

namespace
{

/// Ids reserved ahead of the counter, and saved as one record (#81). A block costs a write each time the counter
/// reaches it, and after a crash the next run's ids jump by up to this much; a clean stop saves the exact next id.
const u64 ID_RESERVATION_BLOCK = 10000;

// What an exception that nothing catches does (#203). It reaches std::terminate where it was thrown, before any
// destructor runs, so a half-built engine is never torn down, and onTerminate takes over from there:
//  - Until the server listens, it's a server that can't start: a data directory it can't open, or a port in use.
//    onTerminate logs the reason at Fatal, writes the log out, and exits with 1. It doesn't abort: the reason
//    would be lost from the log, and in the image every restart would dump core.
//  - Once the server listens, an exception that escapes is a bug. onTerminate logs it the same way, then aborts, so it
//    leaves a core dump.
std::atomic<bool> loggerReady{ false };
std::atomic<bool> listening{ false };
std::atomic<bool> terminating{ false };

[[noreturn]] void onTerminate()
{
    static thread_local bool reporting = false;
    if (reporting)
    {
        std::abort(); // the report itself failed
    }
    reporting = true;
    if (terminating.exchange(true))
    {
        // Another thread is reporting, and will end the process
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::string reason = "std::terminate without an exception";
    if (const std::exception_ptr ex = std::current_exception())
    {
        try
        {
            std::rethrow_exception(ex);
        }
        catch (const std::exception &e)
        {
            reason = e.what();
        }
        catch (...)
        {
            reason = "an exception that isn't a std::exception";
        }
    }
    const bool started = listening.load();
    const std::string message = (started ? "Fatal: " : "Can't start: ") + reason;
    std::fprintf(stderr, "orderProcessorServer: %s\n", message.c_str());
    if (loggerReady.load())
    {
        aux::ExchLogger::instance()->fatal(message);
        spdlog::shutdown(); // writes out what the asynchronous logger holds, and joins its thread
    }
    if (started)
    {
        std::abort();
    }
    std::_Exit(1);
}

} // namespace

int main(int argc, char *argv[])
{
    std::set_terminate(onTerminate);
    // A flag the server doesn't know, or a value it can't read, is a usage error: the reason and the usage on standard
    // error, and exit status 2, as seedData does (#173). Before, the server ignored it and ran with the defaults.
    const std::vector<std::string> args(argv + 1, argv + argc);
    std::string usageError;
    const std::optional<App::ServerArgs> parsed = App::parseServerArgs(args, &usageError);
    if (!parsed)
    {
        std::cerr << "orderProcessorServer: " << usageError << "\n" << App::serverUsage();
        return 2;
    }
    if (parsed->help)
    {
        std::cout << App::serverUsage();
        return 0;
    }
    // The hash a credentials file needs for the token on standard input's first line (#225)
    if (parsed->hashWsToken)
    {
        std::string token;
        std::getline(std::cin, token);
        while (!token.empty() && (('\r' == token.back()) || (' ' == token.back()) || ('\t' == token.back())))
        {
            token.pop_back();
        }
        if (!App::isValidWsToken(token))
        {
            std::cerr << "orderProcessorServer: a token needs at least " << App::WS_MIN_TOKEN_LENGTH
                      << " letters, digits, '.', '_' or '-' (`openssl rand -hex 32` makes one)\n";
            return 2;
        }
        std::cout << App::WsCredentials::hashToken(token) << "\n";
        return 0;
    }
    App::ServerArgs cfg = *parsed;
    // Who may open a WebSocket session (#146). A token that can't be used is a usage error too.
    App::WsAuth wsAuth;
    if (!App::resolveWsAuth(cfg, std::getenv("OMS_WS_TOKEN"), std::getenv("OMS_ALLOWED_ORIGINS"), &wsAuth, &usageError))
    {
        std::cerr << "orderProcessorServer: " << usageError << "\n";
        return 2;
    }
    // The Postgres to mirror to (#151), if any
    std::string pgConnection;
    if (!App::resolvePgConnection(cfg, std::getenv("OMS_PG_CONNECTION"), &pgConnection, &usageError))
    {
        std::cerr << "orderProcessorServer: " << usageError << "\n";
        return 2;
    }

    // 1. Create singletons
    aux::ExchLogger::create();
    loggerReady = true;
    aux::ExchLogger::instance()->setNoteOn(true);
    aux::ExchLogger::instance()->setWarnOn(true);
    aux::ExchLogger::instance()->setErrorOn(true);
    // The logger starts with every level on. Debug lines cost about six per transaction, formatted inside the
    // transaction manager's lock, so they are off unless --debug asks for them (#76).
    aux::ExchLogger::instance()->setDebugOn(cfg.debug);

    Store::WideDataStorage::create();
    IdTGenerator::create();
    Store::OrderStorage::create();

    aux::ExchLogger::instance()->note("OrderProcessor WebSocket Server starting...");

    // 1b. Pin main IO thread to a dedicated core if affinity is enabled
    if (cfg.cpuAffinityStart >= 0)
    {
        if (CpuAffinity::pinThreadToCore(cfg.cpuAffinityStart))
        {
            aux::ExchLogger::instance()->note(std::string("Main IO thread pinned to core ") +
                                              std::to_string(cfg.cpuAffinityStart));
        }
        else
        {
            aux::ExchLogger::instance()->warn(std::string("Failed to pin main thread to core ") +
                                              std::to_string(cfg.cpuAffinityStart));
        }
    }

    // 1c. Log HugePages availability
    if (cfg.hugePages)
    {
        if (HugePages::isAvailable())
        {
            aux::ExchLogger::instance()->note(std::string("Huge pages enabled (") +
                                              std::to_string(HugePages::getFreeCount()) + " free)");
        }
        else
        {
            aux::ExchLogger::instance()->warn("Huge pages requested but not available on this system");
            cfg.hugePages = false;
        }
    }

    // 2-5. Load the data directory: its reference data, then its orders into a book. Continue the ids past every one issued
    // before, find the defaults that orders need, and make the dispatcher the storages' saver (app/ServerData.cpp, #165).
    App::ServerData data = App::loadServerData(cfg.dataDir, static_cast<size_t>(cfg.lmdbMapMb) << 20,
                                               ID_RESERVATION_BLOCK, cfg.clearingFirm, cfg.defaultAccount);
    std::unique_ptr<Store::LMDBStorage> lmdbStorage = std::move(data.lmdbStorage);
    std::unique_ptr<Store::StorageRecordDispatcher> dispatcher = std::move(data.dispatcher);
    std::unique_ptr<OrderBookImpl> orderBook = std::move(data.orderBook);
    const SourceIdT defaultClearingId = data.defaultClearingId;
    const SourceIdT defaultAccountId = data.defaultAccountId;
    // 5a. The WebSocket identities' accounts, now that the directory's are known (#225). An account the directory lacks
    // throws, and the server doesn't start (#203): an identity that could trade nothing is a mistake to say at once.
    if (wsAuth.credentials)
    {
        wsAuth.credentials->resolveAccounts(
            [](const std::string &name)
            {
                return Store::WideDataStorage::instance()->findAccountByName(name);
            });
    }

    // 5b. Postgres, a copy of what LMDB holds, written behind it (#151). It gets everything the directory holds first,
    // which LMDB's load restored without saving, then whatever the server saves from here on, the restored orders'
    // matches below included.
#ifdef BUILD_PG
    std::unique_ptr<PG::PGWriteBehind> pgWriter;
    if (!pgConnection.empty())
    {
        pgWriter = std::make_unique<PG::PGWriteBehind>(pgConnection);
        dispatcher->setPGWriter(pgWriter.get());
        dispatcher->syncToPG(*Store::WideDataStorage::instance());
        aux::ExchLogger::instance()->note("Postgres: mirroring the data directory, " +
                                          std::to_string(pgWriter->totalEnqueued()) + " writes queued to start (#151)");
    }
#endif

    // 6. Create SessionManager and WsOutQueues
    auto sessionMgr = std::make_unique<App::SessionManager>();
    auto inQueues = std::make_unique<Queues::IncomingQueues>();
    auto wsOutQueues = std::make_unique<App::WsOutQueues>(sessionMgr.get(), Store::WideDataStorage::instance(),
                                                          Store::OrderStorage::instance(), orderBook.get());

    // 7. Create TransactionManager. Transaction ids need only increase within a run, and nothing persists them, so
    // they come from a counter of their own: a draw from the shared one can save the next id limit (#81), and the
    // manager draws under the lock that every worker's handoff also takes.
    IdTValueGenerator transactionIds;
    auto transactMgr = std::make_unique<ACID::TransactionMgr>();
    ACID::TransactionMgrParams tmParams(&transactionIds);
    transactMgr->init(tmParams);

    // 8. Create Processor pool
    // Note: TaskManager takes ownership and deletes both pools separately,
    // so event processors and transaction processors must be distinct objects.
    const int numProcessors = (cfg.workers > 0) ? cfg.workers : 2;

    Queues::InQueueProcessorsPoolT evntProcessors;
    ACID::ProcessorPoolT transactProcessors;

    // Expires DAY and GTD orders at their time (#240). The processors set an order's timer when they accept it; its
    // thread starts once the restore below has set the restored orders' timers.
    auto expiry = std::make_unique<Proc::ExpiryScheduler>(inQueues.get(),
                                                          []
                                                          {
                                                              return aux::currentDateTime();
                                                          });

    for (int i = 0; i < numProcessors; ++i)
    {
        Proc::ProcessorParams params(IdTGenerator::instance(), Store::OrderStorage::instance(), orderBook.get(),
                                     inQueues.get(), wsOutQueues.get(), inQueues.get(), transactMgr.get());
        params.expiry_ = expiry.get();

        auto *evtProc = new Proc::Processor();
        evtProc->init(params);
        evntProcessors.push_back(evtProc);

        auto *trProc = new Proc::Processor();
        trProc->init(params);
        transactProcessors.push_back(trProc);
    }

    // 9. Init TaskManager
    Tasks::TaskManager::init(cfg.workers);

    Tasks::TaskManagerParams taskParams;
    taskParams.transactMgr_ = transactMgr.get();
    taskParams.transactProcessors_ = transactProcessors;
    taskParams.evntProcessors_ = evntProcessors;
    taskParams.inQueues_ = inQueues.get();
    taskParams.cpuAffinityStart_ = cfg.cpuAffinityStart;

    auto taskMgr = std::make_unique<Tasks::TaskManager>(taskParams);

    // 9b. A crash after the transaction that booked an order committed, and before the one with its fills did, leaves
    // the order restored without its fills: Phase 2 left each such order out of the book, which it would cross (#207).
    // Match them now, in the order they arrived and before any client can send an order, as their bookings did. Their
    // fills persist and report through the transaction workers. One that can't be matched is no reason not to start.
    auto *restoreProcessor = static_cast<Proc::Processor *>(evntProcessors[0]);
    const std::vector<OrderEntry *> &ordersToMatch = dispatcher->ordersToMatch();
    if (!ordersToMatch.empty())
    {
        size_t matched = 0;
        for (OrderEntry *order : ordersToMatch)
        {
            try
            {
                restoreProcessor->matchRestored(order);
                ++matched;
            }
            catch (const std::exception &ex)
            {
                aux::ExchLogger::instance()->error("Restored order " + std::to_string(order->orderId_.id_) +
                                                   " lost its fills and could not be matched: " + ex.what());
            }
            // A trade here can release a held stop, and the release matches it in a transaction of its own, on a
            // transaction worker (#223). The next order is matched on this thread, outside the manager, so it waits for
            // every transaction to finish: matched at once, the two could trade with the same resting order.
            while (0 != transactMgr->pendingTransactions())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        aux::ExchLogger::instance()->warn(std::to_string(ordersToMatch.size()) +
                                          " restored order(s) had lost their fills; matched " +
                                          std::to_string(matched) + " of them before taking orders");
    }

    // 9c. A crash after a trade's transaction committed, and before the release of a held stop it reached did, leaves the
    // stop held, and only a trade releases one (#237). The trade's transaction persisted its instrument's last trade, and
    // the load found the stops it reached that were held when it was made. Release them now, after the orders above,
    // whose lost trades came before the lost releases, and before any client can send an order.
    const std::vector<OrderEntry *> &stopsReached = dispatcher->stopsReached();
    if (!stopsReached.empty())
    {
        size_t released = 0;
        for (OrderEntry *stop : stopsReached)
        {
            try
            {
                restoreProcessor->releaseRestored(stop);
                ++released;
            }
            catch (const std::exception &ex)
            {
                aux::ExchLogger::instance()->error(
                    "Restored stop " + std::to_string(stop->orderId_.id_) +
                    " that its instrument's last trade reached could not be released: " + ex.what());
            }
            // Each release matches on a transaction worker, and the next one must see its trades
            while (0 != transactMgr->pendingTransactions())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        aux::ExchLogger::instance()->warn(std::to_string(stopsReached.size()) +
                                          " restored stop(s) had lost their release; released " +
                                          std::to_string(released) + " before taking orders");
    }

    // 9d. Restored DAY and GTD orders keep their times in force (#240): each gets its timer again, and one whose time
    // passed while the server was down expires as soon as the timers start. An order that outlives a business day is
    // restated at each day's start (#244), and one whose day ended while the server was down is restated at once.
    // Read with nothing else running: no client is connected yet, and the restore's transactions are done.
    const size_t timers = App::scheduleRestoredExpiries(*Store::OrderStorage::instance(), expiry.get());
    expiry->start();
    aux::ExchLogger::instance()->note("Expiry and restatement timers set for restored orders: " +
                                      std::to_string(timers));

    // 10. Create Beast io_context and WsServer
    boost::asio::io_context ioc{ 1 };

    auto server = std::make_shared<App::WsServer>(
        ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(cfg.bindAddress), cfg.port), sessionMgr.get(),
        Store::WideDataStorage::instance(), Store::OrderStorage::instance(), inQueues.get(), IdTGenerator::instance(),
        orderBook.get(), defaultClearingId, defaultAccountId, cfg.wsLimits, wsAuth);
    server->run();
    if (wsAuth.credentials)
    {
        aux::ExchLogger::instance()->note(
            "WebSocket clients offer an identity's token, from --ws-credentials: " +
            std::to_string(wsAuth.credentials->size()) +
            " identities, each seeing and trading only its own accounts, and coming from " +
            (wsAuth.allowedOrigins.empty() ? std::string("any Origin")
                                           : std::to_string(wsAuth.allowedOrigins.size()) + " allowed Origin(s)") +
            " (#225)");
    }
    else if (wsAuth.token.empty())
    {
        aux::ExchLogger::instance()->warn(
            "No WebSocket access token is set (OMS_WS_TOKEN or --ws-token-file): any client that reaches " +
            cfg.bindAddress + ":" + std::to_string(cfg.port) + " can read every order and trade on any account (#146)");
    }
    else
    {
        aux::ExchLogger::instance()->note(
            "WebSocket clients must offer the access token, from " +
            std::string(cfg.wsTokenFile.empty() ? "OMS_WS_TOKEN" : "--ws-token-file") + ", and come from " +
            (wsAuth.allowedOrigins.empty() ? std::string("any Origin")
                                           : std::to_string(wsAuth.allowedOrigins.size()) + " allowed Origin(s)") +
            " (#146)");
    }

    // 10b. Create MetricsPublisher for system monitoring
    auto metricsPublisher = std::make_shared<App::MetricsPublisher>(
        ioc, sessionMgr.get(), taskMgr.get(), inQueues.get(), Store::OrderStorage::instance(),
        static_cast<Proc::Processor *>(evntProcessors[0])->scopePool(), lmdbStorage.get());
    metricsPublisher->start();

    // 11. Signal handling for graceful shutdown
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&](boost::system::error_code const &, int)
        {
            aux::ExchLogger::instance()->note("Shutdown signal received");
            ioc.stop();
        });

    aux::ExchLogger::instance()->note(std::string("OrderProcessor WebSocket Server listening on port ") +
                                      std::to_string(cfg.port));
    listening = true; // started: from here an exception that escapes is a bug (onTerminate)

    // 12. Run io_context on main thread
    ioc.run();

    // 13. Graceful shutdown
    aux::ExchLogger::instance()->note("Shutting down...");

    metricsPublisher->stop();
    metricsPublisher.reset();
    server.reset();
    // No timer fires from now on. The scheduler itself outlives the processors, which set timers on it (#240).
    expiry->stop();

    taskMgr->waitUntilTransactionsFinished(5);

    // Detach observers
    inQueues->detach();
    transactMgr->detach();

    // TaskManager destructor owns and deletes all processors (uses logger), and waits for every task it started
    taskMgr.reset();
    expiry.reset();

    // Only now stop the transaction manager: until ~TaskManager has waited for every task, an event still being
    // processed adds a transaction and a finishing one removes itself, and both require a started manager (#37).
    transactMgr->stop();
    transactMgr.reset();

    // Nothing draws ids now: save the exact next one, so that the next run continues without a gap (#81). If that
    // fails, the block limit already saved still covers every id issued.
    try
    {
        IdTGenerator::instance()->releaseReservation();
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("Could not save the next id: ") + ex.what());
    }
    wsOutQueues.reset();
    inQueues.reset();
    sessionMgr.reset();
    orderBook.reset();
#ifdef BUILD_PG
    // Nothing saves now: write what is queued, then stop (#151)
    if (pgWriter)
    {
        dispatcher->setPGWriter(nullptr);
        pgWriter->shutdown();
        pgWriter.reset();
    }
#endif
    dispatcher.reset();
    lmdbStorage.reset();

    // Destroy singletons in reverse order
    Store::OrderStorage::destroy();
    IdTGenerator::destroy();
    Store::WideDataStorage::destroy();

    // TaskManager static resources must be destroyed before logger
    Tasks::TaskManager::destroy();

    aux::ExchLogger::instance()->note("OrderProcessor WebSocket Server stopped");
    aux::ExchLogger::destroy();

    return 0;
}
