/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include "ServerData.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <format>

#include "Logger.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "ExpiryScheduler.h"

using namespace COP;

DateTimeT App::keptDaysStart(DateTimeT now, u32 days)
{
    if (0 == days)
    {
        return 0;
    }
    // Back a day's start at a time: a business day is 23 or 25 hours long when New York changes its clocks
    DateTimeT start = Proc::ExpiryScheduler::dayStartOf(now);
    for (u32 day = 1; (day < days) && (0 < start); ++day)
    {
        start = Proc::ExpiryScheduler::dayStartOf(start - 1);
    }
    return start;
}

App::ServerData App::loadServerData(const std::string &dataDir, size_t lmdbMapSize, u64 idBlock,
                                    const std::string &clearingFirm, const std::string &defaultAccount,
                                    u32 keepClosedDays, DateTimeT now)
{
    ServerData data;

    // Phase 1: Load reference data (instruments, accounts, strings) — orderBook is null. Its orders are restored too, and
    // the closed ones of earlier business days dropped (#597): their records are erased, so the second pass finds none.
    const DateTimeT keptFrom = keptDaysStart(now, keepClosedDays);
    data.lmdbStorage = std::make_unique<Store::LMDBStorage>(lmdbMapSize);
    data.dispatcher = std::make_unique<Store::StorageRecordDispatcher>();
    data.dispatcher->init(Store::WideDataStorage::instance(), nullptr, data.lmdbStorage.get(),
                          Store::OrderStorage::instance());
    data.dispatcher->dropClosedBefore(keptFrom);
    data.lmdbStorage->load(dataDir, data.dispatcher.get());
    data.droppedOrders = data.dispatcher->droppedOrders();
    const u64 droppedMaxOrderNumber = data.dispatcher->droppedMaxOrderNumber();

    aux::ExchLogger::instance()->note("Phase 1 LMDB load complete (reference data)");
    if (0 < keepClosedDays)
    {
        // One line a start, not one an order
        aux::ExchLogger::instance()->note(
            "Dropped " + std::to_string(data.droppedOrders) + " closed orders of business days before " +
            std::format("{:%FT%TZ}", std::chrono::sys_seconds(std::chrono::seconds(keptFrom / 1000))) +
            " (--keep-closed-days " + std::to_string(keepClosedDays) + ")");
    }

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
    data.lastOrderNumber = std::max(Store::OrderStorage::instance()->maxOrderNumber(), droppedMaxOrderNumber);
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

size_t App::scheduleRestoredExpiries(const Store::OrderDataStorage &storage, Proc::ExpiryScheduler *expiry)
{
    assert(nullptr != expiry);
    size_t timers = 0;
    storage.forEachOrder(
        [&](const IdT &, const OrderEntry &order)
        {
            DateTimeT when = 0;
            Queues::TimerEvent::TimerType type = Queues::TimerEvent::INVALID_TIMER;
            const bool live = (NEW_ORDSTATUS == order.status_) || (PARTFILL_ORDSTATUS == order.status_) ||
                              (SUSPENDED_ORDSTATUS == order.status_);
            if (live && Proc::ExpiryScheduler::expiryOf(order, &when, &type))
            {
                // Taken before #511 refused one: the timers wait for it with no deadline, so it never comes due
                if (Proc::ExpiryScheduler::LATEST_TIME < when)
                {
                    aux::ExchLogger::instance()->warn(
                        "Order " + std::to_string(order.orderId_.id_) + " expires at " + std::to_string(when) +
                        " ms, past the latest time the server can wait until: it won't expire, and stays open until "
                        "it is canceled (#511)");
                }
                expiry->schedule(order.orderId_, when, type);
                ++timers;
            }
            if (live && Proc::ExpiryScheduler::restatementOf(order, &when))
            {
                expiry->schedule(order.orderId_, when, Queues::TimerEvent::RESTATEMENT);
                ++timers;
            }
        });
    return timers;
}
