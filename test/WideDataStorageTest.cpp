/**
 Concurrent Order Processor library - New Test File

 Author: Sergey Mikhailik
 Test Implementation: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "TestFixtures.h"
#include "TestAux.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::Store;
using namespace test;

namespace {

// =============================================================================
// Test Fixture
// =============================================================================

class WideDataStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        WideDataStorage::create();
    }

    void TearDown() override {
        WideDataStorage::destroy();
    }

    WideParamsDataStorage* storage() {
        return WideDataStorage::instance();
    }
};

// =============================================================================
// Basic Singleton Tests
// =============================================================================

TEST_F(WideDataStorageTest, InstanceNotNull) {
    ASSERT_NE(nullptr, storage());
}

TEST_F(WideDataStorageTest, InstanceReturnsSamePointer) {
    auto* ptr1 = storage();
    auto* ptr2 = storage();
    EXPECT_EQ(ptr1, ptr2);
}

// =============================================================================
// Instrument Tests
// =============================================================================

TEST_F(WideDataStorageTest, AddAndGetInstrument) {
    auto instr = new InstrumentEntry();
    instr->symbol_ = "AAPL";
    instr->securityId_ = "US0378331005";
    instr->securityIdSource_ = "ISIN";

    SourceIdT id = storage()->add(instr);
    ASSERT_TRUE(id.isValid());

    InstrumentEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("AAPL", retrieved.symbol_);
    EXPECT_EQ("US0378331005", retrieved.securityId_);
    EXPECT_EQ("ISIN", retrieved.securityIdSource_);
}

TEST_F(WideDataStorageTest, AddMultipleInstruments) {
    std::vector<SourceIdT> ids;

    for (int i = 0; i < 10; ++i) {
        auto instr = new InstrumentEntry();
        instr->symbol_ = "SYM" + std::to_string(i);
        instr->securityId_ = "SEC" + std::to_string(i);
        instr->securityIdSource_ = "SRC" + std::to_string(i);

        SourceIdT id = storage()->add(instr);
        ASSERT_TRUE(id.isValid());
        ids.push_back(id);
    }

    // Verify all were stored correctly
    for (int i = 0; i < 10; ++i) {
        InstrumentEntry retrieved;
        storage()->get(ids[i], &retrieved);
        EXPECT_EQ("SYM" + std::to_string(i), retrieved.symbol_);
    }
}

// =============================================================================
// String Tests
// =============================================================================

TEST_F(WideDataStorageTest, AddAndGetString) {
    auto str = new StringT("Hello, World!");

    SourceIdT id = storage()->add(str);
    // Note: WideDataStorage uses date_=0 for IDs, so isValid() returns false
    // Check that id_ was assigned instead
    ASSERT_NE(0u, id.id_);

    StringT retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("Hello, World!", retrieved);
}

TEST_F(WideDataStorageTest, AddMultipleStrings) {
    std::vector<SourceIdT> ids;

    for (int i = 0; i < 100; ++i) {
        auto str = new StringT("String" + std::to_string(i));
        SourceIdT id = storage()->add(str);
        // Note: WideDataStorage uses date_=0 for IDs, so isValid() returns false
        ASSERT_NE(0u, id.id_);
        ids.push_back(id);
    }

    // Verify all were stored correctly
    for (int i = 0; i < 100; ++i) {
        StringT retrieved;
        storage()->get(ids[i], &retrieved);
        EXPECT_EQ("String" + std::to_string(i), retrieved);
    }
}

// =============================================================================
// Account Tests
// =============================================================================

TEST_F(WideDataStorageTest, AddAndGetAccount) {
    auto account = new AccountEntry();
    account->account_ = "ACCT001";
    account->firm_ = "TestFirm";
    account->type_ = PRINCIPAL_ACCOUNTTYPE;

    SourceIdT id = storage()->add(account);
    ASSERT_TRUE(id.isValid());

    AccountEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("ACCT001", retrieved.account_);
    EXPECT_EQ("TestFirm", retrieved.firm_);
    EXPECT_EQ(PRINCIPAL_ACCOUNTTYPE, retrieved.type_);
}

// =============================================================================
// Clearing Tests
// =============================================================================

TEST_F(WideDataStorageTest, AddAndGetClearing) {
    auto clearing = new ClearingEntry();
    clearing->firm_ = "ClearingFirm";

    SourceIdT id = storage()->add(clearing);
    ASSERT_TRUE(id.isValid());

    ClearingEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("ClearingFirm", retrieved.firm_);
}

// =============================================================================
// RawData Tests
// =============================================================================

TEST_F(WideDataStorageTest, AddAndGetRawData) {
    auto rawData = new RawDataEntry(STRING_RAWDATATYPE, "TestData", 8);

    SourceIdT id = storage()->add(rawData);
    ASSERT_TRUE(id.isValid());

    RawDataEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ(STRING_RAWDATATYPE, retrieved.type_);
}

// =============================================================================
// Executions Tests
// =============================================================================

TEST_F(WideDataStorageTest, AddAndGetExecutions) {
    auto execList = new ExecutionsT();

    SourceIdT id = storage()->add(execList);
    ASSERT_TRUE(id.isValid());

    ExecutionsT* retrieved = nullptr;
    storage()->get(id, &retrieved);

    ASSERT_NE(nullptr, retrieved);
}

// =============================================================================
// Concurrent Read Tests
// =============================================================================

TEST_F(WideDataStorageTest, ConcurrentReads) {
    // Add some test data
    std::vector<SourceIdT> instrIds;
    for (int i = 0; i < 10; ++i) {
        auto instr = new InstrumentEntry();
        instr->symbol_ = "SYM" + std::to_string(i);
        instrIds.push_back(storage()->add(instr));
    }

    // Concurrent reads
    const int numThreads = 8;
    const int numReadsPerThread = 100;
    std::atomic<int> totalReads{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &instrIds, &totalReads, numReadsPerThread]() {
            for (int i = 0; i < numReadsPerThread; ++i) {
                InstrumentEntry retrieved;
                SourceIdT id = instrIds[i % instrIds.size()];
                storage()->get(id, &retrieved);
                ++totalReads;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numReadsPerThread, totalReads.load());
}

// =============================================================================
// Mixed Concurrent Operations Tests
// =============================================================================

TEST_F(WideDataStorageTest, ConcurrentAddAndRead) {
    const int numWriters = 2;
    const int numReaders = 4;
    const int numOpsPerThread = 50;

    std::atomic<int> totalAdds{0};
    std::atomic<int> totalReads{0};

    std::vector<std::thread> threads;

    // Writer threads
    for (int t = 0; t < numWriters; ++t) {
        threads.emplace_back([this, &totalAdds, numOpsPerThread, t]() {
            for (int i = 0; i < numOpsPerThread; ++i) {
                auto instr = new InstrumentEntry();
                instr->symbol_ = "W" + std::to_string(t) + "_" + std::to_string(i);
                storage()->add(instr);
                ++totalAdds;
            }
        });
    }

    // Let writers add some data first
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Reader threads
    for (int t = 0; t < numReaders; ++t) {
        threads.emplace_back([this, &totalReads, numOpsPerThread]() {
            for (int i = 0; i < numOpsPerThread; ++i) {
                // Just verify the singleton is accessible
                auto* ptr = storage();
                EXPECT_NE(nullptr, ptr);
                ++totalReads;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numWriters * numOpsPerThread, totalAdds.load());
    EXPECT_EQ(numReaders * numOpsPerThread, totalReads.load());
}

// =============================================================================
// Restore Tests
// =============================================================================

TEST_F(WideDataStorageTest, RestoreInstrument) {
    auto instr = new InstrumentEntry();
    instr->symbol_ = "RESTORED";
    instr->securityId_ = "REST001";
    instr->id_ = IdT(999, 1);

    storage()->restore(instr);

    // Verify it was restored
    InstrumentEntry retrieved;
    storage()->get(instr->id_, &retrieved);

    EXPECT_EQ("RESTORED", retrieved.symbol_);
}

// =============================================================================
// Helper Function Tests (using test utilities)
// =============================================================================

TEST_F(WideDataStorageTest, AddInstrumentHelper) {
    SourceIdT id = addInstrument("TSLA", "US88160R1014", "ISIN");
    ASSERT_TRUE(id.isValid());

    InstrumentEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("TSLA", retrieved.symbol_);
}

TEST_F(WideDataStorageTest, AddAccountHelper) {
    SourceIdT id = addAccount("TestAccount", "TestFirm", AGENCY_ACCOUNTTYPE);
    ASSERT_TRUE(id.isValid());

    AccountEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("TestAccount", retrieved.account_);
    EXPECT_EQ("TestFirm", retrieved.firm_);
    EXPECT_EQ(AGENCY_ACCOUNTTYPE, retrieved.type_);
}

TEST_F(WideDataStorageTest, AddClearingHelper) {
    SourceIdT id = addClearing("ClearFirm");
    ASSERT_TRUE(id.isValid());

    ClearingEntry retrieved;
    storage()->get(id, &retrieved);

    EXPECT_EQ("ClearFirm", retrieved.firm_);
}

} // namespace
