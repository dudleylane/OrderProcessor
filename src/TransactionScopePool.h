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
#include "TransactionScope.h"

namespace COP {
namespace ACID {

/**
 * Lock-free object pool for TransactionScope objects.
 *
 * This pool eliminates heap allocations from the hot path by pre-allocating
 * TransactionScope objects and reusing them via acquire()/release().
 *
 * Implementation uses a lock-free circular buffer with CAS operations.
 * Thread-safe for concurrent acquire/release from multiple threads.
 */
class TransactionScopePool {
public:
    static constexpr size_t DEFAULT_POOL_SIZE = 1024;

    explicit TransactionScopePool(size_t poolSize = DEFAULT_POOL_SIZE)
        : poolSize_(poolSize)
        , pool_(new Node[poolSize])
        , head_(0)
        , cacheMisses_(0)
    {
        // Pre-allocate all TransactionScope objects
        for (size_t i = 0; i < poolSize_; ++i) {
            pool_[i].scope = new TransactionScope();
            pool_[i].inUse.store(false, std::memory_order_relaxed);
        }
    }

    ~TransactionScopePool() {
        // Clean up all allocated scopes
        for (size_t i = 0; i < poolSize_; ++i) {
            delete pool_[i].scope;
        }
        delete[] pool_;
    }

    /**
     * Acquire a TransactionScope from the pool.
     * If pool is exhausted, falls back to heap allocation.
     * The returned scope is reset and ready for use.
     */
    TransactionScope* acquire() {
        // Try to find a free slot using CAS
        size_t attempts = 0;
        while (attempts < poolSize_) {
            size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % poolSize_;
            bool expected = false;
            if (pool_[idx].inUse.compare_exchange_strong(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                // Successfully acquired a slot
                pool_[idx].scope->reset();
                return pool_[idx].scope;
            }
            ++attempts;
        }

        // Pool exhausted - fall back to heap allocation
        cacheMisses_.fetch_add(1, std::memory_order_relaxed);
        return new TransactionScope();
    }

    /**
     * Release a TransactionScope back to the pool.
     * If the scope was heap-allocated (not from pool), it is deleted.
     */
    void release(TransactionScope* scope) {
        if (scope == nullptr) return;

        // Check if this scope belongs to our pool
        for (size_t i = 0; i < poolSize_; ++i) {
            if (pool_[i].scope == scope) {
                // Return to pool
                pool_[i].inUse.store(false, std::memory_order_release);
                return;
            }
        }

        // Not from our pool - must be heap-allocated, delete it
        delete scope;
    }

    /**
     * Returns the number of cache misses (heap allocations due to pool exhaustion).
     */
    size_t cacheMisses() const {
        return cacheMisses_.load(std::memory_order_relaxed);
    }

    /**
     * Returns the pool size.
     */
    size_t poolSize() const {
        return poolSize_;
    }

private:
    struct Node {
        TransactionScope* scope;
        std::atomic<bool> inUse;

        Node() : scope(nullptr) { inUse.store(false); }
    };

    size_t poolSize_;
    Node* pool_;

    // Cache-line aligned to prevent false sharing
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> cacheMisses_;

    // Non-copyable
    TransactionScopePool(const TransactionScopePool&) = delete;
    TransactionScopePool& operator=(const TransactionScopePool&) = delete;
};

/**
 * RAII wrapper for TransactionScope acquired from a pool.
 * Automatically releases the scope back to the pool on destruction.
 *
 * When release() is called, the pooled scope is returned to the pool
 * and a new heap-allocated scope is created with the same data.
 * This ensures pooled objects are never deleted externally.
 */
class PooledTransactionScope {
public:
    PooledTransactionScope(TransactionScopePool* pool)
        : pool_(pool)
        , scope_(pool ? pool->acquire() : new TransactionScope())
        , heapAllocated_(!pool)
    {
    }

    ~PooledTransactionScope() {
        if (scope_ == nullptr) {
            return; // Already released
        }
        if (pool_ && !heapAllocated_) {
            pool_->release(scope_);
        } else {
            delete scope_;
        }
    }

    TransactionScope* get() const { return scope_; }
    TransactionScope* operator->() const { return scope_; }
    TransactionScope& operator*() const { return *scope_; }

    /**
     * Release ownership and return a heap-allocated scope that can be safely deleted.
     * The pooled scope is returned to the pool, and a new scope is allocated on the heap.
     * Caller takes full ownership of the returned scope.
     */
    TransactionScope* release() {
        if (scope_ == nullptr) {
            return nullptr;
        }

        TransactionScope* result;

        if (pool_ && !heapAllocated_) {
            // Create a new heap-allocated scope for external ownership
            result = new TransactionScope();

            // Transfer operations from pooled scope to heap scope
            // The pooled scope's operations and state are moved to the new scope
            result->swap(*scope_);

            // Return pooled scope to the pool (it's been reset via swap)
            pool_->release(scope_);
        } else {
            // Scope was heap-allocated, just transfer ownership
            result = scope_;
        }

        scope_ = nullptr;
        pool_ = nullptr;
        heapAllocated_ = true;
        return result;
    }

private:
    TransactionScopePool* pool_;
    TransactionScope* scope_;
    bool heapAllocated_;

    // Non-copyable
    PooledTransactionScope(const PooledTransactionScope&) = delete;
    PooledTransactionScope& operator=(const PooledTransactionScope&) = delete;
};

} // namespace ACID
} // namespace COP
