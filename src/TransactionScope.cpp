/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include <utility> // For std::swap
#include <cstdint>
#include <cstring> // For ::memcpy
#include "TransactionScope.h"
#include "TrOperations.h"
#include "OrderStorage.h"

using namespace std;
using namespace COP::ACID;

// Thread-local active scope for arena allocation
thread_local TransactionScope *TransactionScope::s_activeScope = nullptr;

// Operation arena-aware allocation
void *Operation::operator new(size_t size)
{
    if (auto *scope = TransactionScope::s_activeScope)
    {
        if (void *mem = scope->arenaAllocate(size, alignof(std::max_align_t)))
        {
            return mem;
        }
    }
    return ::operator new(size);
}

void Operation::operator delete(void *ptr, size_t /*size*/) noexcept
{
    if (!ptr)
    {
        return;
    }
    // Check the thread-local active scope first (covers exception-unwind during state machine processing)
    if (auto *scope = TransactionScope::s_activeScope)
    {
        if (scope->isFromArena(ptr))
        {
            return; // Arena memory — reclaimed on scope reset
        }
    }
    // Heap-allocated — free normally
    ::operator delete(ptr);
}

TransactionScope::TransactionScope(void) : arenaOffset_(0) {}

TransactionScope::~TransactionScope(void)
{
    for (size_t pos = 0; pos < operations_.size(); ++pos)
    {
        destroyOperation(operations_[pos]);
    }
}

void TransactionScope::reset()
{
    // Destroy all operations — arena-allocated are just destructed, heap-allocated are freed
    for (size_t pos = 0; pos < operations_.size(); ++pos)
    {
        destroyOperation(operations_[pos]);
    }
    operations_.clear();

    // Reset the arena bump pointer
    arenaOffset_ = 0;

    // Clear stage boundaries, preserving vector capacity
    stageBoundaries_.clear();

    // Reset transaction ID
    id_ = TransactionId();

    // Clear invalid reason string, preserving capacity
    invalidReason_.clear();
}

void *TransactionScope::arenaAllocate(size_t size, size_t align) noexcept
{
    size_t padding = (align - (arenaOffset_ % align)) % align;
    size_t newOffset = arenaOffset_ + padding + size;
    if (newOffset > ARENA_SIZE)
    {
        return nullptr; // Arena full — caller falls back to heap
    }
    void *ptr = arenaBuffer_ + arenaOffset_ + padding;
    arenaOffset_ = newOffset;
    return ptr;
}

bool TransactionScope::isFromArena(const void *ptr) const noexcept
{
    auto p = reinterpret_cast<uintptr_t>(ptr);
    auto base = reinterpret_cast<uintptr_t>(arenaBuffer_);
    return p >= base && p < base + ARENA_SIZE;
}

void TransactionScope::destroyOperation(Operation *op) noexcept
{
    if (!op)
    {
        return;
    }
    op->~Operation(); // Always call destructor
    if (!isFromArena(op))
    {
        ::operator delete(static_cast<void *>(op)); // Free heap memory
    }
    // Arena memory is freed in bulk on reset()
}

void TransactionScope::swap(TransactionScope &other) noexcept
{
    // Swap all member variables efficiently
    // Note: arena buffers are NOT swapped — operations allocated from one
    // scope's arena must stay with that scope. The swap only transfers
    // the operations vector and metadata. Since the detach-based pool
    // release no longer uses swap, this is safe.
    invalidReason_.swap(other.invalidReason_);
    operations_.swap(other.operations_);
    stageBoundaries_.swap(other.stageBoundaries_);
    std::swap(id_, other.id_);
    std::swap(arenaOffset_, other.arenaOffset_);
    // Swap arena buffers so operations still point to valid memory
    char tmpBuf[ARENA_SIZE];
    ::memcpy(tmpBuf, arenaBuffer_, ARENA_SIZE);
    ::memcpy(arenaBuffer_, other.arenaBuffer_, ARENA_SIZE);
    ::memcpy(other.arenaBuffer_, tmpBuf, ARENA_SIZE);
}

const TransactionId &TransactionScope::transactionId() const
{
    return id_;
}

void TransactionScope::addOperation(std::unique_ptr<Operation> &op)
{
    operations_.push_back(op.release());
}

void TransactionScope::addOperationFirst(std::unique_ptr<Operation> &op)
{
    operations_.insert(operations_.begin(), op.get());
    op.release();
    // Stage boundaries are indices into operations_, so every one of them moves along by one.
    for (StageBoundariesT::iterator it = stageBoundaries_.begin(); it != stageBoundaries_.end(); ++it)
    {
        ++(*it);
    }
}

void TransactionScope::removeLastOperation()
{
    if (operations_.empty())
    {
        return;
    }

    destroyOperation(operations_.back());
    operations_.pop_back();

    // If the last operation was at a stage boundary, remove that boundary too
    while (!stageBoundaries_.empty() && stageBoundaries_.back() >= operations_.size())
    {
        stageBoundaries_.pop_back();
    }
}

size_t TransactionScope::startNewStage()
{
    // Record the current position as the start of a new stage
    size_t stageId = stageBoundaries_.size();
    stageBoundaries_.push_back(operations_.size());
    return stageId;
}

bool TransactionScope::hasOperation(OperationType type, const IdT &id) const
{
    for (const Operation *op : operations_)
    {
        if ((type == op->type()) && (id == op->getObjectId()))
        {
            return true;
        }
    }
    return false;
}

void TransactionScope::removeStage(const size_t &stageId)
{
    if (stageId >= stageBoundaries_.size())
    {
        throw std::runtime_error("TransactionScope::removeStage(): invalid stage id!");
    }

    // Get the starting index of this stage
    size_t stageStart = stageBoundaries_[stageId];

    // Destroy all operations from stageStart to the end
    for (size_t i = stageStart; i < operations_.size(); ++i)
    {
        destroyOperation(operations_[i]);
    }

    // Remove operations from stageStart onwards
    operations_.erase(operations_.begin() + static_cast<std::ptrdiff_t>(stageStart), operations_.end());

    // Remove this stage and all subsequent stage boundaries
    stageBoundaries_.erase(stageBoundaries_.begin() + static_cast<std::ptrdiff_t>(stageId), stageBoundaries_.end());
}

void TransactionScope::setTransactionId(const TransactionId &id)
{
    id_ = id;
}

void TransactionScope::getRelatedObjects(ObjectsInTransactionT *obj) const
{
    assert(nullptr != obj);
    obj->size_ = 0;
    for (OperationsT::const_iterator it = operations_.begin(); it != operations_.end(); ++it)
    {
        const IdT &id = (*it)->getObjectId();
        assert(id.isValid());
        if (id.isValid())
        {
            bool duplicateId = false;
            for (size_t i = 0; i < obj->size_; ++i)
            {
                duplicateId = (id == obj->list_[i].id_);
                if (duplicateId)
                {
                    break;
                }
            }
            if (!duplicateId)
            {
                /// todo: should be redesigned
                if (DEPENDANT_OBJECT_LIMIT == obj->size_)
                {
                    throw std::runtime_error(
                        "getRelatedObjects() failed to fill ObjectsInTransactionT, transaction use too many objects!");
                }
                obj->list_[(obj->size_)++] = ObjectInTransaction(order_ObjectType, id);
            }
        }
        const IdT &rid = (*it)->getRelatedId();
        if (rid.isValid())
        {
            bool duplicateId = false;
            for (size_t i = 0; i < obj->size_; ++i)
            {
                duplicateId = (rid == obj->list_[i].id_);
                if (duplicateId)
                {
                    break;
                }
            }
            if (!duplicateId)
            {
                /// todo: should be redesigned
                if (DEPENDANT_OBJECT_LIMIT == obj->size_)
                {
                    throw std::runtime_error(
                        "getRelatedObjects() failed to fill ObjectsInTransactionT, transaction use too many objects!");
                }
                obj->list_[(obj->size_)++] = ObjectInTransaction(instrument_ObjectType, rid);
            }
        }
    }
}

bool TransactionScope::executeTransaction(const Context &cnxt)
{
    if (operations_.empty()) [[unlikely]]
    {
        return true;
    }

    // The persist operations come first (Scope::addOperationFirst, #28). When there are several, one for each order a
    // trade fills, their records go to storage as one write, committed before the first operation that publishes: a
    // crash leaves all of this transaction's orders durable, or none of them (#163). Each used to commit on its own, and
    // a crash between two commits left half a trade.
    // A trade's transaction also persists its instrument's last trade there (#237).
    size_t persists = 0;
    while ((persists < operations_.size()) && ((PERSIST_ORDER_TROPERATION == operations_[persists]->type()) ||
                                               (PERSIST_LASTTRADE_TROPERATION == operations_[persists]->type())))
    {
        ++persists;
    }
    const bool grouped = (1 < persists) && (nullptr != cnxt.orderStorage_);
    bool groupOpen = false;
    bool committing = false;

    size_t pos = 0;
    // execute commit
    try
    {
        if (grouped)
        {
            cnxt.orderStorage_->beginPersistGroup();
            groupOpen = true;
        }
        for (; pos < operations_.size(); ++pos)
        {
            if (groupOpen && (persists == pos))
            {
                committing = true;
                groupOpen = false;
                cnxt.orderStorage_->commitPersistGroup();
                committing = false;
            }
            operations_[pos]->execute(cnxt);
        }
        if (groupOpen)
        {
            committing = true;
            groupOpen = false;
            cnxt.orderStorage_->commitPersistGroup();
        }
        return true;
    }
    catch (const std::exception &)
    {
    }
    catch (...)
    {
    }

    if (groupOpen)
    {
        cnxt.orderStorage_->abortPersistGroup();
    }
    if (committing)
    {
        // The group failed to commit, before the operation at pos ran: roll back from the last persist. Their records
        // never reached storage, so erasing their versions finds nothing.
        pos = persists - 1;
    }

    // commit failed - execute rollback
    try
    {
        // pos points to failed operation, rollback from there to beginning
        // Use do-while to handle unsigned wraparound safely
        do
        {
            operations_[pos]->rollback(cnxt);
        } while (pos-- > 0);
    }
    catch (const std::exception &)
    {
    }
    catch (...)
    {
    }
    return false;
}
