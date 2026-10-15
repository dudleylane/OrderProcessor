/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cstring>
#include <memory>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockStorage.h"
#include "MockOrderBook.h"

#include "DataModelDef.h"
#include "StorageRecordDispatcher.h"
#include "InstrumentCodec.h"
#include "StringTCodec.h"
#include "AccountCodec.h"
#include "ClearingCodec.h"
#include "RawDataCodec.h"
#include "OrderCodec.h"
#include "OrderStorage.h"
#include "LMDBStorage.h"
#include "IdTGenerator.h"
#include "OrderBookImpl.h"

#include <filesystem>

#ifdef BUILD_PG
#include <pqxx/pqxx>

#include "PGWriteBehind.h"
#include "WideDataStorage.h"
#endif

using namespace COP;
using namespace COP::Codec;
using namespace COP::Store;
using namespace test;

namespace
{

// =============================================================================
// Test Data Storage Restore Implementation
// =============================================================================

class TestDataStorageRestore : public DataStorageRestore
{
public:
    ~TestDataStorageRestore() override
    {
        for (auto *instr : instruments_)
        {
            delete instr;
        }
        for (auto *acc : accounts_)
        {
            delete acc;
        }
        for (auto &str : strings_)
        {
            delete str.str_;
        }
        for (auto *raw : rawDatas_)
        {
            delete raw;
        }
        for (auto *clr : clearings_)
        {
            delete clr;
        }
        for (auto &lst : executionLists_)
        {
            delete lst.second;
        }
    }

    void restore(InstrumentEntry *val) override
    {
        ASSERT_NE(nullptr, val);
        instruments_.push_back(val);
    }

    void restore(const IdT &id, StringT *val) override
    {
        ASSERT_NE(nullptr, val);
        strings_.push_back(IdString(val, id));
    }

    void restore(RawDataEntry *val) override
    {
        ASSERT_NE(nullptr, val);
        rawDatas_.push_back(val);
    }

    void restore(AccountEntry *val) override
    {
        ASSERT_NE(nullptr, val);
        accounts_.push_back(val);
    }

    void restore(ClearingEntry *val) override
    {
        ASSERT_NE(nullptr, val);
        clearings_.push_back(val);
    }

    void restore(const IdT &id, ExecutionsT *val) override
    {
        ASSERT_NE(nullptr, val);
        executionLists_.push_back(std::make_pair(id, val));
    }

    // What an order's record carries is looked up here (#192): what was restored, as the real storage would have it
    bool find(const SourceIdT &id, StringT *val) const override
    {
        for (const IdString &str : strings_)
        {
            if (id == str.id_)
            {
                *val = *str.str_;
                return true;
            }
        }
        return false;
    }
    bool find(const SourceIdT &id, RawDataEntry *val) const override
    {
        for (const RawDataEntry *raw : rawDatas_)
        {
            if (id == raw->id_)
            {
                *val = *raw;
                return true;
            }
        }
        return false;
    }

public:
    std::deque<InstrumentEntry *> instruments_;
    std::deque<AccountEntry *> accounts_;
    std::deque<ClearingEntry *> clearings_;
    std::deque<RawDataEntry *> rawDatas_;
    std::deque<std::pair<IdT, ExecutionsT *>> executionLists_;

    struct IdString
    {
        StringT *str_;
        IdT id_;

        IdString() : str_(nullptr), id_() {}
        IdString(StringT *v, const IdT &id) : str_(v), id_(id) {}
    };
    std::deque<IdString> strings_;
};

// =============================================================================
// Test Order Book Implementation
// =============================================================================

class TestOrderBook : public OrderBook
{
public:
    ~TestOrderBook() override = default;

    void add(const OrderEntry & /*order*/) override {}

    void remove(const OrderEntry & /*order*/) override {}

    IdT find(const OrderFunctor & /*functor*/) const override
    {
        return IdT();
    }

    void findAll(const OrderFunctor & /*functor*/, OrdersT * /*result*/) const override {}

    IdT getTop(const SourceIdT & /*instrument*/, const Side & /*side*/) const override
    {
        return IdT();
    }

    void restore(const OrderEntry &order) override
    {
        orders_.push_back(&order); // Just for testing
    }

    // A held stop goes here at restore, not into orders_ (#223)
    void addStop(const OrderEntry &order) override
    {
        stops_.push_back(&order);
    }
    void removeStop(const OrderEntry & /*order*/) override {}
    bool holdsStops(const SourceIdT & /*instrument*/) const override
    {
        return !stops_.empty();
    }
    void triggeredStops(const SourceIdT & /*instrument*/, PriceT /*lastPx*/, OrdersT * /*result*/) const override {}

    std::deque<const OrderEntry *> orders_;
    std::deque<const OrderEntry *> stops_;
};

// =============================================================================
// Test File Saver Implementation
// =============================================================================

class TestFileSaver : public FileSaver
{
public:
    TestFileSaver() = default;
    ~TestFileSaver() override = default;

    void load(const std::string & /*fileName*/, FileStorageObserver * /*observer*/) override {}

    IdT save(const char *buf, size_t size) override
    {
        EXPECT_NE(nullptr, buf);
        EXPECT_GT(size, 0u);
        IdT id;
        records_.insert(RecordsT::value_type(id, Record(std::string(buf, size))));
        return id;
    }

    void save(const IdT &id, const char *buf, size_t size) override
    {
        EXPECT_NE(nullptr, buf);
        EXPECT_GT(size, 0u);
        records_.insert(RecordsT::value_type(id, Record(std::string(buf, size))));
    }

    u32 update(const IdT &id, const char *buf, size_t size) override
    {
        EXPECT_NE(nullptr, buf);
        EXPECT_GT(size, 0u);
        // Mirrors LMDBStorage::update(): each call appends a new version and returns it. Only the
        // newest content is kept here, which is what the dispatcher tests look at.
        RecordsT::iterator it = records_.find(id);
        if (records_.end() == it)
        {
            records_.insert(RecordsT::value_type(id, Record(std::string(buf, size))));
            return 0;
        }
        it->second.record_.assign(buf, size);
        it->second.version_ += 1;
        return it->second.version_;
    }

    u32 replace(const IdT & /*id*/, u32 /*version*/, const char * /*buf*/, size_t /*size*/) override
    {
        return 0;
    }

    void erase(const IdT &id, u32 version) override
    {
        RecordsT::iterator it = records_.find(id);
        if ((records_.end() != it) && (it->second.version_ == version))
        {
            records_.erase(it);
        }
    }

    void erase(const IdT & /*id*/) override {}

    struct Record
    {
        std::string record_;
        u32 version_;

        Record() : record_(), version_(0) {}
        Record(const std::string &rec, u32 version = 0) : record_(rec), version_(version) {}
    };
    typedef std::map<IdT, Record> RecordsT;
    RecordsT records_;
};

// =============================================================================
// Test Helpers
// =============================================================================

SourceIdT addTestRawData(const std::string &name)
{
    auto raw = new RawDataEntry();
    raw->type_ = STRING_RAWDATATYPE;
    auto val = new char[name.size()];
    std::memcpy(val, name.c_str(), name.size());
    raw->data_ = val;
    raw->length_ = name.size();
    return WideDataStorage::instance()->add(raw);
}

OrderEntry *createTestOrder()
{
    SourceIdT instrId = addInstrument("instrument", "instrId", "instrSrc");
    SourceIdT clOrderId = addTestRawData("clOrderId_");
    auto val = new OrderEntry(SourceIdT(1, 1), SourceIdT(2, 2), clOrderId, SourceIdT(4, 4), instrId, SourceIdT(6, 6),
                              SourceIdT(7, 7), SourceIdT(8, 8));

    val->creationTime_ = 1111;
    val->lastUpdateTime_ = 1112;
    val->expireTime_ = 1113;
    val->settlDate_ = 1114;
    val->price_ = 22.22;
    val->stopPx_ = 23.23;
    val->avgPx_ = 24.24;
    val->dayAvgPx_ = 24.24;
    val->status_ = REJECTED_ORDSTATUS;
    val->side_ = BUY_SIDE;
    val->ordType_ = LIMIT_ORDERTYPE;
    val->tif_ = GTD_TIF;
    val->settlType_ = _4_SETTLTYPE;
    val->capacity_ = PROPRIETARY_CAPACITY;
    val->currency_ = USD_CURRENCY;
    val->minQty_ = 3333;
    val->orderQty_ = 3334;
    val->leavesQty_ = 3335;
    val->cumQty_ = 3336;
    val->dayOrderQty_ = 3337;
    val->dayCumQty_ = 3338;
    val->orderId_ = IdT(4444, 4445);
    val->origOrderId_ = IdT(5555, 5556);
    val->stateMachinePersistance_.orderData_ = val;
    val->stateMachinePersistance_.stateZone1Id_ = 6;
    val->stateMachinePersistance_.stateZone2Id_ = 7;

    return val;
}

/// A record's first 4 bytes, as the dispatcher writes them: the type, and in the high 16 bits the version of the layout
/// its codec writes now (#149)
std::string createRecordTypePrefix(int recordType)
{
    u32 formatVersion = 0;
    switch (recordType)
    {
    case StorageRecordDispatcher::INSTRUMENT_RECORDTYPE:
        formatVersion = InstrumentCodec::FORMAT_VERSION;
        break;
    case StorageRecordDispatcher::STRING_RECORDTYPE:
        formatVersion = StringTCodec::FORMAT_VERSION;
        break;
    case StorageRecordDispatcher::ACCOUNT_RECORDTYPE:
        formatVersion = AccountCodec::FORMAT_VERSION;
        break;
    case StorageRecordDispatcher::CLEARING_RECORDTYPE:
        formatVersion = ClearingCodec::FORMAT_VERSION;
        break;
    case StorageRecordDispatcher::RAWDATA_RECORDTYPE:
        formatVersion = RawDataCodec::FORMAT_VERSION;
        break;
    case StorageRecordDispatcher::ORDER_RECORDTYPE:
        formatVersion = OrderCodec::FORMAT_VERSION;
        break;
    default:
        break;
    }
    const u32 word = (formatVersion << 16) | static_cast<u32>(recordType);
    std::string buf(sizeof(word), '\0');
    std::memcpy(buf.data(), &word, sizeof(word));
    return buf;
}

// =============================================================================
// Test Fixture
// =============================================================================

class StorageRecordDispatcherTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        WideDataStorage::create();
        restore_ = std::make_unique<TestDataStorageRestore>();
        orderBook_ = std::make_unique<TestOrderBook>();
        saver_ = std::make_unique<TestFileSaver>();
        orderStorage_ = std::make_unique<OrderDataStorage>();
        dispatcher_ = std::make_unique<StorageRecordDispatcher>();
    }

    void TearDown() override
    {
        dispatcher_.reset();
        orderStorage_.reset();
        saver_.reset();
        orderBook_.reset();
        restore_.reset();
        WideDataStorage::destroy();
    }

protected:
    std::unique_ptr<TestDataStorageRestore> restore_;
    std::unique_ptr<TestOrderBook> orderBook_;
    std::unique_ptr<TestFileSaver> saver_;
    std::unique_ptr<OrderDataStorage> orderStorage_;
    std::unique_ptr<StorageRecordDispatcher> dispatcher_;
};

// =============================================================================
// Initialization Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, EmptyInitialization)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    SUCCEED();
}

TEST_F(StorageRecordDispatcherTest, InitWithStartLoadFinishLoad)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();
    dispatcher_->finishLoad();
    SUCCEED();
}

// =============================================================================
// Load Instrument Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadInstrumentRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    InstrumentEntry val;
    val.id_ = IdT(1111, 6789);
    val.securityId_ = "securityId_";
    val.securityIdSource_ = "securityIdSource_";
    val.symbol_ = "symbol_";

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::INSTRUMENT_RECORDTYPE);
    IdT id;
    u32 version = 0;
    InstrumentCodec::encode(val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    ASSERT_EQ(1u, restore_->instruments_.size());
    ASSERT_NE(nullptr, restore_->instruments_.at(0));
    EXPECT_EQ(val.id_, restore_->instruments_.at(0)->id_);
    EXPECT_EQ(val.securityId_, restore_->instruments_.at(0)->securityId_);
    EXPECT_EQ(val.securityIdSource_, restore_->instruments_.at(0)->securityIdSource_);
    EXPECT_EQ(val.symbol_, restore_->instruments_.at(0)->symbol_);

    dispatcher_->finishLoad();
}

// =============================================================================
// Load String Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadStringRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    StringT val = "string_";
    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::STRING_RECORDTYPE);
    IdT id;
    u32 version = 0;
    StringTCodec::encode(val, &buf);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    ASSERT_EQ(1u, restore_->strings_.size());
    EXPECT_EQ(id, restore_->strings_.at(0).id_);
    ASSERT_NE(nullptr, restore_->strings_.at(0).str_);
    EXPECT_EQ("string_", *(restore_->strings_.at(0).str_));

    dispatcher_->finishLoad();
}

// =============================================================================
// Load Account Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadAccountRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    AccountEntry val;
    val.id_ = IdT(1111, 6789);
    val.account_ = "account_";
    val.firm_ = "firm_";
    val.type_ = PRINCIPAL_ACCOUNTTYPE;

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ACCOUNT_RECORDTYPE);
    IdT id;
    u32 version = 0;
    AccountCodec::encode(val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    ASSERT_EQ(1u, restore_->accounts_.size());
    ASSERT_NE(nullptr, restore_->accounts_.at(0));
    EXPECT_EQ(val.id_, restore_->accounts_.at(0)->id_);
    EXPECT_EQ(val.account_, restore_->accounts_.at(0)->account_);
    EXPECT_EQ(val.firm_, restore_->accounts_.at(0)->firm_);
    EXPECT_EQ(val.type_, restore_->accounts_.at(0)->type_);

    dispatcher_->finishLoad();
}

// =============================================================================
// Load Clearing Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadClearingRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    ClearingEntry val;
    val.id_ = IdT(1111, 6789);
    val.firm_ = "firm_";

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::CLEARING_RECORDTYPE);
    IdT id;
    u32 version = 0;
    ClearingCodec::encode(val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    ASSERT_EQ(1u, restore_->clearings_.size());
    ASSERT_NE(nullptr, restore_->clearings_.at(0));
    EXPECT_EQ(val.id_, restore_->clearings_.at(0)->id_);
    EXPECT_EQ(val.firm_, restore_->clearings_.at(0)->firm_);

    dispatcher_->finishLoad();
}

// =============================================================================
// Load RawData Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadRawDataRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    RawDataEntry val(STRING_RAWDATATYPE, "RawDataValue_", 13);
    val.id_ = IdT(1111, 6789);

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::RAWDATA_RECORDTYPE);
    IdT id;
    u32 version = 0;
    RawDataCodec::encode(val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    ASSERT_EQ(1u, restore_->rawDatas_.size());
    ASSERT_NE(nullptr, restore_->rawDatas_.at(0));
    EXPECT_EQ(val.id_, restore_->rawDatas_.at(0)->id_);
    EXPECT_EQ(val.length_, restore_->rawDatas_.at(0)->length_);
    EXPECT_EQ(val.type_, restore_->rawDatas_.at(0)->type_);
    EXPECT_EQ(0, std::memcmp(restore_->rawDatas_.at(0)->data_, val.data_, val.length_));

    dispatcher_->finishLoad();
}

// =============================================================================
// Load Order Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadOrderRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    std::unique_ptr<OrderEntry> val(createTestOrder());
    val->orderId_ = IdT(1111, 6789);
    val->status_ = NEW_ORDSTATUS;
    val->orderQty_ = 100;
    val->leavesQty_ = 100;

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 version = 0;
    OrderCodec::encode(*val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    // Orders reach the book and the storage at finishLoad(), once the newest version of each is
    // known: the loader replays every version of every record (#20).
    EXPECT_TRUE(orderBook_->orders_.empty());
    dispatcher_->finishLoad();

    ASSERT_EQ(1u, orderBook_->orders_.size());
    ASSERT_NE(nullptr, orderBook_->orders_.at(0));
    EXPECT_TRUE(orderBook_->orders_.at(0)->compare(*val));
    EXPECT_NE(nullptr, orderStorage_->locateByOrderId(id));
}

TEST_F(StorageRecordDispatcherTest, LoadTerminalOrderIsRestoredButNotBooked)
{
    // A terminal order must not come back resting in the book. Before #20 only pre-acceptance
    // snapshots existed on disk, so every record was booked; now the final state is persisted.
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    std::unique_ptr<OrderEntry> val(createTestOrder());
    val->orderId_ = IdT(2222, 6789);
    val->status_ = FILLED_ORDSTATUS;
    val->orderQty_ = 100;
    val->cumQty_ = 100;
    val->leavesQty_ = 0;

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 version = 0;
    OrderCodec::encode(*val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    dispatcher_->finishLoad();

    EXPECT_TRUE(orderBook_->orders_.empty());
    OrderEntry *restored = orderStorage_->locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(FILLED_ORDSTATUS, restored->status_);
    EXPECT_EQ(0, restored->leavesQty_);
}

TEST_F(StorageRecordDispatcherTest, LoadOrderKeepsTheNewestVersion)
{
    // The loader replays every version of a record. Only the newest may be restored, whatever order
    // the versions arrive in (#20).
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    std::unique_ptr<OrderEntry> val(createTestOrder());
    val->orderId_ = IdT(3333, 6789);
    val->status_ = NEW_ORDSTATUS;
    val->orderQty_ = 100;
    val->leavesQty_ = 100;

    std::string first = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 encodedVersion = 0;
    OrderCodec::encode(*val, &first, &id, &encodedVersion);

    val->status_ = PARTFILL_ORDSTATUS;
    val->cumQty_ = 60;
    val->leavesQty_ = 40;
    std::string second = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    OrderCodec::encode(*val, &second, &id, &encodedVersion);

    // newest first, to prove arrival order does not decide
    dispatcher_->onRecordLoaded(id, 1, second.c_str(), second.size());
    dispatcher_->onRecordLoaded(id, 0, first.c_str(), first.size());
    dispatcher_->finishLoad();

    OrderEntry *restored = orderStorage_->locateByOrderId(id);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(PARTFILL_ORDSTATUS, restored->status_);
    EXPECT_EQ(40, restored->leavesQty_);
    EXPECT_EQ(60, restored->cumQty_);
    EXPECT_EQ(1u, orderBook_->orders_.size());
}

// =============================================================================
// Load Multiple Record Types Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadMultipleRecordTypes)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    // Load Instrument
    {
        InstrumentEntry val;
        val.id_ = IdT(1111, 6789);
        val.securityId_ = "securityId_";
        val.securityIdSource_ = "securityIdSource_";
        val.symbol_ = "symbol_";

        std::string buf = createRecordTypePrefix(StorageRecordDispatcher::INSTRUMENT_RECORDTYPE);
        IdT id;
        u32 version = 0;
        InstrumentCodec::encode(val, &buf, &id, &version);
        dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    }

    // Load String
    {
        StringT val = "string_";
        std::string buf = createRecordTypePrefix(StorageRecordDispatcher::STRING_RECORDTYPE);
        IdT id;
        u32 version = 0;
        StringTCodec::encode(val, &buf);
        dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    }

    // Load Account
    {
        AccountEntry val;
        val.id_ = IdT(1111, 6789);
        val.account_ = "account_";
        val.firm_ = "firm_";
        val.type_ = PRINCIPAL_ACCOUNTTYPE;

        std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ACCOUNT_RECORDTYPE);
        IdT id;
        u32 version = 0;
        AccountCodec::encode(val, &buf, &id, &version);
        dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    }

    // Load Clearing
    {
        ClearingEntry val;
        val.id_ = IdT(1111, 6789);
        val.firm_ = "firm_";

        std::string buf = createRecordTypePrefix(StorageRecordDispatcher::CLEARING_RECORDTYPE);
        IdT id;
        u32 version = 0;
        ClearingCodec::encode(val, &buf, &id, &version);
        dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    }

    // Load RawData
    {
        RawDataEntry val(STRING_RAWDATATYPE, "RawDataValue_", 13);
        val.id_ = IdT(1111, 6789);

        std::string buf = createRecordTypePrefix(StorageRecordDispatcher::RAWDATA_RECORDTYPE);
        IdT id;
        u32 version = 0;
        RawDataCodec::encode(val, &buf, &id, &version);
        dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    }

    // Verify all loaded
    EXPECT_EQ(1u, restore_->instruments_.size());
    EXPECT_EQ(1u, restore_->strings_.size());
    EXPECT_EQ(1u, restore_->accounts_.size());
    EXPECT_EQ(1u, restore_->clearings_.size());
    EXPECT_EQ(1u, restore_->rawDatas_.size());

    dispatcher_->finishLoad();
}

// =============================================================================
// Save Instrument Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveInstrumentRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    InstrumentEntry val;
    val.id_ = IdT(1111, 6789);
    val.securityId_ = "securityId_";
    val.securityIdSource_ = "securityIdSource_";
    val.symbol_ = "symbol_";

    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::INSTRUMENT_RECORDTYPE);
    IdT id;
    u32 version = 0;
    InstrumentCodec::encode(val, &expectedBuf, &id, &version);

    dispatcher_->save(val);

    ASSERT_EQ(1u, saver_->records_.size());
    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save String Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveStringRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    StringT val = "string_";
    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::STRING_RECORDTYPE);
    IdT id(1, 1234);
    StringTCodec::encode(val, &expectedBuf);

    dispatcher_->save(id, val);

    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save Account Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveAccountRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    AccountEntry val;
    val.id_ = IdT(1112, 6789);
    val.account_ = "account_";
    val.firm_ = "firm_";
    val.type_ = PRINCIPAL_ACCOUNTTYPE;

    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::ACCOUNT_RECORDTYPE);
    IdT id;
    u32 version = 0;
    AccountCodec::encode(val, &expectedBuf, &id, &version);

    dispatcher_->save(val);

    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save Clearing Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveClearingRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    ClearingEntry val;
    val.id_ = IdT(1113, 6789);
    val.firm_ = "firm_";

    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::CLEARING_RECORDTYPE);
    IdT id;
    u32 version = 0;
    ClearingCodec::encode(val, &expectedBuf, &id, &version);

    dispatcher_->save(val);

    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save RawData Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveRawDataRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    RawDataEntry val(STRING_RAWDATATYPE, "RawDataValue_", 13);
    val.id_ = IdT(1114, 6789);

    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::RAWDATA_RECORDTYPE);
    IdT id;
    u32 version = 0;
    RawDataCodec::encode(val, &expectedBuf, &id, &version);

    dispatcher_->save(val);

    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save Order Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveOrderRecord)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    std::unique_ptr<OrderEntry> val(createTestOrder());
    val->orderId_ = IdT(1115, 6789);

    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 version = 0;
    OrderCodec::encode(*val, &expectedBuf, &id, &version);

    dispatcher_->save(*val);

    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save Multiple Record Types Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveMultipleRecordTypes)
{
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    // Save Instrument
    {
        InstrumentEntry val;
        val.id_ = IdT(2001, 1);
        val.securityId_ = "sec1";
        val.securityIdSource_ = "src1";
        val.symbol_ = "SYM1";
        dispatcher_->save(val);
    }

    // Save Account
    {
        AccountEntry val;
        val.id_ = IdT(2002, 1);
        val.account_ = "acc1";
        val.firm_ = "firm1";
        val.type_ = AGENCY_ACCOUNTTYPE;
        dispatcher_->save(val);
    }

    // Save Clearing
    {
        ClearingEntry val;
        val.id_ = IdT(2003, 1);
        val.firm_ = "clearFirm1";
        dispatcher_->save(val);
    }

    // Save RawData
    {
        RawDataEntry val(STRING_RAWDATATYPE, "rawdata1", 8);
        val.id_ = IdT(2004, 1);
        dispatcher_->save(val);
    }

    // Verify all saved (4 records)
    EXPECT_EQ(4u, saver_->records_.size());
}

} // namespace

// =============================================================================
// Restart Round Trip (#20)
// =============================================================================

#ifdef BUILD_PG
namespace
{
/// The database scripts/pg-for-tests.sh starts, named by PG_CONNECTION_STRING (#151)
std::string pgConnection()
{
    const char *env = std::getenv("PG_CONNECTION_STRING");
    return (nullptr != env) ? std::string(env) : std::string();
}

std::string pgSelect(const std::string &connection, const std::string &sql)
{
    pqxx::connection conn(connection);
    pqxx::nontransaction txn(conn);
    const pqxx::result rows = txn.exec(sql);
    return (rows.empty() || rows[0][0].is_null()) ? std::string() : rows[0][0].as<std::string>();
}

/// Removes what an earlier run of a test left of an order, so the test sees only its own writes
void pgForget(const std::string &connection, u64 orderNumber)
{
    pqxx::connection conn(connection);
    pqxx::work txn(conn);
    txn.exec("DELETE FROM orders WHERE order_id = " + std::to_string(orderNumber));
    txn.commit();
}

/// A resting order whose every reference WideDataStorage holds, as PGRequestBuilder needs
OrderEntry *pgTestOrder(u64 orderNumber)
{
    WideParamsDataStorage *wide = WideDataStorage::instance();
    const SourceIdT instrument = addInstrument("PGDISP_INSTR", "S", "SRC");
    const SourceIdT account = test::addAccount("PGDISP_ACCT");
    const SourceIdT clearing = test::addClearing("PGDISP_CLR");
    const SourceIdT source = wide->add(new StringT("SRC"));
    const SourceIdT destination = wide->add(new StringT("DST"));
    const SourceIdT clOrderId = addTestRawData("PGDISP-CL-" + std::to_string(orderNumber));
    const SourceIdT executions = wide->add(new ExecutionsT());
    auto order = new OrderEntry(source, destination, clOrderId, SourceIdT(), instrument, account, clearing, executions);
    order->orderId_ = IdT(orderNumber, 1792022400);
    order->status_ = NEW_ORDSTATUS;
    order->side_ = BUY_SIDE;
    order->ordType_ = LIMIT_ORDERTYPE;
    order->tif_ = DAY_TIF;
    order->settlType_ = _2_SETTLTYPE;
    order->capacity_ = AGENCY_CAPACITY;
    order->currency_ = USD_CURRENCY;
    order->price_ = 10.5;
    order->orderQty_ = 100;
    order->leavesQty_ = 100;
    order->stateMachinePersistance_.orderData_ = order;
    order->stateMachinePersistance_.stateZone1Id_ = 6;
    order->stateMachinePersistance_.stateZone2Id_ = 7;
    return order;
}
} // namespace

TEST_F(StorageRecordDispatcherTest, PostgresFollowsARollbackToTheVersionLmdbKeeps)
{
    // save() mirrors each version to Postgres as LMDB takes it, and a rollback used to leave Postgres with the version
    // LMDB erased (#151): now the row goes back to LMDB's newest version, or away when none is left.
    const std::string connection = pgConnection();
    if (connection.empty())
    {
        GTEST_SKIP() << "PG_CONNECTION_STRING isn't set: scripts/pg-for-tests.sh starts a database for this";
    }
    pgForget(connection, 151101);
    const std::string dir = test::uniqueTestPath("dispatcher-pg-rollback");
    std::unique_ptr<OrderEntry> order(pgTestOrder(151101));
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        {
            PG::PGWriteBehind writer(connection);
            dispatcher_->setPGWriter(&writer);
            dispatcher_->syncToPG(*WideDataStorage::instance()); // the order's instrument, account and clearing firm
            dispatcher_->save(*order);                           // resting
            order->status_ = FILLED_ORDSTATUS;
            order->cumQty_ = 100;
            order->leavesQty_ = 0;
            const u32 filled = dispatcher_->save(*order);
            dispatcher_->erase(order->orderId_, filled); // the fill's transaction rolled back
            dispatcher_->setPGWriter(nullptr);
            writer.shutdown();
            EXPECT_EQ(0u, writer.totalErrors());
        }
        EXPECT_EQ("NEW", pgSelect(connection, "SELECT status::text FROM orders WHERE order_id = 151101"));
        EXPECT_EQ("100", pgSelect(connection, "SELECT leaves_qty::text FROM orders WHERE order_id = 151101"));
        {
            // The first version too: a new order whose first persist rolled back has no version left
            PG::PGWriteBehind writer(connection);
            dispatcher_->setPGWriter(&writer);
            dispatcher_->erase(order->orderId_, 0);
            dispatcher_->setPGWriter(nullptr);
            writer.shutdown();
            EXPECT_EQ(0u, writer.totalErrors());
        }
        EXPECT_EQ("0", pgSelect(connection, "SELECT count(*) FROM orders WHERE order_id = 151101"));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, PostgresGetsWhatTheDirectoryHeldBeforeItWasWiredUp)
{
    // LMDB's load restores orders without save(), so a server started with Postgres for the first time would never have
    // mirrored them (#151): syncToPG() hands over everything the directory holds
    const std::string connection = pgConnection();
    if (connection.empty())
    {
        GTEST_SKIP() << "PG_CONNECTION_STRING isn't set: scripts/pg-for-tests.sh starts a database for this";
    }
    pgForget(connection, 151102);
    const std::string dir = test::uniqueTestPath("dispatcher-pg-sync");
    std::unique_ptr<OrderEntry> order(pgTestOrder(151102));
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        dispatcher_->save(*order); // no Postgres yet
    }
    {
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);
        ASSERT_NE(nullptr, storageAfter.locateByOrderId(order->orderId_));

        PG::PGWriteBehind writer(connection);
        dispatcherAfter.setPGWriter(&writer);
        dispatcherAfter.syncToPG(*WideDataStorage::instance());
        dispatcherAfter.setPGWriter(nullptr);
        writer.shutdown();
        EXPECT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("NEW", pgSelect(connection, "SELECT status::text FROM orders WHERE order_id = 151102"));
    EXPECT_EQ("PGDISP_INSTR", pgSelect(connection, "SELECT i.symbol FROM orders o JOIN instruments i ON "
                                                   "i.id = o.instrument_id WHERE o.order_id = 151102"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif

TEST_F(StorageRecordDispatcherTest, OrderSurvivesRestartWithItsFinalState)
{
    // The bug this covers: an order used to be written once, before acceptance, so a restart
    // brought filled orders back as live resting orders and lost every fill (#20).
    const std::string dir = test::uniqueTestPath("dispatcher-restart");
    std::unique_ptr<OrderEntry> order(createTestOrder());
    order->orderId_ = IdT(4444, 6789);
    order->status_ = NEW_ORDSTATUS;
    order->orderQty_ = 100;
    order->leavesQty_ = 100;
    const IdT orderId = order->orderId_;

    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());

        dispatcher_->save(*order); // resting
        order->status_ = FILLED_ORDSTATUS;
        order->cumQty_ = 100;
        order->leavesQty_ = 0;
        dispatcher_->save(*order); // filled: a second version, which save() would have rejected
    }

    {
        // restart: a fresh dispatcher, storage and book over the same directory
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);

        OrderEntry *restored = storageAfter.locateByOrderId(orderId);
        ASSERT_NE(nullptr, restored);
        EXPECT_EQ(FILLED_ORDSTATUS, restored->status_);
        EXPECT_EQ(0, restored->leavesQty_);
        EXPECT_EQ(100, restored->cumQty_);
        EXPECT_TRUE(bookAfter.orders_.empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// =============================================================================
// Execution Lists (#33)
// =============================================================================

TEST_F(StorageRecordDispatcherTest, EveryNewExecutionListCanBeAdded)
{
    // The bug this covers: every new list was written under the key IdT(list size, 0), so the second
    // order's empty list collided with the first, LMDBStorage::save threw, and the server terminated (#33).
    const std::string dir = test::uniqueTestPath("dispatcher-execlists");
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        WideDataStorage::instance()->bindStorage(dispatcher_.get());

        SourceIdT first;
        SourceIdT second;
        EXPECT_NO_THROW(first = WideDataStorage::instance()->add(new ExecutionsT()));
        EXPECT_NO_THROW(second = WideDataStorage::instance()->add(new ExecutionsT()));
        EXPECT_NE(first, second);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, ExecutionListRecordFromAnEarlierBuildIsSkipped)
{
    // A data directory from a build before #33 holds the one list record that build managed to write:
    // an empty list keyed (0,0). Loading must skip it, and the next list must not collide with it.
    const std::string dir = test::uniqueTestPath("dispatcher-old-execlist");
    {
        StorageRecordDispatcher seeder;
        LMDBStorage lmdb;
        seeder.init(restore_.get(), nullptr, &lmdb, orderStorage_.get());
        lmdb.load(dir, &seeder);
        std::string record = createRecordTypePrefix(StorageRecordDispatcher::EXECUTIONS_RECORDTYPE);
        u64 count = 0;
        record.append(reinterpret_cast<const char *>(&count), sizeof(count));
        lmdb.save(IdT(0, 0), record.data(), record.size());
    }
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        EXPECT_NO_THROW(lmdb.load(dir, dispatcher_.get()));
        WideDataStorage::instance()->bindStorage(dispatcher_.get());

        EXPECT_NO_THROW(WideDataStorage::instance()->add(new ExecutionsT()));
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, RestoredOrderGetsAnEmptyExecutionList)
{
    // Execution lists are not persisted, so finishLoad() registers an empty one under the id the order's
    // record holds (#33).
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    std::unique_ptr<OrderEntry> val(createTestOrder());
    val->orderId_ = IdT(1111, 6789);
    val->status_ = NEW_ORDSTATUS;
    val->orderQty_ = 100;
    val->leavesQty_ = 100;

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 version = 0;
    OrderCodec::encode(*val, &buf, &id, &version);
    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());
    dispatcher_->finishLoad();

    ASSERT_EQ(1u, restore_->executionLists_.size());
    EXPECT_EQ(val->executions_.getId(), restore_->executionLists_.at(0).first);
    ASSERT_NE(nullptr, restore_->executionLists_.at(0).second);
    EXPECT_TRUE(restore_->executionLists_.at(0).second->empty());
}

TEST_F(StorageRecordDispatcherTest, RestoredOrderCanRecordAnExecution)
{
    // Nothing used to register a list under the id a restored order's record holds, so the order's first
    // execution after a restart threw "execution list not found" (#33). createTestOrder()'s list id is
    // registered nowhere, which is exactly a restored order's position.
    const std::string dir = test::uniqueTestPath("dispatcher-restored-execlist");
    std::unique_ptr<OrderEntry> order(createTestOrder());
    order->status_ = NEW_ORDSTATUS;
    order->orderQty_ = 100;
    order->leavesQty_ = 100;
    const IdT orderId = order->orderId_;
    const SourceIdT listId = order->executions_.getId();

    {
        LMDBStorage lmdb;
        dispatcher_->init(WideDataStorage::instance(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        dispatcher_->save(*order);
    }

    {
        // restart: a fresh dispatcher, storage and book over the same directory
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(WideDataStorage::instance(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);

        OrderEntry *restored = storageAfter.locateByOrderId(orderId);
        ASSERT_NE(nullptr, restored);
        EXPECT_NO_THROW(restored->addExecution(IdT(9001, 1)));

        ExecutionsT *list = nullptr;
        EXPECT_NO_THROW(WideDataStorage::instance()->get(listId, &list));
        ASSERT_NE(nullptr, list);
        EXPECT_EQ(1u, list->size());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// =============================================================================
// Load Order (#49)
// =============================================================================

TEST_F(StorageRecordDispatcherTest, OrderRecordBeforeItsInstrumentIsRestored)
{
    // LMDB visits keys in byte order, so an order's record can come before its instrument's (#49). Decoding the
    // order right then built an OrderEntry, whose constructor loads the instrument, and that threw "instrument not
    // found": the server could not start.
    std::unique_ptr<OrderEntry> order(createTestOrder());
    order->orderId_ = IdT(4444, 6789);
    order->status_ = NEW_ORDSTATUS;
    order->orderQty_ = 100;
    order->leavesQty_ = 100;

    std::string orderRecord = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT orderId;
    u32 orderVersion = 0;
    OrderCodec::encode(*order, &orderRecord, &orderId, &orderVersion);

    InstrumentEntry instrument;
    WideDataStorage::instance()->get(order->instrument_.getId(), &instrument);
    std::string instrumentRecord = createRecordTypePrefix(StorageRecordDispatcher::INSTRUMENT_RECORDTYPE);
    IdT instrumentId;
    u32 instrumentVersion = 0;
    InstrumentCodec::encode(instrument, &instrumentRecord, &instrumentId, &instrumentVersion);

    RawDataEntry clOrderId;
    WideDataStorage::instance()->get(order->clOrderId_.getId(), &clOrderId);
    std::string rawRecord = createRecordTypePrefix(StorageRecordDispatcher::RAWDATA_RECORDTYPE);
    IdT rawId;
    u32 rawVersion = 0;
    RawDataCodec::encode(clOrderId, &rawRecord, &rawId, &rawVersion);

    // restart: nothing is restored yet when the order's record arrives
    WideDataStorage::destroy();
    WideDataStorage::create();
    dispatcher_->init(WideDataStorage::instance(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();
    EXPECT_NO_THROW(dispatcher_->onRecordLoaded(orderId, orderVersion, orderRecord.data(), orderRecord.size()));
    dispatcher_->onRecordLoaded(rawId, rawVersion, rawRecord.data(), rawRecord.size());
    dispatcher_->onRecordLoaded(instrumentId, instrumentVersion, instrumentRecord.data(), instrumentRecord.size());
    ASSERT_NO_THROW(dispatcher_->finishLoad());

    OrderEntry *restored = orderStorage_->locateByOrderId(orderId);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(instrument.symbol_, restored->instrument_.get().symbol_);
}

TEST_F(StorageRecordDispatcherTest, OrderWhoseKeySortsBeforeItsInstrumentSurvivesRestart)
{
    // The same through LMDB itself: keys compare as raw little-endian bytes, so order id 256 (00 01 ...) sorts
    // before instrument id 1 (01 00 ...). Once ids reached 256 the server could not restart (#49).
    const std::string dir = test::uniqueTestPath("dispatcher-load-order");
    const IdT orderId(256, 20260930);
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        WideDataStorage::instance()->bindStorage(dispatcher_.get());

        SourceIdT instrumentId = addInstrument("AAPL", "US0378331005", "ISIN");
        SourceIdT clOrderId = addTestRawData("CL-256");
        ASSERT_EQ(1u, instrumentId.id_);
        OrderEntry order(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrumentId, SourceIdT(), SourceIdT(),
                         SourceIdT());
        order.orderId_ = orderId;
        order.status_ = NEW_ORDSTATUS;
        order.side_ = BUY_SIDE;
        order.ordType_ = LIMIT_ORDERTYPE;
        order.orderQty_ = 100;
        order.leavesQty_ = 100;
        // the states every persisted order has been through: New, and no cancel or replace (#150)
        order.stateMachinePersistance_.stateZone1Id_ = 2;
        order.stateMachinePersistance_.stateZone2Id_ = 8;
        dispatcher_->save(order);
    }

    {
        // restart: fresh reference data, storage, book and dispatcher over the same directory
        WideDataStorage::destroy();
        WideDataStorage::create();
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(WideDataStorage::instance(), &bookAfter, &lmdbAfter, &storageAfter);
        ASSERT_NO_THROW(lmdbAfter.load(dir, &dispatcherAfter));

        OrderEntry *restored = storageAfter.locateByOrderId(orderId);
        ASSERT_NE(nullptr, restored);
        EXPECT_EQ("AAPL", restored->instrument_.get().symbol_);
        EXPECT_EQ(1u, bookAfter.orders_.size());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, OrderNumbersContinueAfterRestart)
{
    // The bug this covers: the id counter starts at 1 in every run, so after a restart a new order got a number a
    // restored order already had, and a client naming orders by number could not tell them apart (#58)
    const std::string dir = test::uniqueTestPath("dispatcher-order-numbers");
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        const u64 numbers[] = { 3, 9 };
        for (u64 number : numbers)
        {
            std::unique_ptr<OrderEntry> order(createTestOrder());
            order->clOrderId_ = addTestRawData("CL-" + std::to_string(number));
            order->orderId_ = IdT(number, 20261001);
            order->status_ = NEW_ORDSTATUS;
            dispatcher_->save(*order);
        }
    }

    {
        // restart: fresh storage, dispatcher and id counter over the same directory
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);
        ASSERT_EQ(9u, storageAfter.maxOrderNumber());

        IdTValueGenerator generator; // starts at 1, as in every run
        generator.advancePast(storageAfter.maxOrderNumber());
        std::unique_ptr<OrderEntry> fresh(createTestOrder());
        fresh->clOrderId_ = addTestRawData("CL-NEW");
        fresh->orderId_ = IdT();
        OrderEntry *saved = storageAfter.save(*fresh, &generator);
        ASSERT_NE(nullptr, saved);
        EXPECT_EQ(10u, saved->orderId_.id_);
        EXPECT_EQ(saved, storageAfter.locateByOrderNumber(10));
        EXPECT_NE(nullptr, storageAfter.locateByOrderNumber(9));
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// =============================================================================
// Id Limit (#81)
// =============================================================================

namespace
{
/// The id limit record's key, as StorageRecordDispatcher.cpp defines it
const IdT ID_LIMIT_KEY(0, 1);

/// How a run continues its ids, as app/main.cpp does: past the largest order restored (#58) and past the saved limit
u64 lastIssuedId(const OrderDataStorage &storage, const StorageRecordDispatcher &dispatcher)
{
    const u64 limit = dispatcher.restoredIdLimit();
    return std::max(storage.maxOrderNumber(), (0 < limit) ? limit - 1 : 0);
}

/// Only the newest version of the id limit record is on disk
void expectOneIdLimitVersion(const LMDBStorage &lmdb)
{
    const u32 top = lmdb.getTopVersion(ID_LIMIT_KEY);
    EXPECT_TRUE(lmdb.isExists(ID_LIMIT_KEY, top));
    for (u32 version = 0; version < top; ++version)
    {
        EXPECT_FALSE(lmdb.isExists(ID_LIMIT_KEY, version)) << "version " << version << " was not erased";
    }
}
} // namespace

TEST_F(StorageRecordDispatcherTest, IdLimitIsOneRecordAndSurvivesARestart)
{
    const std::string dir = test::uniqueTestPath("dispatcher-id-limit");
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        EXPECT_EQ(0u, dispatcher_->restoredIdLimit()) << "a new directory has no limit";
        dispatcher_->saveIdLimit(100);
        dispatcher_->saveIdLimit(250);
        expectOneIdLimitVersion(lmdb);
    }

    {
        // restart: a fresh dispatcher over the same directory
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);
        EXPECT_EQ(250u, dispatcherAfter.restoredIdLimit());
        dispatcherAfter.saveIdLimit(300);
        expectOneIdLimitVersion(lmdbAfter);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, IdLimitKeepsTheLargerOfTheVersionsACrashLeft)
{
    // A crash between writing a new version and erasing the old leaves both on disk. The load keeps the larger limit,
    // and the next save erases both.
    const std::string dir = test::uniqueTestPath("dispatcher-id-limit-crash");
    {
        LMDBStorage lmdb;
        StorageRecordDispatcher writer;
        writer.init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, &writer);
        const u64 limits[] = { 900, 400 };
        for (u64 limit : limits)
        {
            std::string record = createRecordTypePrefix(StorageRecordDispatcher::IDLIMIT_RECORDTYPE);
            record.append(reinterpret_cast<const char *>(&limit), sizeof(limit));
            lmdb.update(ID_LIMIT_KEY, record.data(), record.size());
        }
    }

    {
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);
        EXPECT_EQ(900u, dispatcherAfter.restoredIdLimit());
        dispatcherAfter.saveIdLimit(1000);
        expectOneIdLimitVersion(lmdbAfter);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, ExecutionIdsContinueAfterACrash)
{
    // The bug this covers: execution reports aren't persisted, and after a restart ids continued only past the largest
    // order number, so a new report could repeat an execution id a client already had (#81)
    const std::string dir = test::uniqueTestPath("dispatcher-execution-ids");
    u64 lastExecId = 0;
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        IdTValueGenerator generator;
        generator.reserve(dispatcher_.get(), 1000);

        std::unique_ptr<OrderEntry> order(createTestOrder());
        order->clOrderId_ = addTestRawData("CL-81");
        order->orderId_ = IdT();
        order->status_ = NEW_ORDSTATUS;
        OrderEntry *saved = orderStorage_->save(*order, &generator);
        ASSERT_NE(nullptr, saved);
        // the states Processor gives a stored order before it's persisted: New, and no cancel or replace (#150)
        saved->stateMachinePersistance_.stateZone1Id_ = 2;
        saved->stateMachinePersistance_.stateZone2Id_ = 8;
        dispatcher_->save(*saved); // persisted, as PersistOrderTrOperation does
        for (int i = 0; i < 5; ++i)
        {
            // its fills' reports, which aren't persisted
            ExecutionEntry exec;
            exec.orderId_ = saved->orderId_;
            exec.type_ = TRADE_EXECTYPE;
            lastExecId = orderStorage_->save(exec, &generator)->execId_.id_;
        }
        // stops without releasing the reservation, as a crash does
    }

    {
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);
        ASSERT_LT(storageAfter.maxOrderNumber(), lastExecId) << "the order number alone would repeat execution ids";

        IdTValueGenerator generator; // starts at 1, as in every run
        generator.advancePast(lastIssuedId(storageAfter, dispatcherAfter));
        EXPECT_GT(generator.getId().id_, lastExecId);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(StorageRecordDispatcherTest, IdsContinueWithoutAGapAfterACleanStop)
{
    const std::string dir = test::uniqueTestPath("dispatcher-clean-stop-ids");
    u64 lastId = 0;
    {
        LMDBStorage lmdb;
        dispatcher_->init(restore_.get(), orderBook_.get(), &lmdb, orderStorage_.get());
        lmdb.load(dir, dispatcher_.get());
        IdTValueGenerator generator;
        generator.reserve(dispatcher_.get(), 1000);
        for (int i = 0; i < 7; ++i)
        {
            lastId = generator.getId().id_;
        }
        generator.releaseReservation();
    }

    {
        OrderDataStorage storageAfter;
        TestOrderBook bookAfter;
        StorageRecordDispatcher dispatcherAfter;
        LMDBStorage lmdbAfter;
        dispatcherAfter.init(restore_.get(), &bookAfter, &lmdbAfter, &storageAfter);
        lmdbAfter.load(dir, &dispatcherAfter);

        IdTValueGenerator generator;
        generator.advancePast(lastIssuedId(storageAfter, dispatcherAfter));
        EXPECT_EQ(lastId + 1, generator.getId().id_);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// =============================================================================
// Reference-Data-Only Load (#50)
// =============================================================================

TEST_F(StorageRecordDispatcherTest, OrderRecordsAreSkippedWithoutOrderStorage)
{
    // seedData loads for reference data only and passes no OrderDataStorage. Order records were buffered anyway and
    // finishLoad() dereferenced the missing storage, so seedData crashed on any directory that held orders (#50).
    std::unique_ptr<OrderEntry> order(createTestOrder());
    order->orderId_ = IdT(4444, 6789);
    std::string orderRecord = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT orderId;
    u32 orderVersion = 0;
    OrderCodec::encode(*order, &orderRecord, &orderId, &orderVersion);

    InstrumentEntry instrument;
    WideDataStorage::instance()->get(order->instrument_.getId(), &instrument);
    std::string instrumentRecord = createRecordTypePrefix(StorageRecordDispatcher::INSTRUMENT_RECORDTYPE);
    IdT instrumentId;
    u32 instrumentVersion = 0;
    InstrumentCodec::encode(instrument, &instrumentRecord, &instrumentId, &instrumentVersion);

    dispatcher_->init(restore_.get(), nullptr, saver_.get(), nullptr);
    dispatcher_->startLoad();
    dispatcher_->onRecordLoaded(orderId, orderVersion, orderRecord.data(), orderRecord.size());
    dispatcher_->onRecordLoaded(instrumentId, instrumentVersion, instrumentRecord.data(), instrumentRecord.size());
    EXPECT_NO_THROW(dispatcher_->finishLoad());

    // the reference data still loads
    ASSERT_EQ(1u, restore_->instruments_.size());
    EXPECT_EQ(instrument.symbol_, restore_->instruments_.at(0)->symbol_);
}

// =============================================================================
// Golden records (#149)
// =============================================================================
//
// Records as the dispatcher and its codecs wrote them before #149 changed anything: the 4-byte record type, then the
// codec's fields, in x86-64 byte order (little-endian, an 8-byte size_t), as the format has always been. The tests above
// encode and decode with the same build, so a layout change passes them and then misreads, or fails to load, every data
// directory already written. These keep today's bytes: the build must still write them, and still read every field
// back from them.
//
// To change a format on purpose, give the codec a new FORMAT_VERSION, keep reading the old one, and add golden records
// for the new one beside these. Run the suite with GOLDEN_PRINT=1 to print what the build writes.

namespace
{

std::string toHex(const std::string &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(2 * bytes.size());
    for (const char c : bytes)
    {
        const unsigned char b = static_cast<unsigned char>(c);
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 0xf]);
    }
    return hex;
}

std::string fromHex(const std::string &hex)
{
    std::string bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

/// Expects the bytes written to be the golden record, and prints them as a C++ literal when GOLDEN_PRINT is set
void expectGolden(const char *name, const std::string &written, const char *golden)
{
    if (nullptr != std::getenv("GOLDEN_PRINT"))
    {
        const std::string hex = toHex(written);
        std::cout << "const char *const " << name << " =";
        for (size_t i = 0; i < hex.size(); i += 64)
        {
            std::cout << "\n    \"" << hex.substr(i, 64) << "\"";
        }
        std::cout << ";\n";
    }
    EXPECT_EQ(std::string(golden), toHex(written)) << name << ": the bytes written are not the golden record";
}

// The entries the golden records hold. Every field has a value of its own, so a field read from another's place shows,
// and the strings hold '.', the codecs' field separator.
const IdT GOLDEN_INSTRUMENT_ID(101, 20261013);
const IdT GOLDEN_STRING_ID(102, 20261013);
const IdT GOLDEN_ACCOUNT_ID(103, 20261013);
const IdT GOLDEN_CLEARING_ID(104, 20261013);
const IdT GOLDEN_RAWDATA_ID(105, 20261013);
const IdT GOLDEN_ORDER_ID(4444, 20261013);
const u64 GOLDEN_ID_LIMIT = 1234567890123ULL;
const StringT GOLDEN_STRING = "NASDAQ.US";
const char GOLDEN_RAWDATA[] = "WS-1791900000000.7";

InstrumentEntry goldenInstrument()
{
    InstrumentEntry val;
    val.symbol_ = "EUR/USD.SP";
    val.securityId_ = "GB00B03MLX29";
    val.securityIdSource_ = "ISIN";
    val.id_ = GOLDEN_INSTRUMENT_ID;
    return val;
}

AccountEntry goldenAccount()
{
    AccountEntry val;
    val.account_ = "TRADING-1.A";
    val.firm_ = "HOUSE-CLEARING";
    val.type_ = AGENCY_ACCOUNTTYPE;
    val.id_ = GOLDEN_ACCOUNT_ID;
    return val;
}

ClearingEntry goldenClearing()
{
    ClearingEntry val;
    val.firm_ = "HOUSE.CLEARING";
    val.id_ = GOLDEN_CLEARING_ID;
    return val;
}

RawDataEntry goldenRawData()
{
    RawDataEntry val(STRING_RAWDATATYPE, GOLDEN_RAWDATA, static_cast<u32>(std::strlen(GOLDEN_RAWDATA)));
    val.id_ = GOLDEN_RAWDATA_ID;
    return val;
}

/// An order loads its instrument when it's constructed, so the golden order's must be in WideDataStorage first
void restoreGoldenInstrument()
{
    WideDataStorage::instance()->restore(new InstrumentEntry(goldenInstrument()));
}

/// The golden order's ClOrdID, its original's, its source and its destination, under the ids its fields name, where the
/// dispatcher looks them up to carry them in the order's record (#192)
void restoreGoldenOrderStrings(DataStorageRestore &store)
{
    auto *clOrderId = new RawDataEntry(STRING_RAWDATATYPE, "GOLDEN-CL-1", 11);
    clOrderId->id_ = SourceIdT(13, 3);
    store.restore(clOrderId);
    auto *origClOrderId = new RawDataEntry(STRING_RAWDATATYPE, "GOLDEN-CL-0", 11);
    origClOrderId->id_ = SourceIdT(14, 4);
    store.restore(origClOrderId);
    store.restore(SourceIdT(11, 1), new StringT("CLIENT-1"));
    store.restore(SourceIdT(12, 2), new StringT("Internal"));
}

std::unique_ptr<OrderEntry> goldenOrder()
{
    // source, destination, clOrderId, origClOrderId, instrument, account, clearing, executions
    auto val = std::make_unique<OrderEntry>(SourceIdT(11, 1), SourceIdT(12, 2), SourceIdT(13, 3), SourceIdT(14, 4),
                                            GOLDEN_INSTRUMENT_ID, SourceIdT(16, 6), SourceIdT(17, 7), SourceIdT(18, 8));
    val->orderId_ = GOLDEN_ORDER_ID;
    val->origOrderId_ = IdT(4443, 20261012);
    val->creationTime_ = 1791900000001ULL;
    val->lastUpdateTime_ = 1791900000002ULL;
    val->expireTime_ = 1791900000003ULL;
    val->settlDate_ = 1791900000004ULL;
    val->price_ = 101.25;
    val->stopPx_ = 100.5;
    val->avgPx_ = 101.125;
    val->dayAvgPx_ = 101.0625;
    val->status_ = PARTFILL_ORDSTATUS;
    val->side_ = SELL_SIDE;
    val->ordType_ = STOPLIMIT_ORDERTYPE;
    val->tif_ = GTC_TIF;
    val->settlType_ = _3_SETTLTYPE;
    val->capacity_ = PRINCIPAL_CAPACITY;
    val->currency_ = EUR_CURRENCY;
    val->minQty_ = 5;
    val->orderQty_ = 1000;
    val->leavesQty_ = 600;
    val->cumQty_ = 400;
    val->dayOrderQty_ = 1001;
    val->dayCumQty_ = 401;
    val->stateMachinePersistance_.orderData_ = val.get();
    val->stateMachinePersistance_.stateZone1Id_ = 3;
    val->stateMachinePersistance_.stateZone2Id_ = 2;
    val->execInstruct_ = { STAY_ON_OFFER_SIDE_INSTRUCTION, NOT_HELD_INSTRUCTION };
    val->stopTriggered_ = true; // a STOPLIMIT its stop released (#223)
    return val;
}

// Written by the codecs as of ea18de7, before #149 changed them
const char *const GOLDEN_INSTRUMENT_RECORD = "010000000a000000000000004555522f5553442e53502e0c0000000000000047"
                                             "4230304230334d4c5832392e04000000000000004953494e";
const char *const GOLDEN_STRING_RECORD = "0200000009000000000000004e41534441512e5553";
const char *const GOLDEN_ACCOUNT_RECORD = "030000000b0000000000000054524144494e472d312e412e0e00000000000000"
                                          "484f5553452d434c454152494e472e02000000";
const char *const GOLDEN_CLEARING_RECORD = "040000000e00000000000000484f5553452e434c454152494e47";
const char *const GOLDEN_RAWDATA_RECORD = "05000000010000002e120000002e57532d313739313930303030303030302e37";
const char *const GOLDEN_ORDER_RECORD_V0 = "06000000952835013a65000000000000002e060000003a10000000000000002e"
                                           "070000003a11000000000000002e020000003a0c000000000000002e03000000"
                                           "3a0d000000000000002e040000003a0e000000000000002e010000003a0b0000"
                                           "00000000002e080000003a12000000000000002e942835013a5b110000000000"
                                           "002e011f9035a10100002e021f9035a10100002e031f9035a10100002e041f90"
                                           "35a10100002e00000000005059402e00000000002059402e0000000000485940"
                                           "2e00000000004459402e060000002e020000002e040000002e030000002e0400"
                                           "00002e020000002e020000002e050000002ee80300002e580200002e90010000"
                                           "2ee90300002e910100002e030000003a02000000";
// The order as OrderCodec has written it since #172, in format version 1: version 0's fields, then '.' and the mask of
// its execution instructions
const char *const GOLDEN_ORDER_RECORD_V1 = "06000100952835013a65000000000000002e060000003a10000000000000002e"
                                           "070000003a11000000000000002e020000003a0c000000000000002e03000000"
                                           "3a0d000000000000002e040000003a0e000000000000002e010000003a0b0000"
                                           "00000000002e080000003a12000000000000002e942835013a5b110000000000"
                                           "002e011f9035a10100002e021f9035a10100002e031f9035a10100002e041f90"
                                           "35a10100002e00000000005059402e00000000002059402e0000000000485940"
                                           "2e00000000004459402e060000002e020000002e040000002e030000002e0400"
                                           "00002e020000002e020000002e050000002ee80300002e580200002e90010000"
                                           "2ee90300002e910100002e030000003a020000002e06000000";
// The order as OrderCodec has written it since #223, in format version 2: version 1's fields, then '.' and whether a
// stop order's stop was reached, a byte
const char *const GOLDEN_ORDER_RECORD_V2 = "06000200952835013a65000000000000002e060000003a10000000000000002e"
                                           "070000003a11000000000000002e020000003a0c000000000000002e03000000"
                                           "3a0d000000000000002e040000003a0e000000000000002e010000003a0b0000"
                                           "00000000002e080000003a12000000000000002e942835013a5b110000000000"
                                           "002e011f9035a10100002e021f9035a10100002e031f9035a10100002e041f90"
                                           "35a10100002e00000000005059402e00000000002059402e0000000000485940"
                                           "2e00000000004459402e060000002e020000002e040000002e030000002e0400"
                                           "00002e020000002e020000002e050000002ee80300002e580200002e90010000"
                                           "2ee90300002e910100002e030000003a020000002e060000002e01";
// The order as OrderCodec has written it since #192, in format version 3: version 2's fields, then the order's strings,
// each '.', a 1 for carried, and its value: the ClOrdID "GOLDEN-CL-1" and its original's "GOLDEN-CL-0" (type 1, a
// string; length; bytes), the source "CLIENT-1" and the destination "Internal" (length; bytes)
const char *const GOLDEN_ORDER_RECORD_V3 = "06000300952835013a65000000000000002e060000003a10000000000000002e"
                                           "070000003a11000000000000002e020000003a0c000000000000002e03000000"
                                           "3a0d000000000000002e040000003a0e000000000000002e010000003a0b0000"
                                           "00000000002e080000003a12000000000000002e942835013a5b110000000000"
                                           "002e011f9035a10100002e021f9035a10100002e031f9035a10100002e041f90"
                                           "35a10100002e00000000005059402e00000000002059402e0000000000485940"
                                           "2e00000000004459402e060000002e020000002e040000002e030000002e0400"
                                           "00002e020000002e020000002e050000002ee80300002e580200002e90010000"
                                           "2ee90300002e910100002e030000003a020000002e060000002e012e01010000"
                                           "000b000000474f4c44454e2d434c2d312e01010000000b000000474f4c44454e"
                                           "2d434c2d302e0108000000434c49454e542d312e0108000000496e7465726e61"
                                           "6c";
const char *const GOLDEN_ID_LIMIT_RECORD = "09000000cb04fb711f010000";
/// The golden instrument's last trade (#237): 101.25, made when id 4242 was drawn, keyed (101, 2)
const IdT GOLDEN_TRADE_ID(4242, 1792000000);
const IdT GOLDEN_LAST_TRADE_KEY(101, 2);
const char *const GOLDEN_LAST_TRADE_RECORD = "0a0000006500000000000000952835010000000000505940921000000000000000c0cf6a";

} // namespace

TEST_F(StorageRecordDispatcherTest, WritesTheGoldenRecords)
{
    restoreGoldenInstrument();
    restoreGoldenOrderStrings(*restore_);
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    dispatcher_->save(goldenInstrument());
    dispatcher_->save(GOLDEN_STRING_ID, GOLDEN_STRING);
    dispatcher_->save(goldenAccount());
    dispatcher_->save(goldenClearing());
    dispatcher_->save(goldenRawData());
    const std::unique_ptr<OrderEntry> order = goldenOrder();
    dispatcher_->save(*order);
    dispatcher_->saveIdLimit(GOLDEN_ID_LIMIT);
    dispatcher_->saveLastTrade(GOLDEN_INSTRUMENT_ID, 101.25, GOLDEN_TRADE_ID);

    const auto written = [this](const IdT &id) -> std::string
    {
        const auto it = saver_->records_.find(id);
        return (saver_->records_.end() == it) ? std::string() : it->second.record_;
    };
    expectGolden("GOLDEN_INSTRUMENT_RECORD", written(GOLDEN_INSTRUMENT_ID), GOLDEN_INSTRUMENT_RECORD);
    expectGolden("GOLDEN_STRING_RECORD", written(GOLDEN_STRING_ID), GOLDEN_STRING_RECORD);
    expectGolden("GOLDEN_ACCOUNT_RECORD", written(GOLDEN_ACCOUNT_ID), GOLDEN_ACCOUNT_RECORD);
    expectGolden("GOLDEN_CLEARING_RECORD", written(GOLDEN_CLEARING_ID), GOLDEN_CLEARING_RECORD);
    expectGolden("GOLDEN_RAWDATA_RECORD", written(GOLDEN_RAWDATA_ID), GOLDEN_RAWDATA_RECORD);
    expectGolden("GOLDEN_ORDER_RECORD_V3", written(GOLDEN_ORDER_ID), GOLDEN_ORDER_RECORD_V3);
    expectGolden("GOLDEN_ID_LIMIT_RECORD", written(IdT(0, 1)), GOLDEN_ID_LIMIT_RECORD);
    expectGolden("GOLDEN_LAST_TRADE_RECORD", written(GOLDEN_LAST_TRADE_KEY), GOLDEN_LAST_TRADE_RECORD);
}

namespace
{
/// Decodes a golden order record in the version its record word names, and expects every field of the golden order,
/// with the execution instructions given: a version 0 record has none
void expectEveryFieldOfTheGoldenOrder(const char *golden, u32 formatVersion, const InstructionSetT &instructions,
                                      bool stopTriggered)
{
    SCOPED_TRACE("order record in format version " + std::to_string(formatVersion));
    const std::string record = fromHex(golden);
    ASSERT_LT(sizeof(u32), record.size());
    u32 word = 0;
    std::memcpy(&word, record.data(), sizeof(word));
    EXPECT_EQ(static_cast<u32>(StorageRecordDispatcher::ORDER_RECORDTYPE), word & 0xffff);
    EXPECT_EQ(formatVersion, word >> 16);
    const std::unique_ptr<OrderEntry> expected = goldenOrder();
    OrderStrings strings;
    const std::unique_ptr<OrderEntry> order(OrderCodec::decode(
        GOLDEN_ORDER_ID, word >> 16, record.data() + sizeof(word), record.size() - sizeof(word), &strings));
    ASSERT_NE(nullptr, order);
    EXPECT_EQ(GOLDEN_ORDER_ID, order->orderId_);
    EXPECT_EQ(expected->source_.getId(), order->source_.getId());
    EXPECT_EQ(expected->destination_.getId(), order->destination_.getId());
    EXPECT_EQ(expected->clOrderId_.getId(), order->clOrderId_.getId());
    EXPECT_EQ(expected->origClOrderId_.getId(), order->origClOrderId_.getId());
    EXPECT_EQ(expected->instrument_.getId(), order->instrument_.getId());
    EXPECT_EQ(expected->account_.getId(), order->account_.getId());
    EXPECT_EQ(expected->clearing_.getId(), order->clearing_.getId());
    EXPECT_EQ(expected->executions_.getId(), order->executions_.getId());
    EXPECT_EQ(expected->origOrderId_, order->origOrderId_);
    EXPECT_EQ(expected->creationTime_, order->creationTime_);
    EXPECT_EQ(expected->lastUpdateTime_, order->lastUpdateTime_);
    EXPECT_EQ(expected->expireTime_, order->expireTime_);
    EXPECT_EQ(expected->settlDate_, order->settlDate_);
    EXPECT_EQ(expected->price_, order->price_);
    EXPECT_EQ(expected->stopPx_, order->stopPx_);
    EXPECT_EQ(expected->avgPx_, order->avgPx_);
    EXPECT_EQ(expected->dayAvgPx_, order->dayAvgPx_);
    EXPECT_EQ(expected->status_, order->status_);
    EXPECT_EQ(expected->side_, order->side_);
    EXPECT_EQ(expected->ordType_, order->ordType_);
    EXPECT_EQ(expected->tif_, order->tif_);
    EXPECT_EQ(expected->settlType_, order->settlType_);
    EXPECT_EQ(expected->capacity_, order->capacity_);
    EXPECT_EQ(expected->currency_, order->currency_);
    EXPECT_EQ(expected->minQty_, order->minQty_);
    EXPECT_EQ(expected->orderQty_, order->orderQty_);
    EXPECT_EQ(expected->leavesQty_, order->leavesQty_);
    EXPECT_EQ(expected->cumQty_, order->cumQty_);
    EXPECT_EQ(expected->dayOrderQty_, order->dayOrderQty_);
    EXPECT_EQ(expected->dayCumQty_, order->dayCumQty_);
    EXPECT_EQ(expected->stateMachinePersistance_.stateZone1Id_, order->stateMachinePersistance_.stateZone1Id_);
    EXPECT_EQ(expected->stateMachinePersistance_.stateZone2Id_, order->stateMachinePersistance_.stateZone2Id_);
    EXPECT_EQ(instructions, order->execInstruct_);
    // Format version 2 says whether the stop was reached; an older record's stop order is held (#223)
    EXPECT_EQ(stopTriggered, order->stopTriggered_) << "format version " << formatVersion;
    // Format version 3 carries the order's strings, under its fields' ids (#192); an older record carries none
    if (3 <= formatVersion)
    {
        ASSERT_TRUE(strings.clOrderId && strings.origClOrderId && strings.source && strings.destination);
        EXPECT_EQ("GOLDEN-CL-1", std::string(strings.clOrderId->data_, strings.clOrderId->length_));
        EXPECT_EQ(STRING_RAWDATATYPE, strings.clOrderId->type_);
        EXPECT_EQ(expected->clOrderId_.getId(), strings.clOrderId->id_);
        EXPECT_EQ("GOLDEN-CL-0", std::string(strings.origClOrderId->data_, strings.origClOrderId->length_));
        EXPECT_EQ(expected->origClOrderId_.getId(), strings.origClOrderId->id_);
        EXPECT_EQ("CLIENT-1", *strings.source);
        EXPECT_EQ("Internal", *strings.destination);
    }
    else
    {
        EXPECT_FALSE(strings.clOrderId || strings.origClOrderId || strings.source || strings.destination);
    }
}

/// The golden order records, each with the format version it was written in
const std::vector<std::pair<const char *, u32>> &goldenOrderRecords()
{
    static const std::vector<std::pair<const char *, u32>> records = { { GOLDEN_ORDER_RECORD_V0, 0 },
                                                                       { GOLDEN_ORDER_RECORD_V1, 1 },
                                                                       { GOLDEN_ORDER_RECORD_V2, 2 },
                                                                       { GOLDEN_ORDER_RECORD_V3, 3 } };
    return records;
}
} // namespace

TEST_F(StorageRecordDispatcherTest, AnOrderRecordRestoresTheStringsItCarriesBeforeItsOrder)
{
    // Since format version 3 an order's record carries its ClOrdID, its original's, its source and its destination, and
    // nothing writes records of their own for them (#192). Restored, they come back under the ids the order's fields
    // name, before the order is stored: storing it indexes its ClOrdID.
    restoreGoldenInstrument();
    OrderDataStorage storage;
    TestFileSaver saver;
    StorageRecordDispatcher dispatcher;
    dispatcher.init(WideDataStorage::instance(), nullptr, &saver, &storage);
    dispatcher.startLoad();
    const std::string record = fromHex(GOLDEN_ORDER_RECORD_V3);
    dispatcher.onRecordLoaded(GOLDEN_ORDER_ID, 0, record.data(), record.size());
    dispatcher.finishLoad();

    const RawDataEntry clOrdId(STRING_RAWDATATYPE, "GOLDEN-CL-1", 11);
    const OrderEntry *order = storage.locateByClOrderId(clOrdId);
    ASSERT_NE(nullptr, order) << "the order isn't found by the ClOrdID its record carries";
    EXPECT_EQ(GOLDEN_ORDER_ID, order->orderId_);
    EXPECT_EQ("CLIENT-1", order->source_.get());
    EXPECT_EQ("Internal", order->destination_.get());
    EXPECT_EQ("GOLDEN-CL-0", std::string(order->origClOrderId_.get().data_, order->origClOrderId_.get().length_));
    EXPECT_TRUE(saver.records_.empty()) << "restoring an order wrote a record";
}

TEST_F(StorageRecordDispatcherTest, AnOrdersRecordCarriesTheStringsItsSessionAddedAndRestoresThemUnderTheirIds)
{
    // As a WebSocket session stores a new order's strings (#192): a ClOrdID, a source and a destination, none of them
    // saved. A string's id has date 0, which IdT::isValid() refuses, and the record must carry them anyway: a server
    // that left the source and destination out restarted with orders that named strings nobody had.
    restoreGoldenInstrument();
    WideParamsDataStorage *wide = WideDataStorage::instance();
    const SourceIdT clOrdId = wide->addUnsaved(new RawDataEntry(STRING_RAWDATATYPE, "WS-1-1", 6));
    const SourceIdT source = wide->addUnsaved(new StringT("desk-a"));
    const SourceIdT destination = wide->addUnsaved(new StringT("Internal"));
    ASSERT_FALSE(source.isValid()) << "a string's id has date 0, which this test is about";
    OrderEntry order(source, destination, clOrdId, SourceIdT(), GOLDEN_INSTRUMENT_ID, SourceIdT(16, 6),
                     SourceIdT(17, 7), SourceIdT(18, 8));
    order.orderId_ = IdT(192, 20261015);
    order.status_ = NEW_ORDSTATUS;
    order.side_ = BUY_SIDE;
    order.ordType_ = LIMIT_ORDERTYPE;
    order.price_ = 10.0;
    order.orderQty_ = 5;
    order.leavesQty_ = 5;
    order.stateMachinePersistance_.stateZone1Id_ = 2;
    order.stateMachinePersistance_.stateZone2Id_ = 8;

    TestFileSaver saver;
    OrderDataStorage storage;
    StorageRecordDispatcher writer;
    writer.init(wide, nullptr, &saver, &storage);
    writer.save(order);
    const auto written = saver.records_.find(order.orderId_);
    ASSERT_NE(saver.records_.end(), written);

    // Restored by a dispatcher whose storage only records what it is given
    TestDataStorageRestore restored;
    OrderDataStorage restoredOrders;
    TestFileSaver restoredSaver;
    StorageRecordDispatcher reader;
    reader.init(&restored, nullptr, &restoredSaver, &restoredOrders);
    reader.startLoad();
    reader.onRecordLoaded(order.orderId_, 0, written->second.record_.data(), written->second.record_.size());
    reader.finishLoad();

    ASSERT_EQ(1u, restored.rawDatas_.size()) << "the ClOrdID wasn't carried";
    EXPECT_EQ(clOrdId, restored.rawDatas_[0]->id_);
    EXPECT_EQ("WS-1-1", std::string(restored.rawDatas_[0]->data_, restored.rawDatas_[0]->length_));
    std::map<IdT, std::string> strings;
    for (const auto &str : restored.strings_)
    {
        strings[str.id_] = *str.str_;
    }
    EXPECT_EQ((std::map<IdT, std::string>{ { source, "desk-a" }, { destination, "Internal" } }), strings)
        << "the source or destination wasn't carried under its own id";
}

TEST_F(StorageRecordDispatcherTest, ARecordCutInsideACarriedStringIsRefused)
{
    // The golden version 3 record ends with its destination, "Internal". Cut 3 bytes short, from a buffer of its own
    // length, the decoder must say the destination is short before it reads it: reading its length's worth went past
    // the record (#192).
    restoreGoldenInstrument();
    const std::string golden = fromHex(GOLDEN_ORDER_RECORD_V3);
    const std::vector<char> cut(golden.begin() + sizeof(u32), golden.end() - 3);
    try
    {
        std::unique_ptr<OrderEntry>(OrderCodec::decode(GOLDEN_ORDER_ID, 3, cut.data(), cut.size()));
        ADD_FAILURE() << "a record cut inside its destination was decoded";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_NE(std::string::npos, std::string(ex.what()).find("unable decode destination_")) << ex.what();
    }
}

TEST_F(StorageRecordDispatcherTest, ReadsEveryFieldOfTheGoldenRecords)
{
    restoreGoldenInstrument();
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();
    const auto load = [this](const IdT &id, const char *golden)
    {
        const std::string record = fromHex(golden);
        dispatcher_->onRecordLoaded(id, 0, record.data(), record.size());
    };
    load(GOLDEN_INSTRUMENT_ID, GOLDEN_INSTRUMENT_RECORD);
    load(GOLDEN_STRING_ID, GOLDEN_STRING_RECORD);
    load(GOLDEN_ACCOUNT_ID, GOLDEN_ACCOUNT_RECORD);
    load(GOLDEN_CLEARING_ID, GOLDEN_CLEARING_RECORD);
    load(GOLDEN_RAWDATA_ID, GOLDEN_RAWDATA_RECORD);
    load(IdT(0, 1), GOLDEN_ID_LIMIT_RECORD);
    dispatcher_->finishLoad();

    const InstrumentEntry instrument = goldenInstrument();
    ASSERT_EQ(1u, restore_->instruments_.size());
    EXPECT_EQ(instrument.symbol_, restore_->instruments_.at(0)->symbol_);
    EXPECT_EQ(instrument.securityId_, restore_->instruments_.at(0)->securityId_);
    EXPECT_EQ(instrument.securityIdSource_, restore_->instruments_.at(0)->securityIdSource_);
    EXPECT_EQ(instrument.id_, restore_->instruments_.at(0)->id_);

    ASSERT_EQ(1u, restore_->strings_.size());
    EXPECT_EQ(GOLDEN_STRING, *restore_->strings_.at(0).str_);
    EXPECT_EQ(GOLDEN_STRING_ID, restore_->strings_.at(0).id_);

    const AccountEntry account = goldenAccount();
    ASSERT_EQ(1u, restore_->accounts_.size());
    EXPECT_EQ(account.account_, restore_->accounts_.at(0)->account_);
    EXPECT_EQ(account.firm_, restore_->accounts_.at(0)->firm_);
    EXPECT_EQ(account.type_, restore_->accounts_.at(0)->type_);
    EXPECT_EQ(account.id_, restore_->accounts_.at(0)->id_);

    ASSERT_EQ(1u, restore_->clearings_.size());
    EXPECT_EQ(goldenClearing().firm_, restore_->clearings_.at(0)->firm_);
    EXPECT_EQ(GOLDEN_CLEARING_ID, restore_->clearings_.at(0)->id_);

    ASSERT_EQ(1u, restore_->rawDatas_.size());
    const RawDataEntry &raw = *restore_->rawDatas_.at(0);
    EXPECT_EQ(STRING_RAWDATATYPE, raw.type_);
    ASSERT_EQ(std::strlen(GOLDEN_RAWDATA), raw.length_);
    EXPECT_EQ(std::string(GOLDEN_RAWDATA), std::string(raw.data_, raw.length_));
    EXPECT_EQ(GOLDEN_RAWDATA_ID, raw.id_);

    EXPECT_EQ(GOLDEN_ID_LIMIT, dispatcher_->restoredIdLimit());

    // The order's codec, by itself: restoring an order looks its ClOrdID up in WideDataStorage, which the golden
    // record's ids don't name. Every version this build has written is still read (#172).
    expectEveryFieldOfTheGoldenOrder(GOLDEN_ORDER_RECORD_V3, 3, goldenOrder()->execInstruct_, true);
    expectEveryFieldOfTheGoldenOrder(GOLDEN_ORDER_RECORD_V2, 2, goldenOrder()->execInstruct_, true);
    expectEveryFieldOfTheGoldenOrder(GOLDEN_ORDER_RECORD_V1, 1, goldenOrder()->execInstruct_, false);
    expectEveryFieldOfTheGoldenOrder(GOLDEN_ORDER_RECORD_V0, 0, InstructionSetT(), false);
}

namespace
{
/// A copy of bytes in a heap block of exactly their size, so that ASan reports any read past the end
std::unique_ptr<char[]> exactCopy(const std::string &bytes)
{
    std::unique_ptr<char[]> copy(new char[bytes.size()]);
    std::memcpy(copy.get(), bytes.data(), bytes.size());
    return copy;
}

/// The golden records the dispatcher decodes when it loads them; an order's record is decoded at finishLoad()
const std::vector<std::pair<IdT, const char *>> &goldenLoadedRecords()
{
    static const std::vector<std::pair<IdT, const char *>> records = {
        { GOLDEN_INSTRUMENT_ID, GOLDEN_INSTRUMENT_RECORD }, { GOLDEN_STRING_ID, GOLDEN_STRING_RECORD },
        { GOLDEN_ACCOUNT_ID, GOLDEN_ACCOUNT_RECORD },       { GOLDEN_CLEARING_ID, GOLDEN_CLEARING_RECORD },
        { GOLDEN_RAWDATA_ID, GOLDEN_RAWDATA_RECORD },       { IdT(0, 1), GOLDEN_ID_LIMIT_RECORD },
        { GOLDEN_LAST_TRADE_KEY, GOLDEN_LAST_TRADE_RECORD }
    };
    return records;
}
} // namespace

TEST_F(StorageRecordDispatcherTest, ALastTradeRecordLongerThanItsFieldsOrKeyedForAnotherInstrumentIsRefused)
{
    // The record names its instrument, and its key must be that instrument's (#237): a record under another key would be
    // pruned with the wrong record's versions (#148)
    restoreGoldenInstrument();
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    const std::string golden = fromHex(GOLDEN_LAST_TRADE_RECORD);
    EXPECT_NO_THROW(dispatcher_->onRecordLoaded(GOLDEN_LAST_TRADE_KEY, 0, golden.data(), golden.size()));
    const std::string longer = golden + std::string(1, '\0');
    EXPECT_THROW(dispatcher_->onRecordLoaded(GOLDEN_LAST_TRADE_KEY, 1, longer.data(), longer.size()),
                 std::runtime_error);
    EXPECT_THROW(dispatcher_->onRecordLoaded(IdT(102, 2), 1, golden.data(), golden.size()), std::runtime_error);
    EXPECT_THROW(dispatcher_->onRecordLoaded(GOLDEN_INSTRUMENT_ID, 1, golden.data(), golden.size()), std::runtime_error)
        << "under the instrument's own key";
}

TEST_F(StorageRecordDispatcherTest, EveryRecordCutShortIsRejected)
{
    // A decoder used to read the '.' after a field without checking that the record went on that far, and a string's
    // length was checked only by an assert (#149). Every prefix of every golden record must now be refused, and under
    // ASan none may be read past its end.
    restoreGoldenInstrument();
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    for (const auto &golden : goldenLoadedRecords())
    {
        const std::string record = fromHex(golden.second);
        for (size_t length = 0; length < record.size(); ++length)
        {
            const std::unique_ptr<char[]> cut = exactCopy(record.substr(0, length));
            EXPECT_THROW(dispatcher_->onRecordLoaded(golden.first, 0, cut.get(), length), std::runtime_error)
                << toHex(record.substr(0, 4)) << " record cut to " << length << " of " << record.size() << " bytes";
        }
    }

    for (const auto &golden : goldenOrderRecords())
    {
        const std::string order = fromHex(golden.first).substr(sizeof(u32));
        for (size_t length = 0; length < order.size(); ++length)
        {
            const std::unique_ptr<char[]> cut = exactCopy(order.substr(0, length));
            EXPECT_THROW(
                std::unique_ptr<OrderEntry>(OrderCodec::decode(GOLDEN_ORDER_ID, golden.second, cut.get(), length)),
                std::runtime_error)
                << "order record in format version " << golden.second << " cut to " << length << " of " << order.size()
                << " bytes";
        }
    }
}

TEST_F(StorageRecordDispatcherTest, EveryRecordWithAByteMoreIsRejected)
{
    // Bytes after the last field mean a layout this build doesn't know (#149). The extra byte is a '.', the codecs'
    // separator, the likeliest to pass for part of the record.
    restoreGoldenInstrument();
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    for (const auto &golden : goldenLoadedRecords())
    {
        const std::string record = fromHex(golden.second) + '.';
        const std::unique_ptr<char[]> longer = exactCopy(record);
        EXPECT_THROW(dispatcher_->onRecordLoaded(golden.first, 0, longer.get(), record.size()), std::runtime_error)
            << toHex(record.substr(0, 4)) << " record with a byte more";
    }

    for (const auto &golden : goldenOrderRecords())
    {
        const std::string order = fromHex(golden.first).substr(sizeof(u32)) + '.';
        const std::unique_ptr<char[]> longer = exactCopy(order);
        EXPECT_THROW(
            std::unique_ptr<OrderEntry>(OrderCodec::decode(GOLDEN_ORDER_ID, golden.second, longer.get(), order.size())),
            std::runtime_error)
            << "order record in format version " << golden.second << " with a byte more";
    }
}

TEST_F(StorageRecordDispatcherTest, ARecordInANewerFormatIsRejected)
{
    // A record's first 4 bytes are its type, and in the high 16 bits the version of the layout its codec wrote (#149).
    // A version this build doesn't know is refused by name, rather than misread as the layout it does know.
    restoreGoldenInstrument();
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    for (const auto &golden : goldenLoadedRecords())
    {
        std::string record = fromHex(golden.second);
        record[2] = 1; // format version 1
        try
        {
            dispatcher_->onRecordLoaded(golden.first, 0, record.data(), record.size());
            ADD_FAILURE() << toHex(record.substr(0, 4)) << " record in format version 1 was loaded";
        }
        catch (const std::runtime_error &ex)
        {
            EXPECT_NE(std::string::npos, std::string(ex.what()).find("format version 1"))
                << toHex(record.substr(0, 4)) << ": " << ex.what();
        }
    }
    // The order codec is at a version of its own (#172): one past it is refused the same way
    std::string order = fromHex(GOLDEN_ORDER_RECORD_V3);
    order[2] = static_cast<char>(OrderCodec::FORMAT_VERSION + 1);
    try
    {
        dispatcher_->onRecordLoaded(GOLDEN_ORDER_ID, 0, order.data(), order.size());
        ADD_FAILURE() << "an order record one format version past this build's was loaded";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_NE(std::string::npos,
                  std::string(ex.what()).find("format version " + std::to_string(OrderCodec::FORMAT_VERSION + 1)))
            << ex.what();
    }

    // A type no build has written is still refused as one
    std::string unknown = fromHex(GOLDEN_STRING_RECORD);
    unknown[0] = 77;
    try
    {
        dispatcher_->onRecordLoaded(GOLDEN_STRING_ID, 0, unknown.data(), unknown.size());
        ADD_FAILURE() << "a record of type 77 was loaded";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_NE(std::string::npos, std::string(ex.what()).find("Invalid record type")) << ex.what();
    }
}

// =============================================================================
// Orders that lost their fills (#207)
// =============================================================================

namespace
{
/// An open limit order on the instrument, as the transaction that booked it wrote it
std::unique_ptr<OrderEntry> bookedOrder(const SourceIdT &instrument, u64 number, Side side, PriceT price, QuantityT qty)
{
    std::unique_ptr<OrderEntry> order = test::createCorrectOrder(instrument);
    test::assignClOrderId(order.get());
    order->orderId_ = IdT(number, 20261014);
    order->status_ = NEW_ORDSTATUS;
    order->side_ = side;
    order->price_ = price;
    order->orderQty_ = qty;
    order->leavesQty_ = qty;
    // In New, with no cancel or replace pending: the ids #150 pins
    order->stateMachinePersistance_.orderData_ = order.get();
    order->stateMachinePersistance_.stateZone1Id_ = 2;
    order->stateMachinePersistance_.stateZone2Id_ = 8;
    return order;
}

void loadOrderRecord(StorageRecordDispatcher &dispatcher, const OrderEntry &order)
{
    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 version = 0;
    OrderCodec::encode(order, &buf, &id, &version);
    dispatcher.onRecordLoaded(id, version, buf.c_str(), buf.size());
}
} // namespace

TEST_F(StorageRecordDispatcherTest, RestoredOrdersThatCrossAnEarlierOneAreLeftToMatch)
{
    // Each restored order is checked against the orders that arrived before it (#207). X rests. Y crosses X. W crosses Y,
    // which waits to be matched: matched first, Y rests, and W then trades with it at Y's price, as it did. Booked now,
    // W would rest, and Y would trade with it at W's. Z crosses nothing. Only X and Z are booked.
    const SourceIdT instrument = test::addInstrument("UNCROSS");
    OrderBookImpl book;
    OrderBookImpl::InstrumentsT instruments;
    instruments.insert(instrument);
    book.init(instruments);
    dispatcher_->init(restore_.get(), &book, saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();
    std::unique_ptr<OrderEntry> x = bookedOrder(instrument, 11, SELL_SIDE, 100.0, 100);
    std::unique_ptr<OrderEntry> y = bookedOrder(instrument, 12, BUY_SIDE, 101.0, 200);
    std::unique_ptr<OrderEntry> w = bookedOrder(instrument, 13, SELL_SIDE, 100.5, 50);
    std::unique_ptr<OrderEntry> z = bookedOrder(instrument, 14, BUY_SIDE, 99.0, 10);
    // In any order: the dispatcher restores them in the order they arrived
    loadOrderRecord(*dispatcher_, *z);
    loadOrderRecord(*dispatcher_, *w);
    loadOrderRecord(*dispatcher_, *x);
    loadOrderRecord(*dispatcher_, *y);
    dispatcher_->finishLoad();

    EXPECT_EQ(x->orderId_, book.getTop(instrument, SELL_SIDE));
    EXPECT_EQ(z->orderId_, book.getTop(instrument, BUY_SIDE));
    ASSERT_EQ(2u, dispatcher_->ordersToMatch().size());
    EXPECT_EQ(y->orderId_, dispatcher_->ordersToMatch()[0]->orderId_);
    EXPECT_EQ(w->orderId_, dispatcher_->ordersToMatch()[1]->orderId_);
}

// =============================================================================
// Which restored orders are booked, for every status (#165, #215)
// =============================================================================

TEST_F(StorageRecordDispatcherTest, OnlyAWorkingOrderIsBookedOrLeftToMatch)
{
    // A live book holds only orders that are NEW or PARTFILL with quantity left, and a restore must book exactly those. It
    // booked every status but a list of terminal ones, SUSPENDED included, though suspending takes an order out of the
    // book (#215). A market order is never booked: an open one is left to match (#207). PENDINGNEW is never set and
    // INVALID never on an order, and RECEIVEDNEW is an order before acceptance, so none of the three should come back.
    // A working stop order whose stop hasn't been reached is held, in the stop book; once released, a STOP is matched as
    // a market order and a STOPLIMIT booked as a limit order (#223).
    enum Outcome
    {
        NEITHER,
        BOOKED,
        TO_MATCH,
        HELD
    };
    struct Case
    {
        OrderStatus status_;
        Outcome limit_;  // with quantity left
        Outcome market_; // with quantity left
    };
    const Case cases[] = {
        { INVALID_ORDSTATUS, NEITHER, NEITHER },        { RECEIVEDNEW_ORDSTATUS, NEITHER, NEITHER },
        { REJECTED_ORDSTATUS, NEITHER, NEITHER },       { PENDINGNEW_ORDSTATUS, NEITHER, NEITHER },
        { PENDINGREPLACE_ORDSTATUS, NEITHER, NEITHER }, { NEW_ORDSTATUS, BOOKED, TO_MATCH },
        { PARTFILL_ORDSTATUS, BOOKED, TO_MATCH },       { FILLED_ORDSTATUS, NEITHER, NEITHER },
        { EXPIRED_ORDSTATUS, NEITHER, NEITHER },        { DFD_ORDSTATUS, NEITHER, NEITHER },
        { SUSPENDED_ORDSTATUS, NEITHER, NEITHER },      { REPLACED_ORDSTATUS, NEITHER, NEITHER },
        { CANCELED_ORDSTATUS, NEITHER, NEITHER },
    };
    static_assert(std::size(cases) == CANCELED_ORDSTATUS + 1, "a case for every status");
    const char *const names[] = { "neither", "booked", "left to match", "held" };
    const struct
    {
        OrderType type_;
        bool stopTriggered_;
        const char *name_;
    } kinds[] = { { LIMIT_ORDERTYPE, false, "limit" },
                  { MARKET_ORDERTYPE, false, "market" },
                  { STOPLIMIT_ORDERTYPE, false, "held stop limit" },
                  { STOP_ORDERTYPE, false, "held stop" },
                  { STOPLIMIT_ORDERTYPE, true, "released stop limit" },
                  { STOP_ORDERTYPE, true, "released stop" } };

    const SourceIdT instrument = test::addInstrument("STATUSES");
    u64 number = 100;
    for (const Case &c : cases)
    {
        for (const auto &kind : kinds)
        {
            for (const QuantityT leaves : { QuantityT(100), QuantityT(0) })
            {
                OrderBookImpl book;
                OrderBookImpl::InstrumentsT instruments;
                instruments.insert(instrument);
                book.init(instruments);
                OrderDataStorage storage;
                TestFileSaver saver;
                StorageRecordDispatcher dispatcher;
                dispatcher.init(restore_.get(), &book, &saver, &storage);
                dispatcher.startLoad();
                std::unique_ptr<OrderEntry> order = bookedOrder(instrument, ++number, BUY_SIDE, 10.0, 100);
                order->status_ = c.status_;
                order->ordType_ = kind.type_;
                order->stopPx_ = 9.0;
                order->stopTriggered_ = kind.stopTriggered_;
                order->leavesQty_ = leaves;
                loadOrderRecord(dispatcher, *order);
                dispatcher.finishLoad();

                const Outcome expected = (0 == leaves)       ? NEITHER
                                         : order->heldStop() ? ((BOOKED == c.limit_) ? HELD : NEITHER)
                                         : (OrderParams::Book::PRICE == order->book()) ? c.limit_
                                                                                       : c.market_;
                const Outcome actual = book.getTop(instrument, BUY_SIDE).isValid() ? BOOKED
                                       : !dispatcher.ordersToMatch().empty()       ? TO_MATCH
                                       : book.holdsStops(instrument)               ? HELD
                                                                                   : NEITHER;
                EXPECT_EQ(std::string(names[expected]), std::string(names[actual]))
                    << "status " << c.status_ << ", " << kind.name_ << ", " << leaves << " left";
            }
        }
    }
}

// =============================================================================
// The FX branch's data, read here (#165)
// =============================================================================

TEST_F(StorageRecordDispatcherTest, AnFxSwapRecordIsRefusedNotMisread)
{
    // FX's golden swap record (feature/fx-swap-support, 0e96d47): an order's fields, then '.', its far price, '.', and its
    // far settlement date (#144), with an order type this branch doesn't have. Before #149 a decoder that ignored the
    // bytes past its fields read it as a priced order with no far leg. It must be refused, so a server pointed at an FX
    // directory doesn't start (#203) rather than trade half a swap.
    const IdT fxSwapId(4446, 20261013);
    const std::string record = fromHex("06000000952835013a65000000000000002e060000003a10000000000000002e"
                                       "070000003a11000000000000002e020000003a0c000000000000002e03000000"
                                       "3a0d000000000000002e040000003a0e000000000000002e010000003a0b0000"
                                       "00000000002e080000003a12000000000000002e942835013a5b110000000000"
                                       "002e011f9035a10100002e021f9035a10100002e031f9035a10100002e041f90"
                                       "35a10100002e00000000005059402e00000000002059402e0000000000485940"
                                       "2e00000000004459402e060000002e020000002e050000002e030000002e0400"
                                       "00002e020000002e020000002e050000002ee80300002e580200002e90010000"
                                       "2ee90300002e910100002e030000003a020000002e000000000000f13f2e0500"
                                       "863ba1010000");
    // The codec itself refuses it, as a load does. Its instrument is the golden records', registered so that it is the
    // swap's bytes that are refused, not a missing instrument.
    restoreGoldenInstrument();
    const std::string payload = record.substr(sizeof(u32));
    try
    {
        std::unique_ptr<OrderEntry> order(OrderCodec::decode(fxSwapId, 0, payload.data(), payload.size()));
        ADD_FAILURE() << "FX's swap record was read as an order of type " << order->ordType_;
    }
    catch (const std::runtime_error &ex)
    {
        // The far leg: '.', its price, '.', its date (#149's bounds check)
        EXPECT_NE(std::string::npos, std::string(ex.what()).find("18 bytes after its last field")) << ex.what();
    }
}
