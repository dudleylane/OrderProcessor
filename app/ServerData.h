/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#pragma once

#include <memory>
#include <string>

#include "TypesDef.h"
#include "LMDBStorage.h"
#include "StorageRecordDispatcher.h"
#include "OrderBookImpl.h"

namespace COP
{
namespace Proc
{
class ExpiryScheduler;
}
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
    /// The largest order number restored
    u64 lastOrderNumber = 0;
    /// The id counter continues after it
    u64 lastIssued = 0;
    /// Invalid when the clearing firm isn't in the directory
    SourceIdT defaultClearingId;
    /// Invalid when the default account isn't in the directory
    SourceIdT defaultAccountId;
};

/// Loads the data directory as the server starts, which app/main.cpp did inline until #165:
/// - reference data first, then everything again with an order book, so that the orders are restored into it;
/// - the id counter continued past every id issued before (#58, #81), with idBlock ids reserved ahead;
/// - the clearing firm and default account that orders need, if the directory has them (#34);
/// - the dispatcher made the saver of the wide data and order storages.
/// It needs the singletons app/main.cpp creates first. It throws what a load throws, such as for a directory LMDB can't
/// open or a record this build can't read (#149, #150), and app/main.cpp then exits with the reason (#203).
ServerData loadServerData(const std::string &dataDir, size_t lmdbMapSize, u64 idBlock, const std::string &clearingFirm,
                          const std::string &defaultAccount);

/// Sets the timers of the restored DAY and GTD orders that are live (#240): one whose time passed while the server was
/// down fires as soon as the timers start. Call it with nothing else running on the orders, before the server takes
/// orders. Returns how many it set.
size_t scheduleRestoredExpiries(const Store::OrderDataStorage &storage, Proc::ExpiryScheduler *expiry);

} // namespace App
} // namespace COP
