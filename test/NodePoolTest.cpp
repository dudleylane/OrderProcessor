/**
 * Concurrent Order Processor library - NodePool Tests
 *
 * The pool that keeps a std::map's nodes together (#467): blocks of one chunk lie side by side, a freed block is
 * handed out next, blocks are aligned for their type, chunks grow to their limit, anything but one node comes from
 * the global heap, and a map's nodes, swapped or not, stay with the pool they came from.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "NodePool.h"
#include "TypesDef.h"

using namespace COP;

namespace
{

struct Node56
{
    char bytes_[56];
};

struct alignas(32) Aligned32
{
    char bytes_[40];
};

struct Node12
{
    char bytes_[12];
};

static_assert(!std::is_default_constructible_v<NodeAllocator<int>>, "a map is made with its pool, never without one");

TEST(NodePoolTest, TheBlocksOfAChunkLieSideBySide)
{
    NodePool pool(16, 16);
    NodeAllocator<Node56> alloc(&pool);
    std::vector<Node56 *> blocks;
    for (int i = 0; i < 16; ++i)
    {
        blocks.push_back(alloc.allocate(1));
    }
    EXPECT_EQ(56u, pool.blockSize());
    EXPECT_EQ(1u, pool.chunkCount());
    for (std::size_t i = 1; i < blocks.size(); ++i)
    {
        EXPECT_EQ(reinterpret_cast<char *>(blocks[0]) + i * 56, reinterpret_cast<char *>(blocks[i])) << i;
    }
    for (Node56 *block : blocks)
    {
        EXPECT_TRUE(pool.owns(block));
        alloc.deallocate(block, 1);
    }
}

TEST(NodePoolTest, AFreedBlockIsTheNextHandedOut)
{
    NodePool pool;
    NodeAllocator<Node56> alloc(&pool);
    Node56 *a = alloc.allocate(1);
    Node56 *b = alloc.allocate(1);
    alloc.deallocate(a, 1);
    Node56 *c = alloc.allocate(1);
    EXPECT_EQ(a, c);
    Node56 *d = alloc.allocate(1);
    EXPECT_NE(b, d);
    EXPECT_NE(a, d);
    EXPECT_EQ(1u, pool.chunkCount());
    alloc.deallocate(b, 1);
    alloc.deallocate(c, 1);
    alloc.deallocate(d, 1);
}

TEST(NodePoolTest, BlocksAreAlignedForTheirType)
{
    NodePool pool(4, 4);
    NodeAllocator<Aligned32> alloc(&pool);
    std::vector<Aligned32 *> blocks;
    for (int i = 0; i < 9; ++i)
    {
        blocks.push_back(alloc.allocate(1));
        EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(blocks.back()) % 32) << i;
    }
    EXPECT_EQ(64u, pool.blockSize());
    EXPECT_EQ(3u, pool.chunkCount());
    for (Aligned32 *block : blocks)
    {
        alloc.deallocate(block, 1);
    }
    // A freed block holds the free list's pointer, so a type aligned to 1 still gets blocks aligned for that
    NodePool bytes;
    NodeAllocator<Node12> bytesAlloc(&bytes);
    Node12 *first = bytesAlloc.allocate(1);
    Node12 *second = bytesAlloc.allocate(1);
    EXPECT_EQ(16u, bytes.blockSize());
    EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(second) % alignof(void *));
    bytesAlloc.deallocate(first, 1);
    bytesAlloc.deallocate(second, 1);
}

TEST(NodePoolTest, ChunksDoubleUpToTheirLimit)
{
    NodePool pool(2, 8);
    NodeAllocator<Node56> alloc(&pool);
    std::vector<Node56 *> blocks;
    // 2, 4, 8 and then 8 again
    const std::size_t chunks[] = { 2, 4, 8, 8 };
    std::size_t total = 0;
    for (std::size_t c = 0; c < 4; ++c)
    {
        for (std::size_t i = 0; i < chunks[c]; ++i)
        {
            blocks.push_back(alloc.allocate(1));
            if (0 < i)
            {
                // Within a chunk, each block follows the one before it
                EXPECT_EQ(reinterpret_cast<char *>(blocks[total + i - 1]) + 56,
                          reinterpret_cast<char *>(blocks.back()));
            }
        }
        total += chunks[c];
        EXPECT_EQ(c + 1, pool.chunkCount());
    }
    for (Node56 *block : blocks)
    {
        EXPECT_TRUE(pool.owns(block));
        alloc.deallocate(block, 1);
    }
}

TEST(NodePoolTest, AnythingButOneNodeComesFromTheHeap)
{
    NodePool pool;
    NodeAllocator<Node56> alloc(&pool);
    // An array isn't a node: it never touches the pool
    Node56 *array = alloc.allocate(3);
    EXPECT_EQ(0u, pool.blockSize());
    EXPECT_EQ(0u, pool.chunkCount());
    Node56 *node = alloc.allocate(1);
    EXPECT_TRUE(pool.owns(node));
    EXPECT_FALSE(pool.owns(array));
    // Nor does what a block can't hold, once the first allocation has set the blocks' size
    NodeAllocator<Aligned32> wider(&pool);
    Aligned32 *aligned = wider.allocate(1);
    EXPECT_FALSE(pool.owns(aligned));
    wider.deallocate(aligned, 1);
    alloc.deallocate(node, 1);
    alloc.deallocate(array, 3);
    EXPECT_EQ(1u, pool.chunkCount());
}

TEST(NodePoolTest, AMapsNodesComeFromItsPool)
{
    NodePool pool;
    PooledMap<IdT, int> map{ PooledMap<IdT, int>::allocator_type(&pool) };
    for (u64 i = 1; i <= 1000; ++i)
    {
        map.emplace(IdT(i, 20261030), static_cast<int>(i));
    }
    EXPECT_LT(0u, pool.chunkCount());
    for (const auto &entry : map)
    {
        EXPECT_TRUE(pool.owns(&entry)) << entry.first.id_;
    }
    // A node erased is the next one handed out
    const auto *erased = &*map.find(IdT(500, 20261030));
    map.erase(IdT(500, 20261030));
    map.emplace(IdT(1001, 20261030), 1001);
    EXPECT_EQ(erased, &*map.find(IdT(1001, 20261030)));
}

TEST(NodePoolTest, ASwapHandsThePoolOverWithTheNodes)
{
    NodePool poolA;
    NodePool poolB;
    PooledMap<IdT, int> a{ PooledMap<IdT, int>::allocator_type(&poolA) };
    PooledMap<IdT, int> b{ PooledMap<IdT, int>::allocator_type(&poolB) };
    a.emplace(IdT(1, 1), 1);
    b.emplace(IdT(2, 1), 2);
    std::swap(a, b);
    EXPECT_EQ(&poolB, a.get_allocator().pool());
    EXPECT_EQ(&poolA, b.get_allocator().pool());
    EXPECT_TRUE(poolB.owns(&*a.begin()));
    EXPECT_TRUE(poolA.owns(&*b.begin()));
    // What each map adds now comes from the pool it took over
    a.emplace(IdT(3, 1), 3);
    EXPECT_TRUE(poolB.owns(&*a.find(IdT(3, 1))));
}

TEST(NodePoolTest, ATemporaryMadeFromAMapsAllocatorSharesItsPool)
{
    // The stores' destructors take their maps' nodes into a temporary under the write lock, and free them after it
    NodePool pool;
    PooledMap<IdT, int> map{ PooledMap<IdT, int>::allocator_type(&pool) };
    map.emplace(IdT(1, 1), 1);
    const auto *node = &*map.begin();
    PooledMap<IdT, int> tmp(map.get_allocator());
    std::swap(tmp, map);
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(node, &*tmp.begin());
    EXPECT_EQ(&pool, tmp.get_allocator().pool());
    tmp.clear();
    // Freed into the pool it came from
    map.emplace(IdT(2, 1), 2);
    EXPECT_EQ(node, &*map.begin());
}

} // namespace
