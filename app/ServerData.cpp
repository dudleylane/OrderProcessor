/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include "ServerData.h"

#include <algorithm>

#include "Logger.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"

using namespace COP;

App::ServerData App::loadServerData(const std::string &dataDir, size_t lmdbMapSize, u64 idBlock,
                                    const std::string &clearingFirm, const std::string &defaultAccount)
{
    ServerData data;

    // Phase 1: Load reference data (instruments, accounts, strings) — orderBook is null
    data.lmdbStorage = std::make_unique<Store::LMDBStorage>(lmdbMapSize);
    data.dispatcher = std::make_unique<Store::StorageRecordDispatcher>();
    data.dispatcher->init(Store::WideDataStorage::instance(), nullptr, data.lmdbStorage.get(),
                          Store::OrderStorage::instance());
    data.lmdbStorage->load(dataDir, data.dispatcher.get());

    aux::ExchLogger::instance()->note("Phase 1 LMDB load complete (reference data)");

    // Collect instrument IDs and init OrderBook
    OrderBookImpl::InstrumentsT instrumentIds;
    Store::WideDataStorage::instance()->forEachInstrument(
        [&](const SourceIdT &id, const InstrumentEntry & /*instr*/)
        {
            instrumentIds.insert(id);
        });

    data.orderBook = std::make_unique<OrderBookImpl>();

    // Phase 2: Create fresh LMDB + dispatcher with orderBook for order restoration
    data.lmdbStorage.reset();
    data.lmdbStorage = std::make_unique<Store::LMDBStorage>(lmdbMapSize);
    data.dispatcher = std::make_unique<Store::StorageRecordDispatcher>();
    data.dispatcher->init(Store::WideDataStorage::instance(), data.orderBook.get(), data.lmdbStorage.get(),
                          Store::OrderStorage::instance());

    // Init order book with loaded instruments; the book itself does not persist orders (#20)
    data.orderBook->init(instrumentIds);

    // Reload LMDB — this time orders will be restored into OrderBook
    data.lmdbStorage->load(dataDir, data.dispatcher.get());

    aux::ExchLogger::instance()->note("Phase 2 LMDB load complete (orders restored)");

    // Order and execution ids come from one counter, which starts at 1 in every run. Continue it past the orders just
    // restored, so that no new order gets a number a restored order already has: clients name orders by number (#58).
    // And past the limit the previous run saved, below which it issued every id, execution ids included: execution
    // reports aren't persisted, so without it a new report could repeat an id a client already has (#81).
    data.lastOrderNumber = Store::OrderStorage::instance()->maxOrderNumber();
    const u64 idLimit = data.dispatcher->restoredIdLimit();
    data.lastIssued = std::max(data.lastOrderNumber, (0 < idLimit) ? idLimit - 1 : 0);
    IdTGenerator::instance()->advancePast(data.lastIssued);
    // From here on no id is returned before a limit above it is saved
    IdTGenerator::instance()->reserve(data.dispatcher.get(), idBlock);
    aux::ExchLogger::instance()->note("Order numbers continue after " + std::to_string(data.lastOrderNumber) +
                                      "; ids continue after " + std::to_string(data.lastIssued));

    // Resolve the defaults that WebSocket and FIX orders need (#34). Without them the server still starts, as it does
    // on an unseeded directory, but refuses the orders that need them. seedData provides both.
    data.defaultClearingId = Store::WideDataStorage::instance()->findClearingByFirm(clearingFirm);
    if (!data.defaultClearingId.isValid())
    {
        aux::ExchLogger::instance()->warn("Clearing firm '" + clearingFirm + "' is not in " + dataDir +
                                          "; orders will be refused until it is seeded and the server restarted");
    }
    data.defaultAccountId = Store::WideDataStorage::instance()->findAccountByName(defaultAccount);
    if (!data.defaultAccountId.isValid())
    {
        aux::ExchLogger::instance()->warn("Default account '" + defaultAccount + "' is not in " + dataDir +
                                          "; orders that name no account will be refused");
    }

    // Bind dispatcher as saver for WideDataStorage
    Store::WideDataStorage::instance()->bindStorage(data.dispatcher.get());
    Store::OrderStorage::instance()->attach(data.dispatcher.get());
    return data;
}
