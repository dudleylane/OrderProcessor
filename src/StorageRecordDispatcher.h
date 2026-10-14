/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "FileStorageDef.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"

#ifdef BUILD_PG
namespace COP::PG
{
class PGWriteBehind;
}
#endif

namespace COP
{

class OrderBook;

namespace Store
{

class OrderDataStorage;
class WideParamsDataStorage;

class FileStorage;

/// parse incoming buffer into the record
/// buffer format: <type - 32 bit><body, format depends on type>
class StorageRecordDispatcher : public FileStorageObserver, public DataSaver, public OrderSaver, public IdLimitSaver
{
public:
    StorageRecordDispatcher(void);
    virtual ~StorageRecordDispatcher(void);

    /// orderBook and orderStorage may be null. Without an order book, restored orders are not booked; without order
    /// storage, order records are skipped, for a load that needs only reference data (seedData, #50).
    void init(DataStorageRestore *storage, OrderBook *orderBook, FileSaver *fileStorage,
              OrderDataStorage *orderStorage);

public:
    /// reimplemented from FileStorageObserver
    virtual void startLoad();
    virtual void onRecordLoaded(const IdT &id, u32 version, const char *buf, size_t size);
    virtual void finishLoad();

public:
    /// reimplemented from DataSaver
    virtual void save(const InstrumentEntry &val);
    virtual void save(const IdT &id, const StringT &val);
    virtual void save(const RawDataEntry &val);
    virtual void save(const AccountEntry &val);
    virtual void save(const ClearingEntry &val);

public:
    /// reimplemented from OrderSaver
    virtual u32 save(const OrderEntry &val);
    virtual void erase(const IdT &orderId, u32 version);
    /// A group of orders' saves, written as one by the file storage (#163)
    void beginGroup() override;
    void commitGroup() override;
    void abortGroup() noexcept override;

public:
    /// reimplemented from IdLimitSaver: one record, the newest version kept (#81)
    virtual void saveIdLimit(u64 limit);
    /// The id limit the last load found, or 0 when the directory has none. Every id issued before is below it (#81).
    u64 restoredIdLimit() const
    {
        return restoredIdLimit_;
    }
    /// The orders the last load restored without booking, in the order they arrived: each lost the fills its booking
    /// started, because the transaction that booked it committed and the one with its fills didn't (#207). An open
    /// market order is one, and so is an order that crosses one that arrived before it; booked as they were, they
    /// would leave the book crossed. Processor::matchRestored() matches each as the transaction that booked it did,
    /// and the server calls it for each, in this order, before it takes orders.
    const std::vector<OrderEntry *> &ordersToMatch() const
    {
        return ordersToMatch_;
    }

#ifdef BUILD_PG
public:
    /// Mirrors what is saved from now on to Postgres, and what a rollback erases (#151)
    void setPGWriter(PG::PGWriteBehind *writer)
    {
        pgWriter_ = writer;
    }
    /// Hands the Postgres writer every instrument, account, clearing firm and order the data directory holds (#151).
    /// LMDB's load restores them without save(), so Postgres would otherwise never hear of anything stored before it
    /// was wired up, and an order's row would point at no instrument.
    void syncToPG(const WideParamsDataStorage &wideData) const;
#endif

public:
    enum RecordType
    {
        INVALID_RECORDTYPE = 0,
        INSTRUMENT_RECORDTYPE,
        STRING_RECORDTYPE,
        ACCOUNT_RECORDTYPE,
        CLEARING_RECORDTYPE,
        RAWDATA_RECORDTYPE,
        ORDER_RECORDTYPE,
        EXECUTION_RECORDTYPE,
        EXECUTIONS_RECORDTYPE,
        IDLIMIT_RECORDTYPE, // every id issued is below the limit it holds (#81)
        TOTAL_RECORDTYPE
    };

private:
    /// Whether a restored order would have traded with an order that arrived before it: one the matcher's own search
    /// finds among the orders booked so far, or one already left to match (#207)
    bool crossesAnEarlierOrder(OrderEntry *order) const;

private:
    DataStorageRestore *storage_;
    OrderBook *orderBook_;
    FileSaver *fileStorage_;
    OrderDataStorage *orderStorage_;

    /// Orders seen during a load: the newest version of each and its record, still encoded. The loader
    /// replays every version of every record, so the newest cannot be picked until the load ends, and
    /// decoding waits for finishLoad() too: an order's instrument may not be restored yet (#49).
    struct PendingOrder
    {
        u32 version_;       ///< LMDB's version of the record, which picks the newest
        u32 formatVersion_; ///< the layout the codec wrote, which decode() reads by (#149)
        std::string record_;
    };
    typedef std::map<IdT, PendingOrder> PendingOrdersT;
    PendingOrdersT pendingOrders_;
    /// The largest id limit loaded, and every version of its record on disk: a crash between writing a new version
    /// and erasing the old leaves two, and saveIdLimit() erases all but the one it writes (#81)
    u64 restoredIdLimit_;
    std::vector<u32> idLimitVersions_;
    std::vector<OrderEntry *> ordersToMatch_;
#ifdef BUILD_PG
    PG::PGWriteBehind *pgWriter_ = nullptr;
#endif
};

} // namespace Store
} // namespace COP