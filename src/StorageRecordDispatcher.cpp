/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "StorageRecordDispatcher.h"

#include "InstrumentCodec.h"
#include "StringTCodec.h"
#include "AccountCodec.h"
#include "ClearingCodec.h"
#include "RawDataCodec.h"
#include "OrderCodec.h"
#include "OrderStorage.h"

#ifdef BUILD_PG
#include "PGRequestBuilder.h"
#include "PGWriteBehind.h"
#endif

using namespace std;
using namespace COP;
using namespace COP::Store;

namespace
{
const int MINIMAL_SIZE = 4;
}

StorageRecordDispatcher::StorageRecordDispatcher(void)
    : storage_(nullptr), orderBook_(nullptr), fileStorage_(nullptr), orderStorage_(nullptr)
{
}

StorageRecordDispatcher::~StorageRecordDispatcher(void) {}

void StorageRecordDispatcher::init(DataStorageRestore *storage, OrderBook *orderBook, FileSaver *fileStorage,
                                   OrderDataStorage *orderStorage)
{
    assert(nullptr == storage_);
    assert(nullptr == orderBook_);
    assert(nullptr == fileStorage_);
    assert(nullptr == orderStorage_);

    storage_ = storage;
    orderBook_ = orderBook;
    fileStorage_ = fileStorage;
    orderStorage_ = orderStorage;
}

void StorageRecordDispatcher::startLoad() {}

void StorageRecordDispatcher::onRecordLoaded(const IdT &id, u32 version, const char *buf, size_t size)
{
    assert(nullptr != buf);

    if (MINIMAL_SIZE > size)
    {
        throw std::runtime_error("Record size invalid, record could not be restored!");
    }

    RecordType type;
    memcpy(&type, buf, sizeof(type));
    switch (type)
    {
    case INSTRUMENT_RECORDTYPE:
    {
        std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
        Codec::InstrumentCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), instr.get());
        storage_->restore(instr.get());
        instr.release();
    }
    break;
    case STRING_RECORDTYPE:
    {
        std::unique_ptr<StringT> str(new StringT());
        Codec::StringTCodec::decode(buf + sizeof(type), size - sizeof(type), str.get());
        storage_->restore(id, str.get());
        str.release();
    }
    break;
    case ACCOUNT_RECORDTYPE:
    {
        std::unique_ptr<AccountEntry> acct(new AccountEntry());
        Codec::AccountCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), acct.get());
        storage_->restore(acct.get());
        acct.release();
    }
    break;
    case CLEARING_RECORDTYPE:
    {
        std::unique_ptr<ClearingEntry> clr(new ClearingEntry());
        Codec::ClearingCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), clr.get());
        storage_->restore(clr.get());
        clr.release();
    }
    break;
    case RAWDATA_RECORDTYPE:
    {
        std::unique_ptr<RawDataEntry> raw(new RawDataEntry());
        Codec::RawDataCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), raw.get());
        storage_->restore(raw.get());
        raw.release();
    }
    break;
    case EXECUTION_RECORDTYPE:
        break;
    case EXECUTIONS_RECORDTYPE:
        break;
    case ORDER_RECORDTYPE:
    {
        std::unique_ptr<OrderEntry> order(
            Codec::OrderCodec::decode(id, version, buf + sizeof(type), size - sizeof(type)));
        // Keep the newest version only. Restoring here would replay every intermediate state, and
        // OrderDataStorage::restore() rejects an order id it already holds.
        PendingOrdersT::iterator it = pendingOrders_.find(id);
        if (pendingOrders_.end() == it)
        {
            pendingOrders_.insert(PendingOrdersT::value_type(id, std::make_pair(version, order.release())));
        }
        else if (version >= it->second.first)
        {
            std::unique_ptr<OrderEntry> previous(it->second.second);
            it->second.first = version;
            it->second.second = order.release();
        }
    }
    break;
    default:
        throw std::runtime_error("Invalid record type, unable to decode record!");
    };
}

namespace
{
/// An order belongs in the book only while it can still trade. Before #20 every record was a
/// pre-acceptance snapshot, so restoring all of them was right; now terminal states persist too.
bool belongsInBook(const OrderEntry &order)
{
    if (MARKET_ORDERTYPE == order.ordType_)
    {
        return false;
    }
    switch (order.status_)
    {
    case FILLED_ORDSTATUS:
    case CANCELED_ORDSTATUS:
    case REJECTED_ORDSTATUS:
    case EXPIRED_ORDSTATUS:
    case DFD_ORDSTATUS:
    case REPLACED_ORDSTATUS:
        return false;
    default:
        break;
    }
    return 0 < order.leavesQty_;
}
} // namespace

void StorageRecordDispatcher::finishLoad()
{
    for (PendingOrdersT::iterator it = pendingOrders_.begin(); it != pendingOrders_.end(); ++it)
    {
        std::unique_ptr<OrderEntry> order(it->second.second);
        it->second.second = nullptr;
        assert(nullptr != orderStorage_);
        // app/main.cpp loads twice, once without an order book and once with it, so an order can
        // already be in storage from the first pass. Restore it once, and book it on the pass that
        // has a book.
        OrderEntry *restored = orderStorage_->locateByOrderId(it->first);
        if (nullptr == restored)
        {
            orderStorage_->restore(order.get());
            restored = order.release();
        }
        if ((nullptr != orderBook_) && belongsInBook(*restored))
        {
            orderBook_->restore(*restored);
        }
    }
    pendingOrders_.clear();
}

void StorageRecordDispatcher::save(const InstrumentEntry &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::INSTRUMENT_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    IdT id;
    u32 version;
    Codec::InstrumentCodec::encode(val, &buffer, &id, &version);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
#ifdef BUILD_PG
    if (pgWriter_)
    {
        pgWriter_->enqueue(PG::PGRequestBuilder::fromInstrument(val));
    }
#endif
}

void StorageRecordDispatcher::save(const IdT &id, const StringT &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::STRING_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    Codec::StringTCodec::encode(val, &buffer);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const RawDataEntry &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::RAWDATA_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    IdT id;
    u32 version;
    Codec::RawDataCodec::encode(val, &buffer, &id, &version);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const AccountEntry &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::ACCOUNT_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    IdT id;
    u32 version;
    Codec::AccountCodec::encode(val, &buffer, &id, &version);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
#ifdef BUILD_PG
    if (pgWriter_)
    {
        pgWriter_->enqueue(PG::PGRequestBuilder::fromAccount(val));
    }
#endif
}

void StorageRecordDispatcher::save(const ClearingEntry &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::CLEARING_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    IdT id;
    u32 version;
    Codec::ClearingCodec::encode(val, &buffer, &id, &version);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
#ifdef BUILD_PG
    if (pgWriter_)
    {
        pgWriter_->enqueue(PG::PGRequestBuilder::fromClearing(val));
    }
#endif
}

void StorageRecordDispatcher::save(const ExecutionsT &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::EXECUTIONS_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    u64 count = val.size();
    buffer.append(reinterpret_cast<const char *>(&count), sizeof(count));
    for (const auto &entry : val)
    {
        entry.eventId_.serialize(buffer);
    }
    IdT id(count, 0);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
}

u32 StorageRecordDispatcher::save(const OrderEntry &val)
{
    string buffer;
    {
        char typebuf[36];
        int t = StorageRecordDispatcher::ORDER_RECORDTYPE;
        memcpy(typebuf, &t, sizeof(t));
        buffer.append(typebuf, sizeof(t));
    }
    IdT id;
    u32 version;
    Codec::OrderCodec::encode(val, &buffer, &id, &version);
    // update() appends a new version and returns it; for an order written for the first time that
    // version is 0. save() would reject the second write of an order that changed (issue #20).
    const u32 written = fileStorage_->update(id, buffer.c_str(), buffer.size());
#ifdef BUILD_PG
    if (pgWriter_)
    {
        pgWriter_->enqueue(PG::PGRequestBuilder::fromOrder(val));
    }
#endif
    return written;
}

void StorageRecordDispatcher::erase(const IdT &orderId, u32 version)
{
    assert(nullptr != fileStorage_);
    fileStorage_->erase(orderId, version);
}
