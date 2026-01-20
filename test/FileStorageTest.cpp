/**
 Concurrent Order Processor library - Google Test Migration

 Original Author: Sergey Mikhailik
 Test Migration: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

#include "FileStorage.h"
#include "MockStorage.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::Store;
using test::DummyOrderSaver;

namespace {

// =============================================================================
// Test Observer Implementation
// =============================================================================

class TestFileStorageObserver : public FileStorageObserver {
public:
    TestFileStorageObserver() : finished_(false) {}

    void startLoad() override {
        finished_ = false;
    }

    void onRecordLoaded(const IdT& /*id*/, u32 /*version*/, const char* ptr, size_t s) override {
        ASSERT_NE(nullptr, ptr);
        ASSERT_GT(s, 0u);
        records_.push_back(std::string(ptr, s));
    }

    void finishLoad() override {
        finished_ = true;
    }

    void reset() {
        finished_ = false;
        records_.clear();
    }

public:
    bool finished_;
    std::vector<std::string> records_;
};

// =============================================================================
// Test File Helpers
// =============================================================================

static char VERSION_RECORD[] = "<Sxxxx::Version 0.0.0.1E>";
static const int VERSION_SIZE = 25;
static const int VERSION_BUFFER_SIZE = 64;

void prepareTestFile(const std::string& fileName) {
    std::memcpy(&(VERSION_RECORD[2]), &VERSION_SIZE, sizeof(VERSION_SIZE));

    FILE* f = std::fopen(fileName.c_str(), "w+");
    ASSERT_NE(nullptr, f);
    std::fwrite(VERSION_RECORD, 1, VERSION_SIZE, f);

    char buf[256];
    {
        int s = 0;
        std::memcpy(buf, "<S.....Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::valid vesion 0000E>", 21); s += 21;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }
    std::fclose(f);
}

void prepareBrokenTestFile(const std::string& fileName) {
    std::memcpy(&(VERSION_RECORD[2]), &VERSION_SIZE, sizeof(VERSION_SIZE));

    FILE* f = std::fopen(fileName.c_str(), "w+");
    ASSERT_NE(nullptr, f);
    std::fwrite(VERSION_RECORD, 1, VERSION_SIZE, f);

    char buf[256];
    // Valid record first
    {
        int s = 0;
        std::memcpy(buf, "<S.....Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::valid vesion 0000E>", 21); s += 21;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }
    // Invalid beginning of record (starts with '.' instead of '<')
    {
        int s = 0;
        std::memcpy(buf, ".S.....Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::invalid record 01E>", 21); s += 21;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }
    // Invalid beginning of record (no 'S' marker)
    {
        int s = 0;
        std::memcpy(buf, "<......Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::invalid record 02E>", 21); s += 21;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }
    // Invalid record size
    {
        int s = 0;
        std::memcpy(buf, "<S.....Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::invalid record 03E>", 21); s += 40;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }
    // Invalid end of record
    {
        int s = 0;
        std::memcpy(buf, "<S.....Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::invalid record 01.>", 21); s += 21;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }
    // Huge invalid record
    {
        char hugeBuf[3 * 64 * 1024];
        std::memset(hugeBuf, '.', 3 * 64 * 1024);
        std::fwrite(hugeBuf, 1, 3 * 63 * 1024 + 113, f);
    }
    // Valid record at end
    {
        int s = 0;
        std::memcpy(buf, "<S.....Y.", 10); s += 9;
        IdT id(1, 2);
        std::memcpy(&buf[s], &id, sizeof(id)); s += sizeof(id);
        buf[s] = '.'; s++;
        int ver = 0;
        std::memcpy(&buf[s], &ver, sizeof(ver)); s += sizeof(ver);
        std::memcpy(&buf[s], "::last record 00000E>", 21); s += 21;
        std::memcpy(&buf[2], &s, 4);
        std::fwrite(buf, 1, s, f);
    }

    std::fclose(f);
}

void cleanupTestFile(const std::string& fileName) {
    std::remove(fileName.c_str());
}

// =============================================================================
// Test Fixture
// =============================================================================

class FileStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        FileStorage::init();
        testFile_ = "testStorage.dat";
        brokenFile_ = "testBrokenStorage.dat";
    }

    void TearDown() override {
        cleanupTestFile(testFile_);
        cleanupTestFile(brokenFile_);
    }

protected:
    std::string testFile_;
    std::string brokenFile_;
};

// =============================================================================
// Basic Initialization Tests
// =============================================================================

TEST_F(FileStorageTest, CreateDefaultFileStorage) {
    FileStorage storage;
    SUCCEED();
}

TEST_F(FileStorageTest, CreateWithNonExistentFile) {
    cleanupTestFile(testFile_);
    TestFileStorageObserver observer;
    EXPECT_FALSE(observer.finished_);

    FileStorage storage(testFile_, &observer);

    EXPECT_TRUE(observer.finished_);
}

// =============================================================================
// Load Tests
// =============================================================================

TEST_F(FileStorageTest, LoadValidFile) {
    prepareTestFile(testFile_);
    TestFileStorageObserver observer;

    FileStorage storage(testFile_, &observer);

    EXPECT_TRUE(observer.finished_);
    ASSERT_EQ(1u, observer.records_.size());
    EXPECT_EQ("valid vesion 0000", observer.records_.at(0));
}

TEST_F(FileStorageTest, LoadBrokenFileSkipsInvalidRecords) {
    cleanupTestFile(brokenFile_);
    prepareBrokenTestFile(brokenFile_);
    TestFileStorageObserver observer;

    FileStorage storage(brokenFile_, &observer);

    EXPECT_TRUE(observer.finished_);
    ASSERT_EQ(2u, observer.records_.size());
    EXPECT_EQ("valid vesion 0000", observer.records_.at(0));
    EXPECT_EQ("last record 00000", observer.records_.at(1));
}

// =============================================================================
// Save Tests
// =============================================================================

TEST_F(FileStorageTest, SaveRecord) {
    cleanupTestFile(testFile_);
    TestFileStorageObserver observer;
    FileStorage storage(testFile_, &observer);

    EXPECT_TRUE(observer.finished_);
    EXPECT_EQ(0u, observer.records_.size());

    storage.save(IdT(1, 1), "aaaa", 4);
    SUCCEED();
}

TEST_F(FileStorageTest, SaveDuplicateIdThrows) {
    cleanupTestFile(testFile_);
    TestFileStorageObserver observer;
    FileStorage storage(testFile_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_THROW(storage.save(IdT(1, 1), "dubRec", 6), std::exception);
}

TEST_F(FileStorageTest, SaveWithAutoId) {
    cleanupTestFile(testFile_);
    TestFileStorageObserver observer;
    FileStorage storage(testFile_, &observer);

    IdT id = storage.save("bbbb", 4);

    EXPECT_THROW(storage.save(id, "dubRec2", 7), std::exception);
}

TEST_F(FileStorageTest, SavedRecordsArePersisted) {
    cleanupTestFile(testFile_);

    // Save some records
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);
        storage.save("bbbb", 4);
    }

    // Reload and verify
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);

        EXPECT_TRUE(observer.finished_);
        ASSERT_EQ(2u, observer.records_.size());
        EXPECT_EQ("aaaa", observer.records_.at(0));
        EXPECT_EQ("bbbb", observer.records_.at(1));
    }
}

// =============================================================================
// Erase Tests
// =============================================================================

TEST_F(FileStorageTest, EraseRecord) {
    cleanupTestFile(testFile_);

    // Save records
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);
        storage.save("bbbb", 4);

        // Erase the first record
        storage.erase(IdT(1, 1), 1);
        storage.erase(IdT(1, 1), 0);
        storage.erase(IdT(1, 1));
    }

    // Reload and verify
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);

        EXPECT_TRUE(observer.finished_);
        ASSERT_EQ(1u, observer.records_.size());
        EXPECT_EQ("bbbb", observer.records_.at(0));
    }
}

// =============================================================================
// Update Tests
// =============================================================================

TEST_F(FileStorageTest, UpdateRecord) {
    cleanupTestFile(testFile_);

    // Save and update
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);

        u32 result = storage.update(IdT(1, 1), "aaaaa", 5);
        EXPECT_EQ(1u, result);
    }

    // Reload and verify
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);

        EXPECT_TRUE(observer.finished_);
        ASSERT_EQ(2u, observer.records_.size());
        EXPECT_EQ("aaaa", observer.records_.at(0));
        EXPECT_EQ("aaaaa", observer.records_.at(1));
    }
}

// =============================================================================
// Replace Tests
// =============================================================================

TEST_F(FileStorageTest, ReplaceNonExistentThrows) {
    cleanupTestFile(testFile_);
    TestFileStorageObserver observer;
    FileStorage storage(testFile_, &observer);

    EXPECT_THROW(storage.replace(IdT(2, 1), 1, "aaaaa", 5), std::exception);
}

TEST_F(FileStorageTest, ReplaceWrongVersionThrows) {
    cleanupTestFile(testFile_);
    TestFileStorageObserver observer;
    FileStorage storage(testFile_, &observer);

    storage.save(IdT(1, 1), "aaaa", 4);

    EXPECT_THROW(storage.replace(IdT(1, 1), 1, "aaaaa", 5), std::exception);
}

TEST_F(FileStorageTest, ReplaceCorrectVersion) {
    cleanupTestFile(testFile_);

    // Save and replace
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);
        storage.save(IdT(1, 1), "aaaa", 4);

        u32 result = storage.replace(IdT(1, 1), 0, "aaaaa", 5);
        EXPECT_EQ(1u, result);
    }

    // Reload and verify
    {
        TestFileStorageObserver observer;
        FileStorage storage(testFile_, &observer);

        EXPECT_TRUE(observer.finished_);
        ASSERT_EQ(1u, observer.records_.size());
        EXPECT_EQ("aaaaa", observer.records_.at(0));
    }
}

} // namespace
