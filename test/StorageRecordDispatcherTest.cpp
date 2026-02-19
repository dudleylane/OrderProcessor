/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
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

using namespace COP;
using namespace COP::Codec;
using namespace COP::Store;
using namespace test;

namespace {

// =============================================================================
// Test Data Storage Restore Implementation
// =============================================================================

class TestDataStorageRestore : public DataStorageRestore {
public:
    ~TestDataStorageRestore() override {
        for (auto* instr : instruments_) {
            delete instr;
        }
        for (auto* acc : accounts_) {
            delete acc;
        }
        for (auto& str : strings_) {
            delete str.str_;
        }
        for (auto* raw : rawDatas_) {
            delete raw->data_;
            delete raw;
        }
        for (auto* clr : clearings_) {
            delete clr;
        }
    }

    void restore(InstrumentEntry* val) override {
        ASSERT_NE(nullptr, val);
        instruments_.push_back(val);
    }

    void restore(const IdT& id, StringT* val) override {
        ASSERT_NE(nullptr, val);
        strings_.push_back(IdString(val, id));
    }

    void restore(RawDataEntry* val) override {
        ASSERT_NE(nullptr, val);
        rawDatas_.push_back(val);
    }

    void restore(AccountEntry* val) override {
        ASSERT_NE(nullptr, val);
        accounts_.push_back(val);
    }

    void restore(ClearingEntry* val) override {
        ASSERT_NE(nullptr, val);
        clearings_.push_back(val);
    }

    void restore(ExecutionsT* val) override {
        // Not used in tests
    }

public:
    std::deque<InstrumentEntry*> instruments_;
    std::deque<AccountEntry*> accounts_;
    std::deque<ClearingEntry*> clearings_;
    std::deque<RawDataEntry*> rawDatas_;

    struct IdString {
        StringT* str_;
        IdT id_;

        IdString() : str_(nullptr), id_() {}
        IdString(StringT* v, const IdT& id) : str_(v), id_(id) {}
    };
    std::deque<IdString> strings_;
};

// =============================================================================
// Test Order Book Implementation
// =============================================================================

class TestOrderBook : public OrderBook {
public:
    ~TestOrderBook() override = default;

    void add(const OrderEntry& /*order*/) override {}

    void remove(const OrderEntry& /*order*/) override {}

    IdT find(const OrderFunctor& /*functor*/) const override {
        return IdT();
    }

    void findAll(const OrderFunctor& /*functor*/, OrdersT* /*result*/) const override {}

    IdT getTop(const SourceIdT& /*instrument*/, const Side& /*side*/) const override {
        return IdT();
    }

    void restore(const OrderEntry& order) override {
        orders_.push_back(&order);  // Just for testing
    }

    std::deque<const OrderEntry*> orders_;
};

// =============================================================================
// Test File Saver Implementation
// =============================================================================

class TestFileSaver : public FileSaver {
public:
    TestFileSaver() = default;
    ~TestFileSaver() override = default;

    void load(const std::string& /*fileName*/, FileStorageObserver* /*observer*/) override {}

    IdT save(const char* buf, size_t size) override {
        EXPECT_NE(nullptr, buf);
        EXPECT_GT(size, 0u);
        IdT id;
        records_.insert(RecordsT::value_type(id, Record(std::string(buf, size))));
        return id;
    }

    void save(const IdT& id, const char* buf, size_t size) override {
        EXPECT_NE(nullptr, buf);
        EXPECT_GT(size, 0u);
        records_.insert(RecordsT::value_type(id, Record(std::string(buf, size))));
    }

    u32 update(const IdT& /*id*/, const char* /*buf*/, size_t /*size*/) override {
        return 0;
    }

    u32 replace(const IdT& /*id*/, u32 /*version*/, const char* /*buf*/, size_t /*size*/) override {
        return 0;
    }

    void erase(const IdT& /*id*/, u32 /*version*/) override {}

    void erase(const IdT& /*id*/) override {}

    struct Record {
        std::string record_;

        Record() : record_() {}
        Record(const std::string& rec) : record_(rec) {}
    };
    typedef std::map<IdT, Record> RecordsT;
    RecordsT records_;
};

// =============================================================================
// Test Helpers
// =============================================================================

SourceIdT addTestRawData(const std::string& name) {
    auto raw = new RawDataEntry();
    raw->type_ = STRING_RAWDATATYPE;
    auto val = new char[name.size()];
    std::memcpy(val, name.c_str(), name.size());
    raw->data_ = val;
    raw->length_ = name.size();
    return WideDataStorage::instance()->add(raw);
}

OrderEntry* createTestOrder() {
    SourceIdT instrId = addInstrument("instrument", "instrId", "instrSrc");
    SourceIdT clOrderId = addTestRawData("clOrderId_");
    auto val = new OrderEntry(
        SourceIdT(1, 1), SourceIdT(2, 2), clOrderId,
        SourceIdT(4, 4), instrId, SourceIdT(6, 6),
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

std::string createRecordTypePrefix(int recordType) {
    std::string buf;
    char typebuf[36];
    std::memcpy(typebuf, &recordType, sizeof(int));
    buf.append(typebuf, sizeof(int));
    return buf;
}

// =============================================================================
// Test Fixture
// =============================================================================

class StorageRecordDispatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        WideDataStorage::create();
        restore_ = std::make_unique<TestDataStorageRestore>();
        orderBook_ = std::make_unique<TestOrderBook>();
        saver_ = std::make_unique<TestFileSaver>();
        orderStorage_ = std::make_unique<OrderDataStorage>();
        dispatcher_ = std::make_unique<StorageRecordDispatcher>();
    }

    void TearDown() override {
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

TEST_F(StorageRecordDispatcherTest, EmptyInitialization) {
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    SUCCEED();
}

TEST_F(StorageRecordDispatcherTest, InitWithStartLoadFinishLoad) {
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();
    dispatcher_->finishLoad();
    SUCCEED();
}

// =============================================================================
// Load Instrument Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadInstrumentRecord) {
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

TEST_F(StorageRecordDispatcherTest, LoadStringRecord) {
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

TEST_F(StorageRecordDispatcherTest, LoadAccountRecord) {
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

TEST_F(StorageRecordDispatcherTest, LoadClearingRecord) {
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

TEST_F(StorageRecordDispatcherTest, LoadRawDataRecord) {
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    RawDataEntry val;
    char cbuf[32] = "RawDataValue_";
    val.id_ = IdT(1111, 6789);
    val.data_ = cbuf;
    val.length_ = 13;
    val.type_ = STRING_RAWDATATYPE;

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

TEST_F(StorageRecordDispatcherTest, LoadOrderRecord) {
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());
    dispatcher_->startLoad();

    std::unique_ptr<OrderEntry> val(createTestOrder());
    val->orderId_ = IdT(1111, 6789);

    std::string buf = createRecordTypePrefix(StorageRecordDispatcher::ORDER_RECORDTYPE);
    IdT id;
    u32 version = 0;
    OrderCodec::encode(*val, &buf, &id, &version);
    ASSERT_FALSE(buf.empty());

    dispatcher_->onRecordLoaded(id, version, buf.c_str(), buf.size());

    ASSERT_EQ(1u, orderBook_->orders_.size());
    ASSERT_NE(nullptr, orderBook_->orders_.at(0));
    EXPECT_TRUE(orderBook_->orders_.at(0)->compare(*val));

    dispatcher_->finishLoad();
}

// =============================================================================
// Load Multiple Record Types Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, LoadMultipleRecordTypes) {
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
        RawDataEntry val;
        char cbuf[32] = "RawDataValue_";
        val.id_ = IdT(1111, 6789);
        val.data_ = cbuf;
        val.length_ = 13;
        val.type_ = STRING_RAWDATATYPE;

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

TEST_F(StorageRecordDispatcherTest, SaveInstrumentRecord) {
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

TEST_F(StorageRecordDispatcherTest, SaveStringRecord) {
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    StringT val = "string_";
    std::string expectedBuf = createRecordTypePrefix(StorageRecordDispatcher::STRING_RECORDTYPE);
    IdT id(1, 1234);
    u32 version = 0;
    StringTCodec::encode(val, &expectedBuf);

    dispatcher_->save(id, val);

    auto it = saver_->records_.find(id);
    ASSERT_NE(saver_->records_.end(), it);
    EXPECT_EQ(0, it->second.record_.compare(expectedBuf));
}

// =============================================================================
// Save Account Tests
// =============================================================================

TEST_F(StorageRecordDispatcherTest, SaveAccountRecord) {
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

TEST_F(StorageRecordDispatcherTest, SaveClearingRecord) {
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

TEST_F(StorageRecordDispatcherTest, SaveRawDataRecord) {
    dispatcher_->init(restore_.get(), orderBook_.get(), saver_.get(), orderStorage_.get());

    RawDataEntry val;
    char cbuf[32] = "RawDataValue_";
    val.id_ = IdT(1114, 6789);
    val.data_ = cbuf;
    val.length_ = 13;
    val.type_ = STRING_RAWDATATYPE;

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

TEST_F(StorageRecordDispatcherTest, SaveOrderRecord) {
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

TEST_F(StorageRecordDispatcherTest, SaveMultipleRecordTypes) {
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
        RawDataEntry val;
        char cbuf[32] = "rawdata1";
        val.id_ = IdT(2004, 1);
        val.data_ = cbuf;
        val.length_ = 8;
        val.type_ = STRING_RAWDATATYPE;
        dispatcher_->save(val);
    }

    // Verify all saved (4 records)
    EXPECT_EQ(4u, saver_->records_.size());
}

} // namespace
