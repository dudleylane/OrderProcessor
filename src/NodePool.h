/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace COP
{

/**
 * Blocks of one size, for one std::map's nodes (#467).
 *
 * From the global heap, each node lay among whatever was allocated around it, the orders among them, so how a map's
 * nodes lay, and how many cache lines a walk touched, moved with an order's size. A pool carves its blocks from chunks
 * of its own, in the order they are asked for, so a map's nodes lie together whatever lies between the objects they
 * point to.
 *
 * The first allocation sets the block size. A freed block goes on a free list and is the next one handed out; the
 * chunks go back only when the pool is destroyed, so a pool must outlive the map that uses it: declare it first.
 * AddressSanitizer sees a chunk as one allocation, so it can't catch a node used after the map freed it.
 *
 * Not synchronised: every allocation and deallocation must run under the lock that guards changes to the map, as the
 * stores' write locks do.
 */
class NodePool
{
public:
    explicit NodePool(std::size_t firstChunkBlocks = 64, std::size_t maxChunkBlocks = 8192) noexcept
        : nextChunkBlocks_(firstChunkBlocks), maxChunkBlocks_(maxChunkBlocks)
    {
        assert((0 < firstChunkBlocks) && (firstChunkBlocks <= maxChunkBlocks));
    }
    ~NodePool()
    {
        for (const Chunk &chunk : chunks_)
        {
            ::operator delete(chunk.begin_, std::align_val_t(blockAlign_));
        }
    }
    NodePool(const NodePool &) = delete;
    NodePool &operator=(const NodePool &) = delete;

    /// Whether a block holds an object of this size and alignment. Any does before the first allocation, which sets
    /// the blocks' size and alignment from its own.
    bool fits(std::size_t size, std::size_t align) const noexcept
    {
        return (0 == blockSize_) || ((size <= blockSize_) && (align <= blockAlign_));
    }

    void *allocate(std::size_t size, std::size_t align)
    {
        assert(fits(size, align));
        if (0 == blockSize_) [[unlikely]]
        {
            blockAlign_ = std::max(align, alignof(FreeBlock));
            blockSize_ = (std::max(size, sizeof(FreeBlock)) + blockAlign_ - 1) / blockAlign_ * blockAlign_;
        }
        if (nullptr != free_)
        {
            FreeBlock *block = free_;
            free_ = block->next_;
            return block;
        }
        if (next_ == end_) [[unlikely]]
        {
            addChunk();
        }
        char *block = next_;
        next_ += blockSize_;
        return block;
    }

    void deallocate(void *block) noexcept
    {
        assert(nullptr != block);
        free_ = ::new (block) FreeBlock{ free_ };
    }

    std::size_t blockSize() const noexcept
    {
        return blockSize_;
    }
    std::size_t chunkCount() const noexcept
    {
        return chunks_.size();
    }
    /// Whether the block lies in one of the pool's chunks
    bool owns(const void *block) const noexcept
    {
        const char *p = static_cast<const char *>(block);
        return std::any_of(chunks_.begin(), chunks_.end(),
                           [p](const Chunk &chunk)
                           {
                               return (chunk.begin_ <= p) && (p < chunk.end_);
                           });
    }

private:
    struct FreeBlock
    {
        FreeBlock *next_;
    };
    struct Chunk
    {
        char *begin_;
        char *end_;
    };

    void addChunk()
    {
        const std::size_t bytes = nextChunkBlocks_ * blockSize_;
        chunks_.reserve(chunks_.size() + 1); // so that once the chunk is allocated, nothing can throw and leak it
        char *chunk = static_cast<char *>(::operator new(bytes, std::align_val_t(blockAlign_)));
        chunks_.push_back(Chunk{ chunk, chunk + bytes });
        next_ = chunk;
        end_ = chunk + bytes;
        nextChunkBlocks_ = std::min(nextChunkBlocks_ * 2, maxChunkBlocks_);
    }

    std::size_t blockSize_ = 0;
    std::size_t blockAlign_ = alignof(FreeBlock);
    std::size_t nextChunkBlocks_;
    std::size_t maxChunkBlocks_;
    FreeBlock *free_ = nullptr;
    char *next_ = nullptr;
    char *end_ = nullptr;
    std::vector<Chunk> chunks_;
};

/**
 * The allocator that puts a std::map's nodes in a NodePool (#467). A map asks for one node at a time; anything else,
 * or anything a block can't hold, comes from the global heap as before, and goes back there.
 *
 * A copy uses the same pool, and a swap or an assignment hands the allocator over with the nodes (all three
 * propagate), so a node always goes back to the pool it came from. There is no default: a map made without its pool,
 * such as a temporary to swap with, doesn't compile, where with a default its nodes could go back to another pool.
 */
template <typename T> class NodeAllocator
{
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    explicit NodeAllocator(NodePool *pool) noexcept : pool_(pool)
    {
        assert(nullptr != pool_);
    }
    template <typename U> NodeAllocator(const NodeAllocator<U> &other) noexcept : pool_(other.pool()) {}

    NodePool *pool() const noexcept
    {
        return pool_;
    }

    T *allocate(std::size_t n)
    {
        if ((1 == n) && pool_->fits(sizeof(T), alignof(T))) [[likely]]
        {
            return static_cast<T *>(pool_->allocate(sizeof(T), alignof(T)));
        }
        return std::allocator<T>().allocate(n);
    }

    void deallocate(T *ptr, std::size_t n) noexcept
    {
        if ((1 == n) && pool_->fits(sizeof(T), alignof(T))) [[likely]]
        {
            pool_->deallocate(ptr);
            return;
        }
        std::allocator<T>().deallocate(ptr, n);
    }

    template <typename U> bool operator==(const NodeAllocator<U> &other) const noexcept
    {
        return pool_ == other.pool();
    }
    template <typename U> bool operator!=(const NodeAllocator<U> &other) const noexcept
    {
        return pool_ != other.pool();
    }

private:
    NodePool *pool_;
};

/// A std::map whose nodes come from a NodePool, given to its constructor (#467)
template <typename K, typename V, typename Compare = std::less<K>>
using PooledMap = std::map<K, V, Compare, NodeAllocator<std::pair<const K, V>>>;

} // namespace COP
