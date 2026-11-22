/**
 * Concurrent Order Processor library - CpuAffinity Tests
 *
 * Tests for CPU affinity pinning, ScopedCpuAffinity RAII, and utility
 * functions. Note: actual CPU pinning may require elevated privileges —
 * tests handle EPERM gracefully.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "CpuAffinity.h"

using namespace COP;

namespace
{

// =============================================================================
// CpuAffinity Tests
// =============================================================================

TEST(CpuAffinityTest, GetAvailableCoresReturnsPositive)
{
    int cores = CpuAffinity::getAvailableCores();
    EXPECT_GT(cores, 0);
}

TEST(CpuAffinityTest, GetThreadAffinityReturnsCores)
{
    std::vector<int> coreIds;
    bool success = CpuAffinity::getThreadAffinity(coreIds);
    EXPECT_TRUE(success);
    EXPECT_GT(coreIds.size(), 0u);

    // All core IDs should be non-negative
    for (int id : coreIds)
    {
        EXPECT_GE(id, 0);
    }
}

TEST(CpuAffinityTest, PinAndResetRoundTrip)
{
    // Save original affinity
    std::vector<int> original;
    CpuAffinity::getThreadAffinity(original);

    // Pin to core 0
    bool pinned = CpuAffinity::pinThreadToCore(0);
    if (pinned)
    {
        std::vector<int> afterPin;
        CpuAffinity::getThreadAffinity(afterPin);
        ASSERT_EQ(1u, afterPin.size());
        EXPECT_EQ(0, afterPin[0]);
    }
    // May fail without privileges — that's OK

    // Reset to all cores
    bool reset = CpuAffinity::resetAffinity();
    if (reset)
    {
        std::vector<int> afterReset;
        CpuAffinity::getThreadAffinity(afterReset);
        EXPECT_GE(afterReset.size(), 1u);
    }
}

TEST(CpuAffinityTest, PinToMultipleCores)
{
    std::vector<int> cores = { 0 };
    if (CpuAffinity::getAvailableCores() > 1)
    {
        cores.push_back(1);
    }

    bool success = CpuAffinity::pinThreadToCores(cores);
    // May fail without privileges
    if (success)
    {
        std::vector<int> actual;
        CpuAffinity::getThreadAffinity(actual);
        EXPECT_EQ(cores.size(), actual.size());
    }

    CpuAffinity::resetAffinity();
}

// =============================================================================
// ScopedCpuAffinity RAII Tests
// =============================================================================

TEST(ScopedCpuAffinityTest, RestoresAffinityOnDestruction)
{
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

TEST(ScopedCpuAffinityTest, ManualRestore)
{
    std::vector<int> before;
    CpuAffinity::getThreadAffinity(before);

    ScopedCpuAffinity guard(0);
    guard.restore();

    std::vector<int> after;
    CpuAffinity::getThreadAffinity(after);
    EXPECT_EQ(before.size(), after.size());
}

} // namespace
