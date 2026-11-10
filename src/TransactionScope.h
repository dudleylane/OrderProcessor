/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include "TransactionDef.h"

namespace COP
{

namespace ACID
{
class TransactOperation;

class TransactionScope final : public Scope, public Transaction
{
public:
    /// Arena size: enough for ~12 typical operations (largest is ~160 bytes).
    /// Falls back to heap for larger transactions.
    static constexpr size_t ARENA_SIZE = 2048;

    TransactionScope(void);
    ~TransactionScope(void);

    /**
	 * Reset the TransactionScope for reuse without deallocation.
	 * Clears all operations and stage boundaries while preserving
	 * allocated capacity for better performance.
	 */
    void reset();

    /**
	 * Swap the contents of this TransactionScope with another.
	 * Used by the pool to transfer data from a pooled scope to a heap-allocated one.
	 */
    void swap(TransactionScope &other) noexcept;

    /**
	 * Allocate memory from the embedded arena (bump allocator).
	 * Returns nullptr if arena is full — caller should fall back to heap.
	 */
    void *arenaAllocate(size_t size, size_t align) noexcept;

    /**
	 * Check if a pointer was allocated from this scope's arena.
	 */
    bool isFromArena(const void *ptr) const noexcept;

    /**
	 * Thread-local pointer to the currently active TransactionScope.
	 * Set by Processor before state machine processing so that
	 * Operation::operator new can allocate from the arena.
	 */
    static thread_local TransactionScope *s_activeScope;

public:
    /// reimplemented from Scope
    virtual void addOperation(std::unique_ptr<Operation> &op);
    virtual void addOperationFirst(std::unique_ptr<Operation> &op);
    virtual void removeLastOperation();
    virtual size_t startNewStage();
    virtual void removeStage(const size_t &id);
    virtual Operation *findOperation(OperationType type, const IdT &id) const;

    /// the operations the scope holds
    size_t operationCount() const noexcept
    {
        return operations_.size();
    }

public:
    /// reimplemented from Transaction
    virtual const TransactionId &transactionId() const;
    virtual void setTransactionId(const TransactionId &id);
    virtual void getRelatedObjects(ObjectsInTransactionT *obj) const;
    virtual bool executeTransaction(const Context &cnxt);

    /// How many transactions have failed in this process: an operation threw, or their persists' group did as it began
    /// or committed (#163), and their operations were rolled back. Each is logged at error level too (#514).
    static uint64_t failedCount() noexcept;
    /// The error line for a transaction that failed (#514). id: its own, invalid for one with none (Processor's match
    /// of a restored order). failed: the operation that threw, the pos'th of count from 0, or null when the persists'
    /// group did, the first pos of them.
    static std::string describeFailure(const TransactionId &id, const Operation *failed, size_t pos, size_t count,
                                       const char *what);
    /// The error line for a rollback that threw, that of the pos'th operation of count from 0 (#514)
    static std::string describeFailedRollback(const TransactionId &id, const Operation &op, size_t pos, size_t count,
                                              const char *what);

private:
    /// Destroy an operation: calls destructor, frees memory only if heap-allocated.
    void destroyOperation(Operation *op) noexcept;
    /// Throws std::logic_error if op persists a record this scope persists already (#632)
    void refuseSecondPersist(const Operation &op) const;
    /// What executeTransaction() does once something threw: counts and logs the failure, then rolls back from the
    /// operation at pos to the first, each rollback on its own (#514). groupCall: the persists' group threw, as it
    /// began (pos 0) or committed (pos persists), not the operation at pos.
    void fail(const Context &cnxt, size_t pos, bool groupOpen, bool groupCall, size_t persists,
              const char *what) noexcept;

    std::string invalidReason_;

    typedef std::vector<Operation *> OperationsT;
    OperationsT operations_;

    // Stage boundaries - each entry is the starting index of a stage in operations_
    typedef std::vector<size_t> StageBoundariesT;
    StageBoundariesT stageBoundaries_;

    TransactionId id_;

    // Bump allocator arena for Operation objects — avoids heap allocation on hot path
    alignas(std::max_align_t) char arenaBuffer_[ARENA_SIZE];
    size_t arenaOffset_;

private:
    TransactionScope(const TransactionScope &);
    const TransactionScope &operator=(const TransactionScope &);
};

// Compile-time validation for TransactionScope layout
static_assert(TransactionScope::ARENA_SIZE >= 1024, "Arena must be large enough for multiple Operation objects");

} // namespace ACID
} // namespace COP