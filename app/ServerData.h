/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TypesDef.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"
#include "OrderBookImpl.h"

namespace COP
{
namespace Proc
{
class ExpiryScheduler;
class Processor;
} // namespace Proc
namespace Store
{
class OrderDataStorage;
}

namespace App
{

/// What a server takes from its data directory at startup (#165)
struct ServerData
{
    std::unique_ptr<Store::LMDBStorage> lmdbStorage;
    /// Saves for both storages from here on, and lists the restored orders left to match (#207)
    std::unique_ptr<Store::StorageRecordDispatcher> dispatcher;
    /// Holds the restored orders that rest
    std::unique_ptr<OrderBookImpl> orderBook;
    /// The largest order number restored, or dropped (#597)
    u64 lastOrderNumber = 0;
    /// The id counter continues after it
    u64 lastIssued = 0;
    /// Invalid when the clearing firm isn't in the directory
    SourceIdT defaultClearingId;
    /// Invalid when the default account isn't in the directory
    SourceIdT defaultAccountId;
    /// The closed orders the load dropped (#597)
    size_t droppedOrders = 0;
};
/// The start of the earliest of the last days business days as of now, the current one counted (#597): 0 for 0 days,
/// which keeps every day
DateTimeT keptDaysStart(DateTimeT now, u32 days);

/// Loads the data directory as the server starts, which app/main.cpp did inline until #165:
/// - reference data first, then everything again with an order book, so that the orders are restored into it;
/// - with keepClosedDays above 0, each closed order whose business day isn't one of the last keepClosedDays as of now
///   dropped: not restored, and its records erased (#597);
/// - the id counter continued past every id issued before (#58, #81), with idBlock ids reserved ahead;
/// - the clearing firm and default account that orders need (#34). A directory that holds reference data and lacks
///   either refuses the start: every order would be refused, or every order that names no account (#640). Under
///   --ws-credentials, defaultAccountUsed false, an order with no account trades its identity's first, so the default
///   account isn't needed (#225). An empty directory starts, as before, for seedData to seed (#161);
/// - the dispatcher made the saver of the wide data and order storages.
/// It needs the singletons app/main.cpp creates first. It throws what a load throws, such as for a directory LMDB can't
/// open or a record this build can't read (#149, #150), and app/main.cpp then exits with the reason (#203).
ServerData loadServerData(const std::string &dataDir, size_t lmdbMapSize, u64 idBlock, const std::string &clearingFirm,
                          const std::string &defaultAccount, u32 keepClosedDays = 0, DateTimeT now = 0,
                          bool defaultAccountUsed = true);

/// What a check of a data directory found (#591)
struct DataCheck
{
    size_t instruments = 0;
    size_t accounts = 0;
    size_t clearingFirms = 0;
    /// The orders restored, each from the newest version of its record
    size_t orders = 0;
    /// Of those, the open orders a crash left without their fills, which the next start matches (#207)
    size_t ordersToMatch = 0;
};

/// Restores the data directory as loadServerData() does, in its two passes, for seedData --check (#591), with the
/// environment opened read-only (LMDBStorage::loadReadOnly()). It writes nothing to the data file: it drops no order
/// and reserves no id, so it may run on a directory a server is using, or on a copy mdb_copy made. LMDB still opens the
/// directory's lock.mdb for writing, creating it beside a copy that has none, as every reader does, unless the
/// filesystem is read-only. It throws what a start's load throws, and names a record it refuses by its key and version.
/// It needs the singletons loadServerData() needs, and leaves what it restored in them.
DataCheck checkServerData(const std::string &dataDir, size_t lmdbMapSize);

/// Sets the timers of the restored orders that are live: a DAY or GTD order's expiry (#240), and the restatement of
/// one that outlives its business day (#244). One whose time passed while the server was down is due at once: an order
/// whose day ended then begins the day the server restarted in. An order of unbooked, which restore left out of the
/// book (StorageRecordDispatcher::ordersToMatch()), gets its restatement alone: an expiry's unbooking wouldn't find
/// it, so Processor::matchRestored() decides its expiry, and scheduleExpiriesOf() sets its timer after (#768). Call it
/// with nothing else running on the orders, before the server takes orders. Returns how many it set.
size_t scheduleRestoredExpiries(const Store::OrderDataStorage &storage, Proc::ExpiryScheduler *expiry,
                                const std::vector<OrderEntry *> &unbooked = {});
/// Sets the expiry timers of the orders that are still live, a DAY or GTD order's: the restart's, once it has matched
/// those restore left out of the book (#768). Call it with nothing else running on the orders. Returns how many it set.
size_t scheduleExpiriesOf(const std::vector<OrderEntry *> &orders, Proc::ExpiryScheduler *expiry);
/// What finishRestart() did
struct RestartWork
{
    /// The timers set for the restored orders, and how many of them were due, and fired, before the matching
    size_t timers = 0;
    size_t fired = 0;
    /// Of the orders restore left out of the book: those matched, and those expired instead
    size_t matched = 0;
    size_t expired = 0;
    /// The held stops whose lost releases were released
    size_t released = 0;
};
/// The restart's work on the restored orders once the engine runs, before the server takes orders, which app/main.cpp
/// did inline until #768. In this order:
/// - expire first (#768, as ruled): the restored orders' timers are set, and those due fire, until none is, with the
///   engine idle between rounds. An order whose time in force ended while the server was down expires before anything
///   could trade with it, and one whose business day ended begins the new one;
/// - each order restore left out of the book, which a crash left without its fills or its cancel (#207, #489), is
///   matched, or expired if its time in force ended (Processor::matchRestored());
/// - each held stop whose release a crash lost is released (#237, #518);
/// - the expiry timers of the orders matched above that still rest are set.
/// waitIdle returns once the engine has finished everything queued: the events, and the transactions they enqueue.
/// The caller starts the expiry scheduler's thread after it. An order that can't be matched, expired or released is
/// logged and left, as no reason not to start.
RestartWork finishRestart(Proc::Processor &processor, const std::vector<OrderEntry *> &ordersToMatch,
                          const std::vector<OrderEntry *> &stopsReached, const Store::OrderDataStorage &storage,
                          Proc::ExpiryScheduler &expiry, const std::function<void()> &waitIdle);

} // namespace App
} // namespace COP
