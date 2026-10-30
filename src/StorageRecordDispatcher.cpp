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
#include "OrderMatcher.h"

#ifdef BUILD_PG
#include <memory>

#include "Logger.h"
#include "PGRequestBuilder.h"
#include "PGWriteBehind.h"
#include "WideDataStorage.h"
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
        (StorageRecordDispatcher::EXECUTIONS_RECORDTYPE == 8) && (StorageRecordDispatcher::IDLIMIT_RECORDTYPE == 9) &&
        (StorageRecordDispatcher::LASTTRADE_RECORDTYPE == 10),
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
/// The last-trade record's layout (#237): the instrument's id, the price, a double, and the trade's id. An id is its u64
/// number, then its u32 date.
const u32 LAST_TRADE_FORMAT_VERSION = 0;
const size_t LAST_TRADE_RECORD_SIZE = sizeof(u32) + 2 * (sizeof(u64) + sizeof(u32)) + sizeof(PriceT);
void appendId(string *buffer, const IdT &id)
{
    buffer->append(reinterpret_cast<const char *>(&id.id_), sizeof(id.id_));
    buffer->append(reinterpret_cast<const char *>(&id.date_), sizeof(id.date_));
}
const char *readId(const char *p, IdT *id)
{
    memcpy(&id->id_, p, sizeof(id->id_));
    p += sizeof(id->id_);
    memcpy(&id->date_, p, sizeof(id->date_));
    return p + sizeof(id->date_);
}
/// Its key: the instrument's number, with date 2. Reference data is keyed with dates 0 and 1, and an order with the Unix
/// second its id was drawn in, so no other record has it. The instrument's own key must not be used: a new version prunes
/// every older version of its key (#148), and the instrument's record would go.
const u32 LAST_TRADE_KEY_DATE = 2;
IdT lastTradeKey(const SourceIdT &instrument)
{
    return IdT(instrument.id_, LAST_TRADE_KEY_DATE);
}
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
    case LASTTRADE_RECORDTYPE:
    {
        expectFormat("Last trade", formatVersion, LAST_TRADE_FORMAT_VERSION);
        if (LAST_TRADE_RECORD_SIZE != size)
        {
            throw std::runtime_error("Last trade record is not 36 bytes, record could not be restored!");
        }
        SourceIdT instrument;
        LastTrade trade{ version, 0.0, IdT() };
        const char *p = readId(buf + sizeof(word), &instrument);
        memcpy(&trade.lastPx_, p, sizeof(trade.lastPx_));
        readId(p + sizeof(trade.lastPx_), &trade.tradeId_);
        if (lastTradeKey(instrument) != id)
        {
            throw std::runtime_error("Last trade record keyed for another instrument, record could not be restored!");
        }
        // The newest version is the last trade: the transactions that write it run one after another (#237)
        std::map<IdT, LastTrade>::iterator it = lastTrades_.find(instrument);
        if ((lastTrades_.end() == it) || (version >= it->second.version_))
        {
            lastTrades_[instrument] = trade;
        }
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

bool StorageRecordDispatcher::crossesAnEarlierOrder(OrderEntry *order) const
{
    assert(nullptr != orderBook_);
    assert(nullptr != orderStorage_);
    // The matcher decides, not a copy of its test here: a restore must book exactly what a live match would have left
    // resting, and the test changes, as it did for FX swaps (#212)
    if (Proc::OrderMatcher::findContra(order, *orderBook_, orderStorage_).isValid())
    {
        return true;
    }
    // An order left to match may rest once it is matched, and then this order trades with it at its price, as it would
    // have when this order arrived. Booked now instead, this order would rest, and the earlier order would trade with
    // it at this order's price.
    const SourceIdT instrument = order->instrument_.getId();
    const Side otherSide = (BUY_SIDE == order->side_) ? SELL_SIDE : BUY_SIDE;
    for (const OrderEntry *earlier : ordersToMatch_)
    {
        if ((otherSide == earlier->side_) && (instrument == earlier->instrument_.getId()) &&
            Proc::OrderMatcher::tradesWith(*order, *earlier))
        {
            return true;
        }
    }
    return false;
}

void StorageRecordDispatcher::finishLoad()
{
    ordersToMatch_.clear();
    // In id order, which is the order the orders arrived in: ids increase across restarts too (#58)
    for (PendingOrdersT::iterator it = pendingOrders_.begin(); it != pendingOrders_.end(); ++it)
    {
        const std::string &record = it->second.record_;
        Codec::OrderStrings carried;
        std::unique_ptr<OrderEntry> order(
            Codec::OrderCodec::decode(it->first, it->second.formatVersion_, record.data(), record.size(), &carried));
        assert(nullptr != orderStorage_);
        // app/main.cpp loads twice, once without an order book and once with it, so an order can
        // already be in storage from the first pass. Restore it once, and book it on the pass that
        // has a book.
        OrderEntry *restored = orderStorage_->locateByOrderId(it->first);
        if (nullptr == restored)
        {
            // The strings its record carries (#192), under the ids its fields name, before the order: storing it
            // indexes its ClOrdID. A replacement carries its original's ClOrdID, which the original's own record, or
            // an older directory's separate record, may have restored first; the store keeps the first and frees the
            // copy (#61), and both hold the one entry's value.
            assert(nullptr != storage_);
            if (carried.clOrderId)
            {
                storage_->restore(new RawDataEntry(std::move(*carried.clOrderId)));
            }
            if (carried.origClOrderId)
            {
                storage_->restore(new RawDataEntry(std::move(*carried.origClOrderId)));
            }
            if (carried.source)
            {
                storage_->restore(order->source_.getId(), new StringT(std::move(*carried.source)));
            }
            if (carried.destination)
            {
                storage_->restore(order->destination_.getId(), new StringT(std::move(*carried.destination)));
            }
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
        // Only a working order is booked: a live book holds only working orders, and a live market order is open only
        // while it is one (#207). Every other status is out of the book: finished, never accepted, or suspended, which
        // takes an order out until it is resumed (#215). A list of the statuses to leave out missed that one.
        if ((nullptr == orderBook_) || !restored->working())
        {
            continue;
        }
        // A held stop goes back into its instrument's stop book, in id order, so stops at one price keep their order,
        // and waits there for a trade: no trade price survives a restart (#223)
        if (restored->heldStop())
        {
            orderBook_->addStop(*restored);
            continue;
        }
        // An open order that never rests, or one that crosses an order that arrived before it, lost what its booking
        // started: the transaction that booked it committed, and the one with its fills, or with the cancel of what it
        // couldn't trade, didn't (#207). One that never rests is a market order, a STOP its stop released, or an IOC or
        // FOK order (#489): what it doesn't trade when it is matched is canceled, so it is never left open (its book()
        // is NONE here, since a held stop went to the stop book above). One that crosses an earlier order trades with
        // it. Each is left for the server to match, in this order, and not booked as it is: an IOC or FOK order booked
        // so rested, and traded with an order that arrived after it, and a crossing order left the book crossed.
        if (restored->cancelsWhatIsLeft() || crossesAnEarlierOrder(restored))
        {
            ordersToMatch_.push_back(restored);
        }
        else
        {
            orderBook_->restore(*restored);
        }
    }
    pendingOrders_.clear();

    // The held stops each instrument's last trade reached (#237). A trade's release is a transaction of its own, after
    // the trade's, so a crash between the two leaves a stop held that the trade reached. Only a stop that was held when
    // the trade was made: one that arrived after it waits for the next trade, as it would have.
    stopsReached_.clear();
    if (nullptr != orderBook_)
    {
        for (const auto &[instrument, trade] : lastTrades_)
        {
            if (!orderBook_->holdsStops(instrument))
            {
                continue;
            }
            OrderBook::OrdersT reached;
            orderBook_->triggeredStops(instrument, trade.lastPx_, &reached);
            for (const IdT &stopId : reached)
            {
                OrderEntry *stop = (nullptr != orderStorage_) ? orderStorage_->locateByOrderId(stopId) : nullptr;
                if ((nullptr != stop) && (stop->orderId_.id_ < trade.tradeId_.id_))
                {
                    stopsReached_.push_back(stop);
                }
            }
        }
    }
    lastTrades_.clear();
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

namespace
{
/// The strings an order's record carries (#192): the entries its fields name, each when the store has it. They used to
/// be records of their own, each written in its own commit, on the thread that took the order.
Codec::OrderStrings orderStringsOf(const OrderEntry &val, const DataStorageRestore *store)
{
    Codec::OrderStrings strings;
    if (nullptr == store)
    {
        return strings;
    }
    RawDataEntry raw;
    if (Codec::namesEntry(val.clOrderId_.getId()) && store->find(val.clOrderId_.getId(), &raw))
    {
        strings.clOrderId = raw;
    }
    if (Codec::namesEntry(val.origClOrderId_.getId()) && store->find(val.origClOrderId_.getId(), &raw))
    {
        strings.origClOrderId = raw;
    }
    StringT str;
    if (Codec::namesEntry(val.source_.getId()) && store->find(val.source_.getId(), &str))
    {
        strings.source = str;
    }
    if (Codec::namesEntry(val.destination_.getId()) && store->find(val.destination_.getId(), &str))
    {
        strings.destination = str;
    }
    return strings;
}
} // namespace

u32 StorageRecordDispatcher::save(const OrderEntry &val, const OrderFigures &figures)
{
    string buffer = recordWord(ORDER_RECORDTYPE, Codec::OrderCodec::FORMAT_VERSION);
    IdT id;
    u32 version;
    Codec::OrderCodec::encode(val, figures, &buffer, &id, &version, orderStringsOf(val, storage_));
    // update() appends a new version and returns it; for an order written for the first time that
    // version is 0. save() would reject the second write of an order that changed (issue #20).
    const u32 written = fileStorage_->update(id, buffer.c_str(), buffer.size());
#ifdef BUILD_PG
    if (pgWriter_)
    {
        pgWriter_->enqueue(PG::PGRequestBuilder::fromOrder(val, figures));
    }
#endif
    return written;
}

void StorageRecordDispatcher::erase(const IdT &orderId, u32 version)
{
    assert(nullptr != fileStorage_);
    fileStorage_->erase(orderId, version);
#ifdef BUILD_PG
    if (pgWriter_)
    {
        // Postgres follows LMDB back (#151): to the newest version a rollback left, or to no row when it left none. save()
        // had mirrored the erased version already. This runs inside a rollback, whose remaining steps an exception would
        // skip, so it never throws.
        try
        {
            std::vector<char> record;
            switch (fileStorage_->loadNewest(orderId, &record))
            {
            case FileSaver::Newest::Found:
            {
                u32 word = 0;
                if (record.size() < sizeof(word))
                {
                    throw std::runtime_error("a record shorter than its record word");
                }
                std::memcpy(&word, record.data(), sizeof(word));
                std::unique_ptr<OrderEntry> order(Codec::OrderCodec::decode(
                    orderId, word >> 16, record.data() + sizeof(word), record.size() - sizeof(word)));
                pgWriter_->enqueue(PG::PGRequestBuilder::fromOrder(*order));
                break;
            }
            case FileSaver::Newest::None:
                pgWriter_->enqueue(PG::DeleteOrderWrite{ orderId.id_, orderId.date_ });
                break;
            case FileSaver::Newest::Unknown:
                aux::ExchLogger::instance()->warn("Postgres can't follow the rollback of order " +
                                                  std::to_string(orderId.id_) +
                                                  ": the storage can't say what it holds");
                break;
            }
        }
        catch (const std::exception &e)
        {
            aux::ExchLogger::instance()->error("Postgres can't follow the rollback of order " +
                                               std::to_string(orderId.id_) + ": " + e.what());
        }
    }
#endif
}

#ifdef BUILD_PG
void StorageRecordDispatcher::syncToPG(const WideParamsDataStorage &wideData) const
{
    if (nullptr == pgWriter_)
    {
        return;
    }
    // Reference data first: an order's row looks its instrument, account and clearing firm up when it is written
    wideData.forEachInstrument(
        [this](const SourceIdT &, const InstrumentEntry &entry)
        {
            pgWriter_->enqueue(PG::PGRequestBuilder::fromInstrument(entry));
        });
    wideData.forEachAccount(
        [this](const SourceIdT &, const AccountEntry &entry)
        {
            pgWriter_->enqueue(PG::PGRequestBuilder::fromAccount(entry));
        });
    wideData.forEachClearing(
        [this](const SourceIdT &, const ClearingEntry &entry)
        {
            pgWriter_->enqueue(PG::PGRequestBuilder::fromClearing(entry));
        });
    if (nullptr != orderStorage_)
    {
        orderStorage_->forEachOrder(
            [this](const IdT &, const OrderEntry &order)
            {
                pgWriter_->enqueue(PG::PGRequestBuilder::fromOrder(order));
            });
    }
}
#endif

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

u32 StorageRecordDispatcher::saveLastTrade(const SourceIdT &instrument, PriceT lastPx, const IdT &tradeId)
{
    assert(nullptr != fileStorage_);
    string buffer = recordWord(LASTTRADE_RECORDTYPE, LAST_TRADE_FORMAT_VERSION);
    appendId(&buffer, instrument);
    buffer.append(reinterpret_cast<const char *>(&lastPx), sizeof(lastPx));
    appendId(&buffer, tradeId);
    assert(LAST_TRADE_RECORD_SIZE == buffer.size());
    // A new version, which erases all but the newest older one (#148), as an order's does
    return fileStorage_->update(lastTradeKey(instrument), buffer.data(), buffer.size());
}

void StorageRecordDispatcher::eraseLastTrade(const SourceIdT &instrument, u32 version)
{
    assert(nullptr != fileStorage_);
    fileStorage_->erase(lastTradeKey(instrument), version);
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
