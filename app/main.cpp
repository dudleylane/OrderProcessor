#include <iostream>
#include <string>
#include <csignal>
#include <memory>
#include <thread>

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

using namespace COP;

namespace {

struct Config {
    unsigned short port = 8080;
    std::string dataDir = "./data";
    int workers = 0;
};

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            cfg.port = static_cast<unsigned short>(std::stoi(argv[++i]));
        else if (arg == "--data-dir" && i + 1 < argc)
            cfg.dataDir = argv[++i];
        else if (arg == "--workers" && i + 1 < argc)
            cfg.workers = std::stoi(argv[++i]);
    }
    return cfg;
}

}

int main(int argc, char* argv[]) {
    Config cfg = parseArgs(argc, argv);

    // 1. Create singletons
    aux::ExchLogger::create();
    aux::ExchLogger::instance()->setNoteOn(true);
    aux::ExchLogger::instance()->setWarnOn(true);
    aux::ExchLogger::instance()->setErrorOn(true);

    Store::WideDataStorage::create();
    IdTGenerator::create();
    Store::OrderStorage::create();

    aux::ExchLogger::instance()->note("OrderProcessor WebSocket Server starting...");

    // 2. Create LMDB storage and record dispatcher
    auto lmdbStorage = std::make_unique<Store::LMDBStorage>();
    auto dispatcher = std::make_unique<Store::StorageRecordDispatcher>();

    // Phase 1: Load reference data (instruments, accounts, strings) — orderBook is null
    dispatcher->init(Store::WideDataStorage::instance(), nullptr, lmdbStorage.get(),
                     Store::OrderStorage::instance());
    lmdbStorage->load(cfg.dataDir, dispatcher.get());

    aux::ExchLogger::instance()->note("Phase 1 LMDB load complete (reference data)");

    // 3. Collect instrument IDs and init OrderBook
    OrderBookImpl::InstrumentsT instrumentIds;
    Store::WideDataStorage::instance()->forEachInstrument(
        [&](const SourceIdT& id, const InstrumentEntry& /*instr*/) {
            instrumentIds.insert(id);
        });

    auto orderBook = std::make_unique<OrderBookImpl>();

    // 4. Phase 2: Create fresh LMDB + dispatcher with orderBook for order restoration
    lmdbStorage.reset();
    lmdbStorage = std::make_unique<Store::LMDBStorage>();
    dispatcher = std::make_unique<Store::StorageRecordDispatcher>();
    dispatcher->init(Store::WideDataStorage::instance(), orderBook.get(), lmdbStorage.get(),
                     Store::OrderStorage::instance());

    // Init order book with loaded instruments — need the dispatcher as OrderSaver
    orderBook->init(instrumentIds, dispatcher.get());

    // Reload LMDB — this time orders will be restored into OrderBook
    lmdbStorage->load(cfg.dataDir, dispatcher.get());

    aux::ExchLogger::instance()->note("Phase 2 LMDB load complete (orders restored)");

    // 5. Bind dispatcher as saver for WideDataStorage
    Store::WideDataStorage::instance()->bindStorage(dispatcher.get());
    Store::OrderStorage::instance()->attach(dispatcher.get());

    // 6. Create SessionManager and WsOutQueues
    auto sessionMgr = std::make_unique<App::SessionManager>();
    auto inQueues = std::make_unique<Queues::IncomingQueues>();
    auto wsOutQueues = std::make_unique<App::WsOutQueues>(
        sessionMgr.get(),
        Store::WideDataStorage::instance(),
        Store::OrderStorage::instance(),
        orderBook.get());

    // 7. Create TransactionManager
    auto transactMgr = std::make_unique<ACID::TransactionMgr>();
    ACID::TransactionMgrParams tmParams(IdTGenerator::instance());
    transactMgr->init(tmParams);

    // 8. Create Processor pool
    // Note: TaskManager takes ownership and deletes both pools separately,
    // so event processors and transaction processors must be distinct objects.
    const int numProcessors = (cfg.workers > 0) ? cfg.workers : 2;

    Queues::InQueueProcessorsPoolT evntProcessors;
    ACID::ProcessorPoolT transactProcessors;

    for (int i = 0; i < numProcessors; ++i) {
        Proc::ProcessorParams params(
            IdTGenerator::instance(),
            Store::OrderStorage::instance(),
            orderBook.get(),
            inQueues.get(),
            wsOutQueues.get(),
            inQueues.get(),
            transactMgr.get());

        auto* evtProc = new Proc::Processor();
        evtProc->init(params);
        evntProcessors.push_back(evtProc);

        auto* trProc = new Proc::Processor();
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

    auto taskMgr = std::make_unique<Tasks::TaskManager>(taskParams);

    // 10. Create Beast io_context and WsServer
    boost::asio::io_context ioc{1};

    auto server = std::make_shared<App::WsServer>(
        ioc,
        boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("0.0.0.0"), cfg.port),
        sessionMgr.get(),
        Store::WideDataStorage::instance(),
        Store::OrderStorage::instance(),
        inQueues.get(),
        IdTGenerator::instance(),
        orderBook.get());
    server->run();

    // 11. Signal handling for graceful shutdown
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code const&, int) {
        aux::ExchLogger::instance()->note("Shutdown signal received");
        ioc.stop();
    });

    aux::ExchLogger::instance()->note(
        std::string("OrderProcessor WebSocket Server listening on port ") + std::to_string(cfg.port));

    // 12. Run io_context on main thread
    ioc.run();

    // 13. Graceful shutdown
    aux::ExchLogger::instance()->note("Shutting down...");

    server.reset();

    transactMgr->stop();
    taskMgr->waitUntilTransactionsFinished(5);

    // Detach observers
    inQueues->detach();
    transactMgr->detach();

    // TaskManager destructor owns and deletes all processors (uses logger)
    taskMgr.reset();

    transactMgr.reset();
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
