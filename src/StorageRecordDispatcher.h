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

#include "FileStorageDef.h"
#include "DataModelDef.h"

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

class FileStorage;

/// parse incoming buffer into the record
/// buffer format: <type - 32 bit><body, format depends on type>
class StorageRecordDispatcher : public FileStorageObserver, public DataSaver, public OrderSaver
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

#ifdef BUILD_PG
public:
    void setPGWriter(PG::PGWriteBehind *writer)
    {
        pgWriter_ = writer;
    }
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
        TOTAL_RECORDTYPE
    };

private:
    DataStorageRestore *storage_;
    OrderBook *orderBook_;
    FileSaver *fileStorage_;
    OrderDataStorage *orderStorage_;

    /// Orders seen during a load: the newest version of each and its record, still encoded. The loader
    /// replays every version of every record, so the newest cannot be picked until the load ends, and
    /// decoding waits for finishLoad() too: an order's instrument may not be restored yet (#49).
    typedef std::map<IdT, std::pair<u32, std::string>> PendingOrdersT;
    PendingOrdersT pendingOrders_;
#ifdef BUILD_PG
    PG::PGWriteBehind *pgWriter_ = nullptr;
#endif
};

} // namespace Store
} // namespace COP