/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <atomic>
#include <stdexcept>
#include <utility> // For std::swap
#include <cstdint>
#include <cstring> // For ::memcpy
#include "TransactionScope.h"
#include "TrOperations.h"
#include "OrderStorage.h"
#include "Logger.h"

using namespace std;
using namespace COP::ACID;

namespace
{
std::atomic<uint64_t> failedTransactions{ 0 };

/// The type's enumerator, as the source names it. No default: a type added without a name here doesn't compile
/// (-Wswitch under WARNINGS_AS_ERRORS).
const char *operationName(OperationType type) noexcept
{
    switch (type)
    {
    case INVALID_TROPERATION:
        return "INVALID_TROPERATION";
    case CREATE_EXECREPORT_TROPERATION:
        return "CREATE_EXECREPORT_TROPERATION";
    case ADD_ORDERBOOK_TROPERATION:
        return "ADD_ORDERBOOK_TROPERATION";
    case REMOVE_ORDERBOOK_TROPERATION:
        return "REMOVE_ORDERBOOK_TROPERATION";
    case ENQUEUE_EVENT_TROPERATION:
        return "ENQUEUE_EVENT_TROPERATION";
    case CANCEL_REJECT_TROPERATION:
        return "CANCEL_REJECT_TROPERATION";
    case MATCH_ORDER_TROPERATION:
        return "MATCH_ORDER_TROPERATION";
    case CREATE_TRADE_EXECREPORT_TROPERATION:
        return "CREATE_TRADE_EXECREPORT_TROPERATION";
    case CREATE_REJECT_EXECREPORT_TROPERATION:
        return "CREATE_REJECT_EXECREPORT_TROPERATION";
    case CREATE_REPLACE_EXECREPORT_TROPERATION:
        return "CREATE_REPLACE_EXECREPORT_TROPERATION";
    case CREATE_CORRECT_EXECREPORT_TROPERATION:
        return "CREATE_CORRECT_EXECREPORT_TROPERATION";
    case PERSIST_ORDER_TROPERATION:
        return "PERSIST_ORDER_TROPERATION";
    case CANCEL_ORDER_TROPERATION:
        return "CANCEL_ORDER_TROPERATION";
    case REPLACE_ORDER_TROPERATION:
        return "REPLACE_ORDER_TROPERATION";
    case REFUSE_REPLACE_TROPERATION:
        return "REFUSE_REPLACE_TROPERATION";
    case CHANGE_ORDER_STATE_TROPERATION:
        return "CHANGE_ORDER_STATE_TROPERATION";
    case ADD_STOPBOOK_TROPERATION:
        return "ADD_STOPBOOK_TROPERATION";
    case REMOVE_STOPBOOK_TROPERATION:
        return "REMOVE_STOPBOOK_TROPERATION";
    case RELEASE_STOP_TROPERATION:
        return "RELEASE_STOP_TROPERATION";
    case REPORT_RELEASE_TROPERATION:
        return "REPORT_RELEASE_TROPERATION";
    case PERSIST_LASTTRADE_TROPERATION:
        return "PERSIST_LASTTRADE_TROPERATION";
    case REPORT_RESTATEMENT_TROPERATION:
        return "REPORT_RESTATEMENT_TROPERATION";
    case REPORT_TRIGGER_TROPERATION:
        return "REPORT_TRIGGER_TROPERATION";
    }
    return "an operation of an unknown type";
}

std::string transactionName(const TransactionId &id)
{
    return id.isValid() ? "Transaction " + std::to_string(id.id_) : std::string("A transaction without an id");
}

std::string operationOf(const Operation &op, size_t pos, size_t count)
{
    return "its operation " + std::to_string(pos + 1) + " of " + std::to_string(count) + ", " +
           operationName(op.type()) + " for " + std::to_string(op.getObjectId().id_);
}

/// What a second persist of a record in one transaction is refused with (#632)
[[noreturn]] void throwSecondPersist(const Operation &op)
{
    throw std::logic_error("TransactionScope: a second " + std::string(operationName(op.type())) + " for " +
                           std::to_string(op.getObjectId().id_) + " in one transaction (#148, #632)");
}
} // namespace

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

void TransactionScope::refuseSecondPersist(const Operation &op) const
{
    // A transaction writes a record once (#148): LMDBStorage::update() keeps only the newest version it finds besides
    // the one it writes, so a second version from the same transaction would erase the one its rollback falls back to,
    // and a rollback would leave nothing to restore. So the scope refuses a second persist of an order, or of an
    // instrument's last trade, whoever builds it (#632). Before it takes the operation: its caller still owns it.
    const OperationType type = op.type();
    if (((PERSIST_ORDER_TROPERATION == type) || (PERSIST_LASTTRADE_TROPERATION == type)) &&
        (nullptr != findOperation(type, op.getObjectId()))) [[unlikely]]
    {
        throwSecondPersist(op);
    }
}

void TransactionScope::addOperation(std::unique_ptr<Operation> &op)
{
    refuseSecondPersist(*op);
    operations_.push_back(op.release());
}

void TransactionScope::addOperationFirst(std::unique_ptr<Operation> &op)
{
    refuseSecondPersist(*op);
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

Operation *TransactionScope::findOperation(OperationType type, const IdT &id) const
{
    for (Operation *op : operations_)
    {
        if ((type == op->type()) && (id == op->getObjectId()))
        {
            return op;
        }
    }
    return nullptr;
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
    bool groupCall = false; // in the group's begin or commit, so that what throws is the group, not an operation

    size_t pos = 0;
    // execute commit
    try
    {
        if (grouped)
        {
            groupCall = true;
            cnxt.orderStorage_->beginPersistGroup();
            groupCall = false;
            groupOpen = true;
        }
        for (; pos < operations_.size(); ++pos)
        {
            if (groupOpen && (persists == pos))
            {
                groupCall = true;
                groupOpen = false;
                cnxt.orderStorage_->commitPersistGroup();
                groupCall = false;
            }
            operations_[pos]->execute(cnxt);
        }
        if (groupOpen)
        {
            groupCall = true;
            groupOpen = false;
            cnxt.orderStorage_->commitPersistGroup();
        }
        return true;
    }
    // Each failure is logged and counted (#514): until then nothing said that a transaction had failed. The handlers
    // run the rollback, so that what() is still there to log.
    catch (const std::exception &ex)
    {
        fail(cnxt, pos, groupOpen, groupCall, persists, ex.what());
    }
    catch (...)
    {
        fail(cnxt, pos, groupOpen, groupCall, persists, "an exception that isn't a std::exception");
    }
    return false;
}

void TransactionScope::fail(const Context &cnxt, size_t pos, bool groupOpen, bool groupCall, size_t persists,
                            const char *what) noexcept
{
    failedTransactions.fetch_add(1, std::memory_order_relaxed);
    if (groupOpen)
    {
        cnxt.orderStorage_->abortPersistGroup(); // noexcept, as every saver's abortGroup() is
    }
    // As in TaskManager::taskFailed() and OrderState_::swallowed(): nothing may leave here, so a line lost, e.g. to
    // bad_alloc, is the lesser harm, and the count above still records the failure
    try
    {
        aux::ExchLogger::instance()->error(describeFailure(id_, groupCall ? nullptr : operations_[pos],
                                                           groupCall ? persists : pos, operations_.size(), what));
    }
    catch (...)
    {
    }
    if (groupCall && (0 < pos))
    {
        // The group failed to commit, before the operation at pos ran: roll back from the last persist. Their records
        // never reached storage, so erasing their versions finds nothing. A group that failed to begin did so at 0,
        // before any persist ran.
        pos = persists - 1;
    }

    // From the operation that threw back to the first, each rollback on its own: one that throws is logged, and the
    // rest still run. The first that threw used to end the rollback, leaving the operations before it done (#514).
    const auto rollbackFailed = [this](size_t i, const char *failure) noexcept
    {
        try
        {
            aux::ExchLogger::instance()->error(
                describeFailedRollback(id_, *operations_[i], i, operations_.size(), failure));
        }
        catch (...)
        {
        }
    };
    for (size_t i = pos + 1; 0 < i--;)
    {
        try
        {
            operations_[i]->rollback(cnxt);
        }
        catch (const std::exception &ex)
        {
            rollbackFailed(i, ex.what());
        }
        catch (...)
        {
            rollbackFailed(i, "an exception that isn't a std::exception");
        }
    }
}

uint64_t TransactionScope::failedCount() noexcept
{
    return failedTransactions.load(std::memory_order_relaxed);
}

std::string TransactionScope::describeFailure(const TransactionId &id, const Operation *failed, size_t pos,
                                              size_t count, const char *what)
{
    const std::string where = (nullptr != failed)
                                  ? "in " + operationOf(*failed, pos, count)
                                  : "as its " + std::to_string(pos) + " persists were written together (#163)";
    return transactionName(id) + " failed " + where + ": " + what +
           ". Its operations are rolled back; what it decided stays in memory (#659)";
}

std::string TransactionScope::describeFailedRollback(const TransactionId &id, const Operation &op, size_t pos,
                                                     size_t count, const char *what)
{
    return transactionName(id) + " couldn't roll back " + operationOf(op, pos, count) + ": " + what;
}
