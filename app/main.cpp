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

#ifdef BUILD_FIX
#include "FixServer.h"
#include "FixCredentials.h"
#include "MultiOutQueues.h"
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
    App::ServerArgs cfg = *parsed;
#ifdef BUILD_FIX
    if (cfg.hashFixPassword)
    {
        // The password from standard input, so it stays out of the shell's history and the process list (#147)
        std::string password;
        std::getline(std::cin, password);
        if (!password.empty() && ('\r' == password.back()))
        {
            password.pop_back();
        }
        if (password.empty())
        {
            std::cerr
                << "orderProcessorServer: --hash-fix-password reads the password from standard input, and got none\n";
            return 2;
        }
        std::cout << App::FixCredentials::hashPassword(password) << "\n";
        return 0;
    }
#endif
    // Who may open a WebSocket session (#146). A token that can't be used is a usage error too.
    App::WsAuth wsAuth;
    if (!App::resolveWsAuth(cfg, std::getenv("OMS_WS_TOKEN"), std::getenv("OMS_ALLOWED_ORIGINS"), &wsAuth, &usageError))
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

    // 6. Create SessionManager and WsOutQueues
    auto sessionMgr = std::make_unique<App::SessionManager>();
    auto inQueues = std::make_unique<Queues::IncomingQueues>();
    auto wsOutQueues = std::make_unique<App::WsOutQueues>(sessionMgr.get(), Store::WideDataStorage::instance(),
                                                          Store::OrderStorage::instance(), orderBook.get());

    // 6b. Create FIX gateway (if configured)
#ifdef BUILD_FIX
    std::unique_ptr<App::FixServer> fixServer;
    std::unique_ptr<App::MultiOutQueues> multiOutQueues;

    if (!cfg.fixCfg.empty())
    {
        fixServer =
            std::make_unique<App::FixServer>(inQueues.get(), Store::WideDataStorage::instance(),
                                             Store::OrderStorage::instance(), defaultClearingId, defaultAccountId);

        multiOutQueues = std::make_unique<App::MultiOutQueues>();
        multiOutQueues->addDelegate(wsOutQueues.get());
        multiOutQueues->addDelegate(fixServer->outQueues());

        aux::ExchLogger::instance()->note("FIX gateway configured: " + cfg.fixCfg);
    }
#endif

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

    Queues::OutQueues *outQueuesPtr = wsOutQueues.get();
#ifdef BUILD_FIX
    if (multiOutQueues)
    {
        outQueuesPtr = multiOutQueues.get();
    }
#endif

    for (int i = 0; i < numProcessors; ++i)
    {
        Proc::ProcessorParams params(IdTGenerator::instance(), Store::OrderStorage::instance(), orderBook.get(),
                                     inQueues.get(), outQueuesPtr, inQueues.get(), transactMgr.get());

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
    const std::vector<OrderEntry *> &ordersToMatch = dispatcher->ordersToMatch();
    if (!ordersToMatch.empty())
    {
        auto *restoreProcessor = static_cast<Proc::Processor *>(evntProcessors[0]);
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
        }
        aux::ExchLogger::instance()->warn(std::to_string(ordersToMatch.size()) +
                                          " restored order(s) had lost their fills; matched " +
                                          std::to_string(matched) + " of them before taking orders");
    }

    // 10. Create Beast io_context and WsServer
    boost::asio::io_context ioc{ 1 };

    auto server = std::make_shared<App::WsServer>(
        ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(cfg.bindAddress), cfg.port), sessionMgr.get(),
        Store::WideDataStorage::instance(), Store::OrderStorage::instance(), inQueues.get(), IdTGenerator::instance(),
        orderBook.get(), defaultClearingId, defaultAccountId, cfg.wsLimits, wsAuth);
    server->run();
    if (wsAuth.token.empty())
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

    // 10b. Start FIX acceptor (if configured)
#ifdef BUILD_FIX
    if (fixServer)
    {
        fixServer->start(cfg.fixCfg, cfg.fixCredentials);
    }
#endif

    // 10c. Create MetricsPublisher for system monitoring
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

#ifdef BUILD_FIX
    if (fixServer)
    {
        fixServer->stop();
    }
#endif

    metricsPublisher->stop();
    metricsPublisher.reset();
    server.reset();

    taskMgr->waitUntilTransactionsFinished(5);

    // Detach observers
    inQueues->detach();
    transactMgr->detach();

    // TaskManager destructor owns and deletes all processors (uses logger), and waits for every task it started
    taskMgr.reset();

    // Only now stop the transaction manager: until ~TaskManager has waited for every task, an event still being
    // processed adds a transaction and a finishing one removes itself, and both require a started manager (#37).
    transactMgr->stop();
    transactMgr.reset();
#ifdef BUILD_FIX
    multiOutQueues.reset();
    fixServer.reset();
#endif

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
