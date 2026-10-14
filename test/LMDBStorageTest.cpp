/**
 Concurrent Order Processor library - Google Test

 Authors: dudleylane, Claude
 Test: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "LMDBStorage.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using namespace test;

namespace
{

// =============================================================================
// Test Observer Implementation
// =============================================================================

class TestLMDBObserver : public FileStorageObserver
{
public:
    TestLMDBObserver() : finished_(false) {}

    void startLoad() override
    {
        finished_ = false;
    }

    void onRecordLoaded(const IdT &id, u32 version, const char *ptr, size_t s) override
    {
        ASSERT_NE(nullptr, ptr);
        ASSERT_GT(s, 0u);
        ids_.push_back(id);
        versions_.push_back(version);
        records_.push_back(std::string(ptr, s));
    }

    void finishLoad() override
    {
        finished_ = true;
    }

    void reset()
    {
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

class LMDBStorageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // ctest runs every test from the same working directory, so a fixed relative path is shared by
        // all tests of a parallel run: one case would delete another's database. Give each case its own.
        testDir_ = uniqueTestPath("lmdb");
        cleanup();
    }

    void TearDown() override
    {
        cleanup();
    }

    void cleanup()
    {
        std::filesystem::remove_all(testDir_);
    }

protected:
    std::string testDir_;
};

// The map a test fills quickly, and records 16 of which fill it (#148)
const size_t SMALL_MAP = 1024 * 1024;
const std::string BIG_RECORD(64 * 1024, 'r');

// =============================================================================
// Basic Initialization Tests
// =============================================================================

TEST_F(LMDBStorageTest, CreateDefaultLMDBStorage)
{
    LMDBStorage storage;
    SUCCEED();
}

TEST_F(LMDBStorageTest, CreateWithNewDirectory)
{
    TestLMDBObserver observer;
    EXPECT_FALSE(observer.finished_);

    LMDBStorage storage(testDir_, &observer);

    EXPECT_TRUE(observer.finished_);
    EXPECT_EQ(0u, observer.records_.size());
}

// =============================================================================
// Save Tests
// =============================================================================

TEST_F(LMDBStorageTest, SaveWithPredefinedId)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    SUCCEED();
}

TEST_F(LMDBStorageTest, SaveDuplicateIdThrows)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_THROW(storage.save(IdT(1, 1), "dubRec", 6), std::exception);
}

TEST_F(LMDBStorageTest, SaveWithAutoId)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    IdT id = storage.save("bbbb", 4);
    EXPECT_TRUE(id.isValid());

    EXPECT_THROW(storage.save(id, "dubRec2", 7), std::exception);
}

// =============================================================================
// Persistence (Round-Trip) Tests
// =============================================================================

TEST_F(LMDBStorageTest, SaveAndReloadViaObserver)
{
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
        for (size_t i = 0; i < observer.records_.size(); ++i)
        {
            if (observer.records_[i] == "aaaa")
            {
                foundA = true;
            }
            if (observer.records_[i] == "bbbb")
            {
                foundB = true;
            }
        }
        EXPECT_TRUE(foundA);
        EXPECT_TRUE(foundB);
    }
}

TEST_F(LMDBStorageTest, DataSurvivesCloseAndReopen)
{
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

TEST_F(LMDBStorageTest, UpdateCreatesNewVersion)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    u32 result = storage.update(IdT(1, 1), "aaaaa", 5);
    EXPECT_EQ(1u, result);
}

TEST_F(LMDBStorageTest, UpdateNonExistentCreatesVersion0)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    u32 result = storage.update(IdT(99, 99), "newdata", 7);
    EXPECT_EQ(0u, result);
}

TEST_F(LMDBStorageTest, UpdatePersistsBothVersions)
{
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
        for (size_t i = 0; i < observer.records_.size(); ++i)
        {
            if (observer.records_[i] == "v0data")
            {
                foundV0 = true;
            }
            if (observer.records_[i] == "v1data")
            {
                foundV1 = true;
            }
        }
        EXPECT_TRUE(foundV0);
        EXPECT_TRUE(foundV1);
    }
}

// update() erases the versions it supersedes, keeping the newest one it finds besides its own (#148). The transaction
// that wrote the new version can still roll it back, which erases just that one, and the one kept is then the newest
// again. Before #148 every version stayed, and an order's changes filled the map.
TEST_F(LMDBStorageTest, UpdateKeepsTheNewestVersionBeforeItsOwn)
{
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        EXPECT_EQ(0u, storage.update(IdT(1, 1), "v0", 2));
        EXPECT_EQ(0u, storage.update(IdT(2, 1), "w0", 2));
        EXPECT_EQ(1u, storage.update(IdT(1, 1), "v1", 2));
        EXPECT_EQ(2u, storage.update(IdT(1, 1), "v2", 2));
        EXPECT_FALSE(storage.isExists(IdT(1, 1), 0));
        EXPECT_TRUE(storage.isExists(IdT(1, 1), 1));
        EXPECT_TRUE(storage.isExists(IdT(1, 1), 2));
        EXPECT_TRUE(storage.isExists(IdT(2, 1), 0)) << "another record's version was erased";

        // A rollback erases the version it wrote, and leaves the one before it
        storage.erase(IdT(1, 1), 2);
        EXPECT_TRUE(storage.isExists(IdT(1, 1), 1));
        EXPECT_EQ(2u, storage.update(IdT(1, 1), "v2b", 3));
    }

    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);
    std::vector<std::string> loaded = observer.records_;
    std::sort(loaded.begin(), loaded.end());
    EXPECT_EQ((std::vector<std::string>{ "v1", "v2b", "w0" }), loaded);
}

// A directory written before #148 holds every version of an order, and the first update() since erases all but the
// newest of them, one after another
TEST_F(LMDBStorageTest, UpdateErasesEveryVersionOlderThanTheNewest)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);
    for (const char *v : { "v0", "v1", "v2" })
    {
        storage.update(IdT(1, 1), v, 2);
    }
    // save() writes version 0, which update() erased, so versions 0, 1 and 2 are there, as they were before #148
    storage.save(IdT(1, 1), "s0", 2);
    ASSERT_TRUE(storage.isExists(IdT(1, 1), 0));
    ASSERT_TRUE(storage.isExists(IdT(1, 1), 1));

    EXPECT_EQ(3u, storage.update(IdT(1, 1), "v3", 2));
    EXPECT_FALSE(storage.isExists(IdT(1, 1), 0));
    EXPECT_FALSE(storage.isExists(IdT(1, 1), 1));
    EXPECT_TRUE(storage.isExists(IdT(1, 1), 2));
    EXPECT_TRUE(storage.isExists(IdT(1, 1), 3));
}

// A version is a little-endian u32 in the key, so the keys aren't in numeric order: 256 comes before 255. update() keeps
// the newest by value, whether it comes before the versions it erases or after them.
TEST_F(LMDBStorageTest, UpdateKeepsTheNewestVersionByValueNotByKeyOrder)
{
    {
        TestLMDBObserver observer;
        LMDBStorage storage(testDir_, &observer);
        for (u32 v = 0; v <= 256; ++v)
        {
            ASSERT_EQ(v, storage.update(IdT(1, 1), "v", 1));
        }
        storage.save(IdT(1, 1), "s0", 2); // versions 0, 255 and 256, which the keys order 0, 256, 255
        EXPECT_EQ(257u, storage.update(IdT(1, 1), "new", 3));
        EXPECT_FALSE(storage.isExists(IdT(1, 1), 0));
        EXPECT_FALSE(storage.isExists(IdT(1, 1), 255));
        EXPECT_TRUE(storage.isExists(IdT(1, 1), 256));
        EXPECT_TRUE(storage.isExists(IdT(1, 1), 257));
    }

    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);
    std::vector<u32> versions = observer.versions_;
    std::sort(versions.begin(), versions.end());
    EXPECT_EQ((std::vector<u32>{ 256, 257 }), versions);
}

// =============================================================================
// Replace Tests
// =============================================================================

TEST_F(LMDBStorageTest, ReplaceNonExistentIdThrows)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    EXPECT_THROW(storage.replace(IdT(2, 1), 1, "aaaaa", 5), std::exception);
}

TEST_F(LMDBStorageTest, ReplaceWrongVersionThrows)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_THROW(storage.replace(IdT(1, 1), 1, "aaaaa", 5), std::exception);
}

TEST_F(LMDBStorageTest, ReplaceRemovesOldVersionAtomically)
{
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

TEST_F(LMDBStorageTest, EraseSingleVersion)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    storage.update(IdT(1, 1), "bbbb", 4);

    storage.erase(IdT(1, 1), 0);

    EXPECT_FALSE(storage.isExists(IdT(1, 1), 0));
    EXPECT_TRUE(storage.isExists(IdT(1, 1), 1));
}

TEST_F(LMDBStorageTest, EraseAllVersions)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    storage.update(IdT(1, 1), "bbbb", 4);

    storage.erase(IdT(1, 1));

    EXPECT_FALSE(storage.isExists(IdT(1, 1)));
}

TEST_F(LMDBStorageTest, EraseNonExistentIsNoop)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    // Should not throw
    storage.erase(IdT(1, 1), 0);
    storage.erase(IdT(1, 1));
    SUCCEED();
}

TEST_F(LMDBStorageTest, ErasePersists)
{
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

TEST_F(LMDBStorageTest, IsExistsById)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    EXPECT_FALSE(storage.isExists(IdT(1, 1)));

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_TRUE(storage.isExists(IdT(1, 1)));
    EXPECT_FALSE(storage.isExists(IdT(2, 2)));
}

TEST_F(LMDBStorageTest, IsExistsByIdAndVersion)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_TRUE(storage.isExists(IdT(1, 1), 0));
    EXPECT_FALSE(storage.isExists(IdT(1, 1), 1));
}

TEST_F(LMDBStorageTest, GetTopVersion)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);
    EXPECT_EQ(0u, storage.getTopVersion(IdT(1, 1)));

    storage.update(IdT(1, 1), "bbbb", 4);
    EXPECT_EQ(1u, storage.getTopVersion(IdT(1, 1)));

    storage.update(IdT(1, 1), "cccc", 4);
    EXPECT_EQ(2u, storage.getTopVersion(IdT(1, 1)));
}

TEST_F(LMDBStorageTest, RecordSize)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_EQ(4u, storage.recordSize(IdT(1, 1), 0));
    EXPECT_EQ(0u, storage.recordSize(IdT(1, 1), 1)); // non-existent version
    EXPECT_EQ(0u, storage.recordSize(IdT(2, 2), 0)); // non-existent id
}

TEST_F(LMDBStorageTest, LoadRecord)
{
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

TEST_F(LMDBStorageTest, ReadDuringWriteIsolation)
{
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

TEST_F(LMDBStorageTest, MultipleRecordsDifferentIds)
{
    TestLMDBObserver observer;
    LMDBStorage storage(testDir_, &observer);

    for (u64 i = 1; i <= 10; ++i)
    {
        std::string data = "record_" + std::to_string(i);
        storage.save(IdT(i, 1), data.c_str(), data.size());
    }

    for (u64 i = 1; i <= 10; ++i)
    {
        EXPECT_TRUE(storage.isExists(IdT(i, 1)));
        std::string expected = "record_" + std::to_string(i);
        EXPECT_EQ(expected.size(), storage.recordSize(IdT(i, 1), 0));
    }
}

// =============================================================================
// Map size (#148)
// =============================================================================

// The map was fixed at 256 MB, and once it was full every write threw MDB_MAP_FULL, after a restart too (#148). A write
// that finds it full now doubles it and goes through.
TEST_F(LMDBStorageTest, AWriteThatFindsTheMapFullGrowsIt)
{
    const u64 count = 48;
    {
        TestLMDBObserver observer;
        LMDBStorage storage(SMALL_MAP);
        storage.load(testDir_, &observer);
        EXPECT_EQ(SMALL_MAP, storage.mapSize());
        for (u64 i = 1; i <= count; ++i)
        {
            ASSERT_NO_THROW(storage.update(IdT(i, 1), BIG_RECORD.data(), BIG_RECORD.size())) << "record " << i;
        }
        EXPECT_GE(storage.mapSize(), 4 * SMALL_MAP);
        EXPECT_GE(storage.mapUsed(), count * BIG_RECORD.size());
        EXPECT_LE(storage.mapUsed(), storage.mapSize());
    }

    // A directory bigger than the map asked for opens with every record, and grows again when it must
    TestLMDBObserver observer;
    LMDBStorage storage(SMALL_MAP);
    storage.load(testDir_, &observer);
    EXPECT_EQ(count, observer.records_.size());
    EXPECT_NO_THROW(storage.save(IdT(count + 1, 1), BIG_RECORD.data(), BIG_RECORD.size()));
}

// Writers on several threads, as the transaction workers and the IO thread are, while the map grows under them. LMDB
// lets the size change only while no transaction is open in the process (#148).
TEST_F(LMDBStorageTest, TheMapGrowsWhileSeveralThreadsWrite)
{
    const u64 threads = 4;
    const u64 perThread = 12;
    {
        TestLMDBObserver observer;
        LMDBStorage storage(SMALL_MAP);
        storage.load(testDir_, &observer);
        std::atomic<int> failures(0);
        std::vector<std::thread> writers;
        for (u64 t = 0; t < threads; ++t)
        {
            writers.emplace_back(
                [&storage, &failures, t, perThread]()
                {
                    for (u64 i = 0; i < perThread; ++i)
                    {
                        const IdT id(t * perThread + i + 1, 1);
                        try
                        {
                            if (0 == i % 2)
                            {
                                storage.save(id, BIG_RECORD.data(), BIG_RECORD.size());
                            }
                            else
                            {
                                storage.update(id, BIG_RECORD.data(), BIG_RECORD.size());
                            }
                        }
                        catch (const std::exception &)
                        {
                            ++failures;
                        }
                    }
                });
        }
        for (std::thread &w : writers)
        {
            w.join();
        }
        EXPECT_EQ(0, failures.load());
        EXPECT_GE(storage.mapSize(), 4 * SMALL_MAP);
    }

    TestLMDBObserver observer;
    LMDBStorage storage(SMALL_MAP);
    storage.load(testDir_, &observer);
    EXPECT_EQ(threads * perThread, observer.records_.size());
}

// =============================================================================
// Groups (#163): several updates in one transaction
// =============================================================================

TEST_F(LMDBStorageTest, AGroupWritesItsUpdatesInOneTransaction)
{
    // A trade's two orders were committed one after the other, so a crash between the commits left half a trade (#163)
    TestLMDBObserver observer;
    LMDBStorage storage;
    storage.load(testDir_, &observer);
    const IdT buy(1, 1);
    const IdT sell(2, 1);
    ASSERT_EQ(0u, storage.update(buy, "buy-0", 5));
    ASSERT_EQ(0u, storage.update(sell, "sell-0", 6));
    const u64 before = storage.commits();

    storage.beginGroup();
    EXPECT_EQ(1u, storage.update(buy, "buy-1", 5));
    EXPECT_EQ(1u, storage.update(sell, "sell-1", 6));
    EXPECT_FALSE(storage.isExists(buy, 1)) << "a group's update was written before the group committed";
    storage.commitGroup();

    EXPECT_EQ(before + 1, storage.commits()) << "the group took more than one transaction";
    EXPECT_TRUE(storage.isExists(buy, 1));
    EXPECT_TRUE(storage.isExists(sell, 1));
    EXPECT_TRUE(storage.isExists(buy, 0)) << "the version before the new one is kept (#148)";
}

TEST_F(LMDBStorageTest, AnAbortedGroupWritesNothing)
{
    TestLMDBObserver observer;
    LMDBStorage storage;
    storage.load(testDir_, &observer);
    const IdT buy(1, 1);
    const IdT sell(2, 1);
    ASSERT_EQ(0u, storage.update(buy, "buy-0", 5));
    const u64 before = storage.commits();

    storage.beginGroup();
    EXPECT_EQ(1u, storage.update(buy, "buy-1", 5));
    EXPECT_EQ(0u, storage.update(sell, "sell-0", 6));
    storage.abortGroup();

    EXPECT_EQ(before, storage.commits());
    EXPECT_FALSE(storage.isExists(buy, 1));
    EXPECT_FALSE(storage.isExists(sell, 0));
    EXPECT_EQ(1u, storage.update(buy, "buy-1", 5)) << "an update after the abort found a version the group held";
}

TEST_F(LMDBStorageTest, AGroupThatFillsTheMapGrowsItAndWritesItAll)
{
    // The whole group runs again once the map has grown (#148), in one transaction still
    const u64 count = 48;
    TestLMDBObserver observer;
    LMDBStorage storage(SMALL_MAP);
    storage.load(testDir_, &observer);
    storage.beginGroup();
    for (u64 i = 1; i <= count; ++i)
    {
        ASSERT_EQ(0u, storage.update(IdT(i, 1), BIG_RECORD.data(), BIG_RECORD.size())) << "record " << i;
    }
    ASSERT_NO_THROW(storage.commitGroup());

    EXPECT_GE(storage.mapSize(), 4 * SMALL_MAP);
    for (u64 i = 1; i <= count; ++i)
    {
        EXPECT_TRUE(storage.isExists(IdT(i, 1), 0)) << "record " << i;
    }
}

TEST_F(LMDBStorageTest, AGroupUpdatesARecordOnceAndAThreadHasOneGroup)
{
    // A transaction persists an order once (Processor::persistOrder), so a second update in its group is a bug
    TestLMDBObserver observer;
    LMDBStorage storage;
    storage.load(testDir_, &observer);
    storage.beginGroup();
    EXPECT_THROW(storage.beginGroup(), std::logic_error);
    storage.update(IdT(1, 1), "a", 1);
    EXPECT_THROW(storage.update(IdT(1, 1), "b", 1), std::logic_error);
    storage.abortGroup();
    EXPECT_FALSE(storage.isExists(IdT(1, 1), 0));
}

} // namespace
