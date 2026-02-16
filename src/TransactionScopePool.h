/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <memory>
#include <cassert>

// Portable spin-wait hint: reduces power consumption and avoids
// memory-order pipeline stalls during CAS retry loops.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #include <immintrin.h>
  inline void cpu_pause() noexcept { _mm_pause(); }
#elif defined(__aarch64__) || defined(_M_ARM64)
  inline void cpu_pause() noexcept { asm volatile("yield" ::: "memory"); }
#else
  inline void cpu_pause() noexcept { /* no-op fallback */ }
#endif

#include "TransactionScope.h"
#include "HugePages.h"
#include "NumaAllocator.h"

namespace COP {
namespace ACID {

/**
 * Lock-free object pool for TransactionScope objects.
 *
 * Eliminates heap allocations from the hot path by pre-allocating
 * TransactionScope objects and reusing them via acquire()/release().
 *
 * Implementation uses a lock-free circular buffer with CAS operations
 * and exponential backoff to reduce cache-line contention.
 * Thread-safe for concurrent acquire/release from multiple threads.
 */
class TransactionScopePool {
public:
    static constexpr size_t DEFAULT_POOL_SIZE = 1024;
    static constexpr size_t INVALID_INDEX = SIZE_MAX;

    enum AllocMode { ALLOC_DEFAULT = 0, ALLOC_HUGE_PAGES = 1, ALLOC_NUMA_LOCAL = 2 };

    explicit TransactionScopePool(size_t poolSize = DEFAULT_POOL_SIZE, AllocMode mode = ALLOC_DEFAULT)
        : poolSize_(poolSize)
        , pool_(nullptr)
        , allocMode_(ALLOC_DEFAULT)
        , head_(0)
        , cacheMisses_(0)
    {
        const size_t allocBytes = poolSize * sizeof(Node);

        // Try requested allocation strategy, falling back to default on failure
        if (mode == ALLOC_HUGE_PAGES) {
            void* mem = HugePages::allocate(allocBytes);
            if (mem) {
                pool_ = static_cast<Node*>(mem);
                for (size_t i = 0; i < poolSize_; ++i) {
                    new (&pool_[i]) Node();
                }
                allocMode_ = ALLOC_HUGE_PAGES;
            }
        } else if (mode == ALLOC_NUMA_LOCAL) {
            void* mem = NumaAllocator::allocateLocal(allocBytes);
            if (mem) {
                pool_ = static_cast<Node*>(mem);
                for (size_t i = 0; i < poolSize_; ++i) {
                    new (&pool_[i]) Node();
                }
                allocMode_ = ALLOC_NUMA_LOCAL;
            }
        }
        if (!pool_) {
            pool_ = new Node[poolSize];
        }

        // Pre-allocate all TransactionScope objects
        for (size_t i = 0; i < poolSize_; ++i) {
            pool_[i].scope = new TransactionScope();
            pool_[i].inUse.store(false, std::memory_order_relaxed);
        }
    }

    ~TransactionScopePool() {
        for (size_t i = 0; i < poolSize_; ++i) {
            delete pool_[i].scope;
        }
        if (allocMode_ != ALLOC_DEFAULT) {
            for (size_t i = 0; i < poolSize_; ++i) {
                pool_[i].~Node();
            }
            if (allocMode_ == ALLOC_HUGE_PAGES) {
                HugePages::deallocate(pool_, poolSize_ * sizeof(Node));
            } else {
                NumaAllocator::deallocate(pool_, poolSize_ * sizeof(Node));
            }
        } else {
            delete[] pool_;
        }
    }

    /**
     * Acquire a TransactionScope from the pool.
     * Returns the scope pointer and the pool index (INVALID_INDEX if heap-allocated).
     * The returned scope is reset and ready for use.
     *
     * Uses exponential backoff with _mm_pause() to reduce contention.
     */
    TransactionScope* acquire(size_t& outIndex) {
        size_t attempts = 0;
        int backoff = 1;
        while (attempts < poolSize_) {
            size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % poolSize_;
            bool expected = false;
            if (pool_[idx].inUse.compare_exchange_strong(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) [[likely]] {
                // Successfully acquired a slot — lazily replenish if detached
                if (!pool_[idx].scope) {
                    pool_[idx].scope = new TransactionScope();
                } else {
                    pool_[idx].scope->reset();
                }
                outIndex = idx;
                return pool_[idx].scope;
            }
            // Exponential backoff to reduce cache-line bouncing
            for (int i = 0; i < backoff; ++i) {
                cpu_pause();
            }
            backoff = std::min(backoff * 2, 16);
            ++attempts;
        }

        // Pool exhausted — fall back to heap allocation
        cacheMisses_.fetch_add(1, std::memory_order_relaxed);
        outIndex = INVALID_INDEX;
        return new TransactionScope();
    }

    /**
     * Release a pool slot by index — O(1).
     * The scope remains in the pool for reuse.
     */
    void releaseByIndex(size_t index) {
        assert(index < poolSize_);
        pool_[index].inUse.store(false, std::memory_order_release);
    }

    /**
     * Detach a scope from its pool slot and return it for external ownership.
     * The pool slot is marked empty and will be lazily replenished on next acquire.
     * This avoids the second heap allocation previously needed in release().
     */
    TransactionScope* detach(size_t index) {
        assert(index < poolSize_);
        TransactionScope* scope = pool_[index].scope;
        pool_[index].scope = nullptr;  // Pool will lazily allocate replacement
        pool_[index].inUse.store(false, std::memory_order_release);
        return scope;
    }

    size_t cacheMisses() const noexcept {
        return cacheMisses_.load(std::memory_order_relaxed);
    }

    size_t poolSize() const noexcept {
        return poolSize_;
    }

private:
    struct Node {
        TransactionScope* scope;
        std::atomic<bool> inUse;

        Node() : scope(nullptr) { inUse.store(false, std::memory_order_relaxed); }
    };

    size_t poolSize_;
    Node* pool_;
    AllocMode allocMode_;

    // Cache-line aligned to prevent false sharing
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> cacheMisses_;

    TransactionScopePool(const TransactionScopePool&) = delete;
    TransactionScopePool& operator=(const TransactionScopePool&) = delete;
};

/**
 * RAII wrapper for TransactionScope acquired from a pool.
 * Tracks pool index for O(1) release back to the pool.
 *
 * On destruction (no release() called): scope is returned to the pool.
 * On release(): scope is detached from the pool and given to the caller
 * without a second heap allocation.
 */
class PooledTransactionScope {
public:
    explicit PooledTransactionScope(TransactionScopePool* pool)
        : pool_(pool)
        , poolIndex_(TransactionScopePool::INVALID_INDEX)
    {
        if (pool_) {
            scope_ = pool_->acquire(poolIndex_);
        } else {
            scope_ = new TransactionScope();
        }
    }

    ~PooledTransactionScope() {
        if (scope_ == nullptr) {
            return; // Already released
        }
        if (pool_ && poolIndex_ != TransactionScopePool::INVALID_INDEX) {
            pool_->releaseByIndex(poolIndex_);
        } else {
            delete scope_;
        }
    }

    TransactionScope* get() const noexcept { return scope_; }
    TransactionScope* operator->() const noexcept { return scope_; }
    TransactionScope& operator*() const noexcept { return *scope_; }

    /**
     * Release ownership for external use (e.g., TransactionManager).
     * If pooled: detaches the scope from its pool slot (zero allocation).
     * If heap-allocated: transfers ownership directly.
     * Caller takes full ownership of the returned pointer.
     */
    TransactionScope* release() {
        if (scope_ == nullptr) {
            return nullptr;
        }

        TransactionScope* result;

        if (pool_ && poolIndex_ != TransactionScopePool::INVALID_INDEX) {
            // Detach from pool — no second allocation needed.
            // Pool slot is freed and will lazily replenish on next acquire.
            result = pool_->detach(poolIndex_);
        } else {
            result = scope_;
        }

        scope_ = nullptr;
        pool_ = nullptr;
        poolIndex_ = TransactionScopePool::INVALID_INDEX;
        return result;
    }

private:
    TransactionScopePool* pool_;
    TransactionScope* scope_;
    size_t poolIndex_;

    PooledTransactionScope(const PooledTransactionScope&) = delete;
    PooledTransactionScope& operator=(const PooledTransactionScope&) = delete;
};

// Compile-time validation: pool size must be large enough to be useful
static_assert(TransactionScopePool::DEFAULT_POOL_SIZE >= 64,
              "TransactionScopePool default size must be at least 64 for effective concurrency");

} // namespace ACID
} // namespace COP
