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
/// A record's first 4 bytes: its type in the low 16 bits, and the version of the layout its codec wrote in the high 16
/// (#149). Every layout so far is version 0, so those bytes are the type alone, as they always were.
const u32 RECORD_TYPE_MASK = 0xFFFF;
const u32 FORMAT_VERSION_SHIFT = 16;
/// The id limit record's layout: the limit, a u64
const u32 ID_LIMIT_FORMAT_VERSION = 0;
static_assert(sizeof(u32) == 4, "a record starts with a 4-byte word (#149)");
static_assert(sizeof(u64) == 8, "an id limit is stored as 8 bytes (#149)");
// A record's type is stored as its value (#150)
static_assert(
    (StorageRecordDispatcher::INVALID_RECORDTYPE == 0) && (StorageRecordDispatcher::INSTRUMENT_RECORDTYPE == 1) &&
        (StorageRecordDispatcher::STRING_RECORDTYPE == 2) && (StorageRecordDispatcher::ACCOUNT_RECORDTYPE == 3) &&
        (StorageRecordDispatcher::CLEARING_RECORDTYPE == 4) && (StorageRecordDispatcher::RAWDATA_RECORDTYPE == 5) &&
        (StorageRecordDispatcher::ORDER_RECORDTYPE == 6) && (StorageRecordDispatcher::EXECUTION_RECORDTYPE == 7) &&
        (StorageRecordDispatcher::EXECUTIONS_RECORDTYPE == 8) && (StorageRecordDispatcher::IDLIMIT_RECORDTYPE == 9),
    "RecordType values are persisted");

std::string recordWord(StorageRecordDispatcher::RecordType type, u32 formatVersion)
{
    const u32 word = (formatVersion << FORMAT_VERSION_SHIFT) | static_cast<u32>(type);
    return std::string(reinterpret_cast<const char *>(&word), sizeof(word));
}

/// Throws unless this build reads the layout a record was written in (#149)
void expectFormat(const char *record, u32 formatVersion, u32 newestReadable)
{
    if (formatVersion > newestReadable)
    {
        throw std::runtime_error(std::string(record) + " record in format version " + std::to_string(formatVersion) +
                                 ", newer than this build reads (" + std::to_string(newestReadable) +
                                 "), record could not be restored!");
    }
}
/// The id limit record's key (#81). The generators never issue id 0, and the only record an earlier build keyed with
/// id 0 is the empty execution list at (0,0), so (0,1) belongs to no other record.
const IdT ID_LIMIT_RECORD_ID(0, 1);
} // namespace

StorageRecordDispatcher::StorageRecordDispatcher(void)
    : storage_(nullptr), orderBook_(nullptr), fileStorage_(nullptr), orderStorage_(nullptr), restoredIdLimit_(0)
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

    u32 word = 0;
    memcpy(&word, buf, sizeof(word));
    const u32 type = word & RECORD_TYPE_MASK;
    const u32 formatVersion = word >> FORMAT_VERSION_SHIFT;
    switch (type)
    {
    case INSTRUMENT_RECORDTYPE:
    {
        expectFormat("Instrument", formatVersion, Codec::InstrumentCodec::FORMAT_VERSION);
        std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
        Codec::InstrumentCodec::decode(id, formatVersion, buf + sizeof(word), size - sizeof(word), instr.get());
        storage_->restore(instr.get());
        instr.release();
    }
    break;
    case STRING_RECORDTYPE:
    {
        expectFormat("String", formatVersion, Codec::StringTCodec::FORMAT_VERSION);
        std::unique_ptr<StringT> str(new StringT());
        Codec::StringTCodec::decode(buf + sizeof(word), size - sizeof(word), str.get());
        storage_->restore(id, str.get());
        str.release();
    }
    break;
    case ACCOUNT_RECORDTYPE:
    {
        expectFormat("Account", formatVersion, Codec::AccountCodec::FORMAT_VERSION);
        std::unique_ptr<AccountEntry> acct(new AccountEntry());
        Codec::AccountCodec::decode(id, formatVersion, buf + sizeof(word), size - sizeof(word), acct.get());
        storage_->restore(acct.get());
        acct.release();
    }
    break;
    case CLEARING_RECORDTYPE:
    {
        expectFormat("Clearing", formatVersion, Codec::ClearingCodec::FORMAT_VERSION);
        std::unique_ptr<ClearingEntry> clr(new ClearingEntry());
        Codec::ClearingCodec::decode(id, formatVersion, buf + sizeof(word), size - sizeof(word), clr.get());
        storage_->restore(clr.get());
        clr.release();
    }
    break;
    case RAWDATA_RECORDTYPE:
    {
        expectFormat("RawData", formatVersion, Codec::RawDataCodec::FORMAT_VERSION);
        std::unique_ptr<RawDataEntry> raw(new RawDataEntry());
        Codec::RawDataCodec::decode(id, formatVersion, buf + sizeof(word), size - sizeof(word), raw.get());
        storage_->restore(raw.get());
        raw.release();
    }
    break;
    case EXECUTION_RECORDTYPE:
        break;
    case EXECUTIONS_RECORDTYPE:
        // Execution lists are no longer written (#33). A data directory from an earlier build may hold one such
        // record, an empty list keyed (0,0); it is skipped.
        break;
    case IDLIMIT_RECORDTYPE:
    {
        expectFormat("Id limit", formatVersion, ID_LIMIT_FORMAT_VERSION);
        // Every id the previous runs issued is below the largest limit (#81)
        u64 limit = 0;
        if (sizeof(word) + sizeof(limit) != size)
        {
            throw std::runtime_error("Id limit record is not 12 bytes, record could not be restored!");
        }
        memcpy(&limit, buf + sizeof(word), sizeof(limit));
        if (limit > restoredIdLimit_)
        {
            restoredIdLimit_ = limit;
        }
        idLimitVersions_.push_back(version);
    }
    break;
    case ORDER_RECORDTYPE:
    {
        expectFormat("Order", formatVersion, Codec::OrderCodec::FORMAT_VERSION);
        if (nullptr == orderStorage_)
        {
            // a load for reference data only has nowhere to restore orders (#50)
            break;
        }
        // Keep the newest version only, and keep it encoded. Restoring here would replay every intermediate
        // state, and OrderDataStorage::restore() rejects an order id it already holds. Decoding here would build
        // an OrderEntry, whose constructor loads the instrument, and LMDB visits keys in byte order, so an order
        // can come before its instrument's record (#49); finishLoad() decodes once every record is restored.
        PendingOrdersT::iterator it = pendingOrders_.find(id);
        if ((pendingOrders_.end() == it) || (version >= it->second.version_))
        {
            pendingOrders_[id] =
                PendingOrder{ version, formatVersion, std::string(buf + sizeof(word), size - sizeof(word)) };
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
    case PENDINGREPLACE_ORDSTATUS: // a replacement that was never accepted (#74)
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
        const std::string &record = it->second.record_;
        std::unique_ptr<OrderEntry> order(
            Codec::OrderCodec::decode(it->first, it->second.formatVersion_, record.data(), record.size()));
        assert(nullptr != orderStorage_);
        // app/main.cpp loads twice, once without an order book and once with it, so an order can
        // already be in storage from the first pass. Restore it once, and book it on the pass that
        // has a book.
        OrderEntry *restored = orderStorage_->locateByOrderId(it->first);
        if (nullptr == restored)
        {
            orderStorage_->restore(order.get());
            restored = order.release();
            // Execution lists are not persisted (#33), so register an empty one under the id the order's
            // record holds. Without it the order's first execution finds no list and throws.
            const SourceIdT &executionsId = restored->executions_.getId();
            if (executionsId.isValid())
            {
                assert(nullptr != storage_);
                storage_->restore(executionsId, new ExecutionsT());
            }
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
    string buffer = recordWord(INSTRUMENT_RECORDTYPE, Codec::InstrumentCodec::FORMAT_VERSION);
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
    string buffer = recordWord(STRING_RECORDTYPE, Codec::StringTCodec::FORMAT_VERSION);
    Codec::StringTCodec::encode(val, &buffer);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const RawDataEntry &val)
{
    string buffer = recordWord(RAWDATA_RECORDTYPE, Codec::RawDataCodec::FORMAT_VERSION);
    IdT id;
    u32 version;
    Codec::RawDataCodec::encode(val, &buffer, &id, &version);
    fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const AccountEntry &val)
{
    string buffer = recordWord(ACCOUNT_RECORDTYPE, Codec::AccountCodec::FORMAT_VERSION);
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
    string buffer = recordWord(CLEARING_RECORDTYPE, Codec::ClearingCodec::FORMAT_VERSION);
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

u32 StorageRecordDispatcher::save(const OrderEntry &val)
{
    string buffer = recordWord(ORDER_RECORDTYPE, Codec::OrderCodec::FORMAT_VERSION);
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

void StorageRecordDispatcher::beginGroup()
{
    assert(nullptr != fileStorage_);
    fileStorage_->beginGroup();
}

void StorageRecordDispatcher::commitGroup()
{
    assert(nullptr != fileStorage_);
    fileStorage_->commitGroup();
}

void StorageRecordDispatcher::abortGroup() noexcept
{
    if (nullptr != fileStorage_)
    {
        fileStorage_->abortGroup();
    }
}

void StorageRecordDispatcher::saveIdLimit(u64 limit)
{
    assert(nullptr != fileStorage_);
    string buffer = recordWord(IDLIMIT_RECORDTYPE, ID_LIMIT_FORMAT_VERSION);
    buffer.append(reinterpret_cast<const char *>(&limit), sizeof(limit));
    // update() makes the new version durable and erases all but the newest older one with it (#148). Erasing that one
    // takes another write, so a crash in between leaves two, and the load keeps the larger limit. replace() would do
    // both in one write, but it walks the whole database.
    const u32 written = fileStorage_->update(ID_LIMIT_RECORD_ID, buffer.data(), buffer.size());
    for (u32 version : idLimitVersions_)
    {
        if (version != written)
        {
            fileStorage_->erase(ID_LIMIT_RECORD_ID, version);
        }
    }
    idLimitVersions_.assign(1, written);
}
