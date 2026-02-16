/**
 * Concurrent Order Processor library - CpuAffinity & HugePages Tests
 *
 * Tests for CPU affinity pinning, ScopedCpuAffinity RAII, HugePages
 * allocation with fallback, and utility functions.
 * Note: actual CPU pinning and huge page allocation may require
 * elevated privileges — tests handle EPERM gracefully.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "CpuAffinity.h"
#include "HugePages.h"

using namespace COP;

namespace {

// =============================================================================
// CpuAffinity Tests
// =============================================================================

TEST(CpuAffinityTest, GetAvailableCoresReturnsPositive) {
    int cores = CpuAffinity::getAvailableCores();
    EXPECT_GT(cores, 0);
}

TEST(CpuAffinityTest, GetThreadAffinityReturnsCores) {
    std::vector<int> coreIds;
    bool success = CpuAffinity::getThreadAffinity(coreIds);
    EXPECT_TRUE(success);
    EXPECT_GT(coreIds.size(), 0u);

    // All core IDs should be non-negative
    for (int id : coreIds) {
        EXPECT_GE(id, 0);
    }
}

TEST(CpuAffinityTest, PinAndResetRoundTrip) {
    // Save original affinity
    std::vector<int> original;
    CpuAffinity::getThreadAffinity(original);

    // Pin to core 0
    bool pinned = CpuAffinity::pinThreadToCore(0);
    if (pinned) {
        std::vector<int> afterPin;
        CpuAffinity::getThreadAffinity(afterPin);
        ASSERT_EQ(1u, afterPin.size());
        EXPECT_EQ(0, afterPin[0]);
    }
    // May fail without privileges — that's OK

    // Reset to all cores
    bool reset = CpuAffinity::resetAffinity();
    if (reset) {
        std::vector<int> afterReset;
        CpuAffinity::getThreadAffinity(afterReset);
        EXPECT_GE(afterReset.size(), 1u);
    }
}

TEST(CpuAffinityTest, PinToMultipleCores) {
    std::vector<int> cores = {0};
    if (CpuAffinity::getAvailableCores() > 1) {
        cores.push_back(1);
    }

    bool success = CpuAffinity::pinThreadToCores(cores);
    // May fail without privileges
    if (success) {
        std::vector<int> actual;
        CpuAffinity::getThreadAffinity(actual);
        EXPECT_EQ(cores.size(), actual.size());
    }

    CpuAffinity::resetAffinity();
}

// =============================================================================
// ScopedCpuAffinity RAII Tests
// =============================================================================

TEST(ScopedCpuAffinityTest, RestoresAffinityOnDestruction) {
    std::vector<int> before;
    CpuAffinity::getThreadAffinity(before);

    {
        ScopedCpuAffinity guard(0);
        // Inside scope: should be pinned to core 0 (if privileges allow)
    }

    std::vector<int> after;
    CpuAffinity::getThreadAffinity(after);

    // Affinity should be restored
    EXPECT_EQ(before.size(), after.size());
}

TEST(ScopedCpuAffinityTest, ManualRestore) {
    std::vector<int> before;
    CpuAffinity::getThreadAffinity(before);

    ScopedCpuAffinity guard(0);
    guard.restore();

    std::vector<int> after;
    CpuAffinity::getThreadAffinity(after);
    EXPECT_EQ(before.size(), after.size());
}

// =============================================================================
// HugePages Tests
// =============================================================================

TEST(HugePagesTest, RoundUpToHugePage) {
    EXPECT_EQ(HugePages::HUGE_PAGE_SIZE, HugePages::roundUpToHugePage(1));
    EXPECT_EQ(HugePages::HUGE_PAGE_SIZE, HugePages::roundUpToHugePage(HugePages::HUGE_PAGE_SIZE));
    EXPECT_EQ(2 * HugePages::HUGE_PAGE_SIZE, HugePages::roundUpToHugePage(HugePages::HUGE_PAGE_SIZE + 1));
    EXPECT_EQ(HugePages::HUGE_PAGE_SIZE, HugePages::roundUpToHugePage(4096));
}

TEST(HugePagesTest, AllocateWithFallback) {
    // Should always succeed — falls back to regular pages if huge pages unavailable
    void* ptr = HugePages::allocate(4096, true);
    ASSERT_NE(nullptr, ptr);

    std::memset(ptr, 0xAB, 4096);

    HugePages::deallocate(ptr, 4096);
}

TEST(HugePagesTest, DeallocateNullIsSafe) {
    HugePages::deallocate(nullptr, 4096);
    SUCCEED();
}

TEST(HugePagesTest, GetConfiguredCountReturnsValidValue) {
    int count = HugePages::getConfiguredCount();
    // Returns -1 on error, or >= 0 for the actual count
    EXPECT_GE(count, -1);
}

TEST(HugePagesTest, GetFreeCountReturnsValidValue) {
    int count = HugePages::getFreeCount();
    EXPECT_GE(count, -1);
}

TEST(HugePagesTest, IsAvailableDoesNotCrash) {
    // Just verify it doesn't crash — result depends on system config
    bool available = HugePages::isAvailable();
    (void)available;
    SUCCEED();
}

TEST(HugePagesTest, AllocateLargeRegion) {
    const size_t size = 4 * 1024 * 1024; // 4MB
    void* ptr = HugePages::allocate(size, true);
    ASSERT_NE(nullptr, ptr);

    // Touch all pages
    std::memset(ptr, 0, size);

    HugePages::deallocate(ptr, size);
}

TEST(HugePagesTest, AllocateWithoutFallback) {
    // Without fallback, may return nullptr if huge pages aren't configured
    void* ptr = HugePages::allocate(4096, false);
    if (ptr) {
        HugePages::deallocate(ptr, 4096);
    }
    // Either way, should not crash
    SUCCEED();
}

} // namespace
