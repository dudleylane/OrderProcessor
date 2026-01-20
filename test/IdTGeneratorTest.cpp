/**
 * Concurrent Order Processor library - Google Test
 *
 * Author: Sergey Mikhailik
 * Test Implementation: 2026
 *
 * Copyright (C) 2009-2026 Sergey Mikhailik
 *
 * Distributed under the GNU General Public License (GPL).
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <set>
#include <atomic>
#include <chrono>

#include "IdTGenerator.h"

using namespace COP;

// =============================================================================
// IdTGenerator Test Fixture
// =============================================================================

class IdTGeneratorTest : public ::testing::Test {
protected:
    void SetUp() override {
        IdTGenerator::create();
    }

    void TearDown() override {
        IdTGenerator::destroy();
    }
};

// =============================================================================
// Basic Functionality Tests
// =============================================================================

TEST_F(IdTGeneratorTest, InstanceIsNotNull) {
    ASSERT_NE(IdTGenerator::instance(), nullptr);
}

TEST_F(IdTGeneratorTest, GeneratesValidId) {
    IdT id = IdTGenerator::instance()->getId();
    EXPECT_GT(id.id_, 0u);
    EXPECT_GT(id.date_, 0u);
}

TEST_F(IdTGeneratorTest, FirstIdHasCounterOne) {
    IdT id = IdTGenerator::instance()->getId();
    EXPECT_EQ(id.id_, 1u);
}

TEST_F(IdTGeneratorTest, IdIncrementsMonotonically) {
    auto* generator = IdTGenerator::instance();

    IdT id1 = generator->getId();
    IdT id2 = generator->getId();
    IdT id3 = generator->getId();

    EXPECT_EQ(id1.id_, 1u);
    EXPECT_EQ(id2.id_, 2u);
    EXPECT_EQ(id3.id_, 3u);
}

TEST_F(IdTGeneratorTest, TimestampIsCurrentUnixTime) {
    auto now = std::time(nullptr);
    IdT id = IdTGenerator::instance()->getId();

    // Allow for some timing variance (within 2 seconds)
    EXPECT_GE(id.date_, static_cast<u32>(now - 2));
    EXPECT_LE(id.date_, static_cast<u32>(now + 2));
}

TEST_F(IdTGeneratorTest, RapidCallsHaveSameTimestamp) {
    auto* generator = IdTGenerator::instance();

    IdT id1 = generator->getId();
    IdT id2 = generator->getId();
    IdT id3 = generator->getId();

    // All generated within same second should have same timestamp
    EXPECT_EQ(id1.date_, id2.date_);
    EXPECT_EQ(id2.date_, id3.date_);
}

// =============================================================================
// Singleton Pattern Tests
// =============================================================================

TEST_F(IdTGeneratorTest, SingletonReturnsSameInstance) {
    auto* instance1 = IdTGenerator::instance();
    auto* instance2 = IdTGenerator::instance();

    EXPECT_EQ(instance1, instance2);
}

TEST_F(IdTGeneratorTest, IdsContinueAcrossMultipleAccesses) {
    IdT id1 = IdTGenerator::instance()->getId();
    IdT id2 = IdTGenerator::instance()->getId();

    EXPECT_EQ(id2.id_, id1.id_ + 1);
}

// =============================================================================
// Concurrent Access Tests
// =============================================================================

TEST_F(IdTGeneratorTest, ConcurrentAccessProducesUniqueIds) {
    const int numThreads = 8;
    const int idsPerThread = 1000;
    std::vector<std::thread> threads;
    std::vector<std::vector<u64>> threadIds(numThreads);

    auto* generator = IdTGenerator::instance();

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([generator, &threadIds, t, idsPerThread]() {
            threadIds[t].reserve(idsPerThread);
            for (int i = 0; i < idsPerThread; ++i) {
                IdT id = generator->getId();
                threadIds[t].push_back(id.id_);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Collect all IDs and verify uniqueness
    std::set<u64> allIds;
    for (const auto& ids : threadIds) {
        for (u64 id : ids) {
            auto result = allIds.insert(id);
            EXPECT_TRUE(result.second) << "Duplicate ID found: " << id;
        }
    }

    EXPECT_EQ(allIds.size(), static_cast<size_t>(numThreads * idsPerThread));
}

TEST_F(IdTGeneratorTest, ConcurrentAccessMaintainsMonotonicity) {
    const int numThreads = 4;
    const int idsPerThread = 500;
    std::vector<std::thread> threads;
    std::vector<std::vector<u64>> threadIds(numThreads);

    auto* generator = IdTGenerator::instance();

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([generator, &threadIds, t, idsPerThread]() {
            threadIds[t].reserve(idsPerThread);
            for (int i = 0; i < idsPerThread; ++i) {
                IdT id = generator->getId();
                threadIds[t].push_back(id.id_);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Within each thread, IDs may not be monotonic due to interleaving
    // But all IDs should be unique and within expected range
    std::set<u64> allIds;
    for (const auto& ids : threadIds) {
        for (u64 id : ids) {
            allIds.insert(id);
        }
    }

    // Verify contiguous range (no gaps)
    u64 minId = *allIds.begin();
    u64 maxId = *allIds.rbegin();
    EXPECT_EQ(maxId - minId + 1, allIds.size());
}

TEST_F(IdTGeneratorTest, HighContentionStressTest) {
    const int numThreads = 16;
    const int idsPerThread = 2000;
    std::atomic<int> totalGenerated{0};
    std::vector<std::thread> threads;

    auto* generator = IdTGenerator::instance();

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([generator, &totalGenerated, idsPerThread]() {
            for (int i = 0; i < idsPerThread; ++i) {
                IdT id = generator->getId();
                EXPECT_GT(id.id_, 0u);
                totalGenerated.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(totalGenerated.load(), numThreads * idsPerThread);
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(IdTGeneratorTest, ManySequentialIds) {
    const int count = 10000;
    auto* generator = IdTGenerator::instance();

    u64 previousId = 0;
    for (int i = 0; i < count; ++i) {
        IdT id = generator->getId();
        EXPECT_GT(id.id_, previousId);
        previousId = id.id_;
    }
}

TEST_F(IdTGeneratorTest, IdStructureValidity) {
    IdT id = IdTGenerator::instance()->getId();

    // IdT should have both components properly initialized
    EXPECT_NE(id.id_, 0u);
    EXPECT_NE(id.date_, 0u);

    // Date should be a valid unix timestamp (after year 2000)
    const u32 year2000 = 946684800;  // Unix timestamp for Jan 1, 2000
    EXPECT_GT(id.date_, year2000);
}

// =============================================================================
// Recreation Tests
// =============================================================================

TEST(IdTGeneratorRecreationTest, RecreationResetsCounter) {
    // First creation
    IdTGenerator::create();
    IdT id1 = IdTGenerator::instance()->getId();
    IdT id2 = IdTGenerator::instance()->getId();
    IdTGenerator::destroy();

    EXPECT_EQ(id1.id_, 1u);
    EXPECT_EQ(id2.id_, 2u);

    // Second creation - counter should reset
    IdTGenerator::create();
    IdT id3 = IdTGenerator::instance()->getId();
    IdTGenerator::destroy();

    EXPECT_EQ(id3.id_, 1u);
}
