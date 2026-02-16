/**
 * Concurrent Order Processor library - NumaAllocator Tests
 *
 * Tests for NUMA-aware memory allocation: node detection, allocateOnNode,
 * allocateLocal, deallocation, and the STL-compatible NumaNodeAllocator.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "NumaAllocator.h"

using namespace COP;

namespace {

// =============================================================================
// NUMA Topology Detection
// =============================================================================

TEST(NumaAllocatorTest, NodeCountReturnsAtLeastOne) {
    int count = NumaAllocator::nodeCount();
    EXPECT_GE(count, 1);
}

TEST(NumaAllocatorTest, IsNumaAvailableConsistentWithNodeCount) {
    bool available = NumaAllocator::isNumaAvailable();
    int count = NumaAllocator::nodeCount();

    if (count > 1) {
        EXPECT_TRUE(available);
    } else {
        EXPECT_FALSE(available);
    }
}

// =============================================================================
// allocateOnNode
// =============================================================================

TEST(NumaAllocatorTest, AllocateOnNodeReturnsValidMemory) {
    void* ptr = NumaAllocator::allocateOnNode(4096, 0);
    ASSERT_NE(nullptr, ptr);

    // Verify memory is writable
    std::memset(ptr, 0xAB, 4096);

    NumaAllocator::deallocate(ptr, 4096);
}

TEST(NumaAllocatorTest, AllocateOnNodeLargeAllocation) {
    const size_t size = 1024 * 1024; // 1MB
    void* ptr = NumaAllocator::allocateOnNode(size, 0);
    ASSERT_NE(nullptr, ptr);

    // Touch all pages
    std::memset(ptr, 0, size);

    NumaAllocator::deallocate(ptr, size);
}

TEST(NumaAllocatorTest, AllocateOnNodeSmallAllocation) {
    void* ptr = NumaAllocator::allocateOnNode(64, 0);
    ASSERT_NE(nullptr, ptr);

    std::memset(ptr, 0xFF, 64);

    NumaAllocator::deallocate(ptr, 64);
}

// =============================================================================
// allocateLocal
// =============================================================================

TEST(NumaAllocatorTest, AllocateLocalReturnsValidMemory) {
    void* ptr = NumaAllocator::allocateLocal(4096);
    ASSERT_NE(nullptr, ptr);

    std::memset(ptr, 0xCD, 4096);

    NumaAllocator::deallocate(ptr, 4096);
}

TEST(NumaAllocatorTest, AllocateLocalLargeAllocation) {
    const size_t size = 2 * 1024 * 1024; // 2MB
    void* ptr = NumaAllocator::allocateLocal(size);
    ASSERT_NE(nullptr, ptr);

    std::memset(ptr, 0, size);

    NumaAllocator::deallocate(ptr, size);
}

// =============================================================================
// Deallocation Safety
// =============================================================================

TEST(NumaAllocatorTest, DeallocateNullptrIsSafe) {
    NumaAllocator::deallocate(nullptr, 4096);
    // Should not crash
    SUCCEED();
}

TEST(NumaAllocatorTest, AllocateDeallocateRoundTrip) {
    for (int i = 0; i < 100; ++i) {
        void* ptr = NumaAllocator::allocateLocal(256);
        ASSERT_NE(nullptr, ptr);
        NumaAllocator::deallocate(ptr, 256);
    }
}

// =============================================================================
// NumaNodeAllocator<T> STL Compatibility
// =============================================================================

TEST(NumaNodeAllocatorTest, AllocateDeallocateInts) {
    NumaNodeAllocator<int> alloc(0);

    int* ptr = alloc.allocate(10);
    ASSERT_NE(nullptr, ptr);

    for (int i = 0; i < 10; ++i) {
        ptr[i] = i * 42;
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(i * 42, ptr[i]);
    }

    alloc.deallocate(ptr, 10);
}

TEST(NumaNodeAllocatorTest, NodeAccessor) {
    NumaNodeAllocator<double> alloc(0);
    EXPECT_EQ(0, alloc.node());

    NumaNodeAllocator<double> alloc1(1);
    EXPECT_EQ(1, alloc1.node());
}

TEST(NumaNodeAllocatorTest, EqualityBySameNode) {
    NumaNodeAllocator<int> a(0);
    NumaNodeAllocator<int> b(0);
    NumaNodeAllocator<int> c(1);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
}

TEST(NumaNodeAllocatorTest, UseWithStdVector) {
    NumaNodeAllocator<int> alloc(0);
    std::vector<int, NumaNodeAllocator<int>> vec(alloc);

    for (int i = 0; i < 1000; ++i) {
        vec.push_back(i);
    }

    ASSERT_EQ(1000u, vec.size());
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(i, vec[i]);
    }
}

TEST(NumaNodeAllocatorTest, CopyConstructFromDifferentType) {
    NumaNodeAllocator<int> intAlloc(0);
    NumaNodeAllocator<double> dblAlloc(intAlloc);

    EXPECT_EQ(0, dblAlloc.node());
}

} // namespace
