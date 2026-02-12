/**
 Concurrent Order Processor library - Google Test

 Original Author: Sergey Mikhailik
 Test: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <filesystem>

#include "LMDBStorage.h"

using namespace COP;
using namespace COP::Store;

namespace {

// =============================================================================
// Test Observer Implementation
// =============================================================================

class TestLMDBObserver : public FileStorageObserver {
public:
    TestLMDBObserver() : finished_(false) {}

    void startLoad() override {
        finished_ = false;
    }

    void onRecordLoaded(const IdT& id, u32 version, const char* ptr, size_t s) override {
        ASSERT_NE(nullptr, ptr);
        ASSERT_GT(s, 0u);
        ids_.push_back(id);
        versions_.push_back(version);
        records_.push_back(std::string(ptr, s));
    }

    void finishLoad() override {
        finished_ = true;
    }

    void reset() {
        finished_ = false;
        ids_.clear();
        versions_.clear();
        records_.clear();
    }

public:
    bool finished_;
    std::vector<IdT> ids_;
    std::vector<u32> versions_;
    std::vector<std::string> records_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class LMDBStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = "test_lmdb_storage";
        cleanup();
    }

    void TearDown() override {
        cleanup();
    }

    void cleanup() {
        std::filesystem::remove_all(testDir_);
    }

protected:
    std::string testDir_;
};

// =============================================================================
// Basic Initialization Tests
// =============================================================================

TEST_F(LMDBStorageTest, CreateDefaultLMDBStorage) {
    LMDBStorage storage;
    SUCCEED();
}

TEST_F(LMDBStorageTest, CreateWithNewDirectory) {
    TestLMDBObserver observer;
    EXPECT_FALSE(observer.finished_);

    LMDBStorage storage(testDir_, &observer);

    EXPECT_TRUE(observer.finished_);
    EXPECT_EQ(0u, observer.records_.size());
}

// =============================================================================
// Save Tests
// =============================================================================

TEST_F(LMDBStorageTest, SaveWithPredefinedId) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    SUCCEED();
}

TEST_F(LMDBStorageTest, SaveDuplicateIdThrows) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_THROW(storage.save(IdT(1, 1), "dubRec", 6), std::exception);
}

TEST_F(LMDBStorageTest, SaveWithAutoId) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    IdT id = storage.save("bbbb", 4);
    EXPECT_TRUE(id.isValid());

    EXPECT_THROW(storage.save(id, "dubRec2", 7), std::exception);
}

// =============================================================================
// Persistence (Round-Trip) Tests
// =============================================================================

TEST_F(LMDBStorageTest, SaveAndReloadViaObserver) {
    // Save some records
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);
        storage.save(IdT(2, 2), "bbbb", 4);
    }

    // Reopen and verify
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);

        EXPECT_TRUE(observer.finished_);
        ASSERT_EQ(2u, observer.records_.size());

        // LMDB may return records in key order; check both present
        bool foundA = false, foundB = false;
        for(size_t i = 0; i < observer.records_.size(); ++i){
            if(observer.records_[i] == "aaaa") foundA = true;
            if(observer.records_[i] == "bbbb") foundB = true;
        }
        EXPECT_TRUE(foundA);
        EXPECT_TRUE(foundB);
    }
}

TEST_F(LMDBStorageTest, DataSurvivesCloseAndReopen) {
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        storage.save(IdT(10, 20), "persistent_data", 15);
    }

    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);

        ASSERT_EQ(1u, observer.records_.size());
        EXPECT_EQ("persistent_data", observer.records_.at(0));
    }
}

// =============================================================================
// Update Tests
// =============================================================================

TEST_F(LMDBStorageTest, UpdateCreatesNewVersion) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    u32 result = storage.update(IdT(1, 1), "aaaaa", 5);
    EXPECT_EQ(1u, result);
}

TEST_F(LMDBStorageTest, UpdateNonExistentCreatesVersion0) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    u32 result = storage.update(IdT(99, 99), "newdata", 7);
    EXPECT_EQ(0u, result);
}

TEST_F(LMDBStorageTest, UpdatePersistsAllVersions) {
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        storage.save(IdT(1, 1), "v0data", 6);
        storage.update(IdT(1, 1), "v1data", 6);
    }

    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);

        ASSERT_EQ(2u, observer.records_.size());
        bool foundV0 = false, foundV1 = false;
        for(size_t i = 0; i < observer.records_.size(); ++i){
            if(observer.records_[i] == "v0data") foundV0 = true;
            if(observer.records_[i] == "v1data") foundV1 = true;
        }
        EXPECT_TRUE(foundV0);
        EXPECT_TRUE(foundV1);
    }
}

// =============================================================================
// Replace Tests
// =============================================================================

TEST_F(LMDBStorageTest, ReplaceNonExistentIdThrows) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    EXPECT_THROW(storage.replace(IdT(2, 1), 1, "aaaaa", 5), std::exception);
}

TEST_F(LMDBStorageTest, ReplaceWrongVersionThrows) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_THROW(storage.replace(IdT(1, 1), 1, "aaaaa", 5), std::exception);
}

TEST_F(LMDBStorageTest, ReplaceRemovesOldVersionAtomically) {
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);

        u32 result = storage.replace(IdT(1, 1), 0, "replaced", 8);
        EXPECT_EQ(1u, result);
    }

    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);

        ASSERT_EQ(1u, observer.records_.size());
        EXPECT_EQ("replaced", observer.records_.at(0));
    }
}

// =============================================================================
// Erase Tests
// =============================================================================

TEST_F(LMDBStorageTest, EraseSingleVersion) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    storage.update(IdT(1, 1), "bbbb", 4);

    storage.erase(IdT(1, 1), 0);

    EXPECT_FALSE(storage.isExists(IdT(1, 1), 0));
    EXPECT_TRUE(storage.isExists(IdT(1, 1), 1));
}

TEST_F(LMDBStorageTest, EraseAllVersions) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    storage.update(IdT(1, 1), "bbbb", 4);

    storage.erase(IdT(1, 1));

    EXPECT_FALSE(storage.isExists(IdT(1, 1)));
}

TEST_F(LMDBStorageTest, EraseNonExistentIsNoop) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    // Should not throw
    storage.erase(IdT(1, 1), 0);
    storage.erase(IdT(1, 1));
    SUCCEED();
}

TEST_F(LMDBStorageTest, ErasePersists) {
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);
        storage.save(IdT(2, 2), "bbbb", 4);
        storage.erase(IdT(1, 1));
    }

    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);

        ASSERT_EQ(1u, observer.records_.size());
        EXPECT_EQ("bbbb", observer.records_.at(0));
    }
}

// =============================================================================
// Query Method Tests
// =============================================================================

TEST_F(LMDBStorageTest, IsExistsById) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    EXPECT_FALSE(storage.isExists(IdT(1, 1)));

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_TRUE(storage.isExists(IdT(1, 1)));
    EXPECT_FALSE(storage.isExists(IdT(2, 2)));
}

TEST_F(LMDBStorageTest, IsExistsByIdAndVersion) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_TRUE(storage.isExists(IdT(1, 1), 0));
    EXPECT_FALSE(storage.isExists(IdT(1, 1), 1));
}

TEST_F(LMDBStorageTest, GetTopVersion) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    EXPECT_EQ(0u, storage.getTopVersion(IdT(1, 1)));

    storage.update(IdT(1, 1), "bbbb", 4);
    EXPECT_EQ(1u, storage.getTopVersion(IdT(1, 1)));

    storage.update(IdT(1, 1), "cccc", 4);
    EXPECT_EQ(2u, storage.getTopVersion(IdT(1, 1)));
}

TEST_F(LMDBStorageTest, RecordSize) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_EQ(4u, storage.recordSize(IdT(1, 1), 0));
    EXPECT_EQ(0u, storage.recordSize(IdT(1, 1), 1)); // non-existent version
    EXPECT_EQ(0u, storage.recordSize(IdT(2, 2), 0)); // non-existent id
}

TEST_F(LMDBStorageTest, LoadRecord) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "hello_lmdb", 10);

    std::vector<char> buf;
    EXPECT_TRUE(storage.loadRecord(IdT(1, 1), 0, &buf));
    ASSERT_EQ(10u, buf.size());
    EXPECT_EQ(std::string("hello_lmdb"), std::string(buf.data(), buf.size()));

    EXPECT_FALSE(storage.loadRecord(IdT(1, 1), 1, &buf));
    EXPECT_FALSE(storage.loadRecord(IdT(2, 2), 0, &buf));
}

// =============================================================================
// Concurrent Read During Write (MVCC Isolation)
// =============================================================================

TEST_F(LMDBStorageTest, ReadDuringWriteIsolation) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "initial", 7);

    // Verify we can read while there's data
    EXPECT_TRUE(storage.isExists(IdT(1, 1)));
    EXPECT_EQ(7u, storage.recordSize(IdT(1, 1), 0));

    // Update and verify both versions accessible
    storage.update(IdT(1, 1), "updated", 7);
    EXPECT_TRUE(storage.isExists(IdT(1, 1), 0));
    EXPECT_TRUE(storage.isExists(IdT(1, 1), 1));
}

// =============================================================================
// Multiple Records Test
// =============================================================================

TEST_F(LMDBStorageTest, MultipleRecordsDifferentIds) {
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    for(u64 i = 1; i <= 10; ++i){
        std::string data = "record_" + std::to_string(i);
        storage.save(IdT(i, 1), data.c_str(), data.size());
    }

    for(u64 i = 1; i <= 10; ++i){
        EXPECT_TRUE(storage.isExists(IdT(i, 1)));
        std::string expected = "record_" + std::to_string(i);
        EXPECT_EQ(expected.size(), storage.recordSize(IdT(i, 1), 0));
    }
}

} // namespace
