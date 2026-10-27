/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <set>

#include "ExchUtils.h"
#include "TypesDef.h"

namespace COP
{

class OrderBook;
class IdTValueGenerator;

namespace Store
{
class OrderDataStorage;
}
namespace Queues
{
class InQueues;
class OutQueues;
} // namespace Queues

namespace Proc
{
class OrderMatcher;
class DeferedEventContainer;
} // namespace Proc

namespace ACID
{

typedef IdT TransactionId;
typedef std::set<TransactionId> TransactionIdsT;

enum ObjectType
{
    invalid_ObjectType = 0,
    instrument_ObjectType,
    order_ObjectType,
    execution_ObjectType
};

struct ObjectInTransaction
{
    IdT id_;
    ObjectType type_;

    constexpr ObjectInTransaction() : id_(), type_(invalid_ObjectType) {}
    constexpr ObjectInTransaction(const ObjectType &type, const IdT &id) : id_(id), type_(type) {}
};
constexpr bool operator<(const ObjectInTransaction &left, const ObjectInTransaction &right)
{
    if (left.type_ == right.type_)
    {
        return left.id_ < right.id_;
    }
    return left.type_ < right.type_;
}

constexpr bool operator==(const ObjectInTransaction &left, const ObjectInTransaction &right)
{
    return (left.type_ == right.type_) && (left.id_ == right.id_);
}

const size_t DEPENDANT_OBJECT_LIMIT = 10;
struct ObjectsInTransaction
{
    ObjectInTransaction list_[DEPENDANT_OBJECT_LIMIT];
    unsigned char size_;

    ObjectsInTransaction() : size_(0) {}
};

typedef ObjectsInTransaction ObjectsInTransactionT;

enum OperationType
{
    INVALID_TROPERATION = 0,
    CREATE_EXECREPORT_TROPERATION, // creates and enqueues ExecutionReport for the changed order
    ADD_ORDERBOOK_TROPERATION,     // adds order into orderBook
    REMOVE_ORDERBOOK_TROPERATION,  // removes order from orderBook
    ENQUEUE_EVENT_TROPERATION,     // enqueues event for processing
    CANCEL_REJECT_TROPERATION,
    MATCH_ORDER_TROPERATION,
    CREATE_TRADE_EXECREPORT_TROPERATION,   // creates and enqueues Trade ExecutionReport for the changed order
    CREATE_REJECT_EXECREPORT_TROPERATION,  // creates and enqueues Reject ExecutionReport for the changed order
    CREATE_REPLACE_EXECREPORT_TROPERATION, // creates and enqueues replace ExecutionReport for the changed order
    CREATE_CORRECT_EXECREPORT_TROPERATION, // creates and enqueues Correct ExecutionReport for the changed order
    PERSIST_ORDER_TROPERATION,             // writes a new persisted version of the changed order
    CANCEL_ORDER_TROPERATION,              // hands a client's cancel to the processor once it is the order's turn
    REPLACE_ORDER_TROPERATION,             // hands a client's replace to the processor once it is the order's turn
    REFUSE_REPLACE_TROPERATION,            // tells the requester that its replace was refused
    CHANGE_ORDER_STATE_TROPERATION,        // hands a state change to the processor once it is the order's turn
    ADD_STOPBOOK_TROPERATION,              // holds a stop order in its instrument's stop book (#223)
    REMOVE_STOPBOOK_TROPERATION,           // takes a held stop order out of the stop book (#223)
    RELEASE_STOP_TROPERATION,              // releases a held stop order a trade reached, and matches it (#223)
    REPORT_RELEASE_TROPERATION,            // reports a stop's release, if it released the order (#236)
    PERSIST_LASTTRADE_TROPERATION,         // writes a new persisted version of an instrument's last trade (#237)
    REPORT_RESTATEMENT_TROPERATION         // reports an order's day figures begun again for a new business day (#244)
};

struct Context
{
    Store::OrderDataStorage *orderStorage_;
    OrderBook *orderBook_;
    Queues::InQueues *inQueues_;
    Queues::OutQueues *outQueues_;
    Proc::OrderMatcher *orderMatch_;
    IdTValueGenerator *idGenerator_;
    Proc::DeferedEventContainer *deferedEvents_;
    /// The processor's clock, milliseconds since the epoch, UTC: what business day a trade is in (#244). Tests give one
    /// of their own; without one it is the system's.
    const std::function<DateTimeT()> *clock_ = nullptr;

    Context()
        : orderStorage_(nullptr), orderBook_(nullptr), inQueues_(nullptr), outQueues_(nullptr), orderMatch_(nullptr),
          idGenerator_(nullptr), deferedEvents_(nullptr)
    {
    }

    Context(Store::OrderDataStorage *orderStorage, OrderBook *orderBook, Queues::InQueues *inQueues,
            Queues::OutQueues *outQueues, Proc::OrderMatcher *orderMatch, IdTValueGenerator *idGenerator,
            Proc::DeferedEventContainer *deferedEvents = nullptr)
        : orderStorage_(orderStorage), orderBook_(orderBook), inQueues_(inQueues), outQueues_(outQueues),
          orderMatch_(orderMatch), idGenerator_(idGenerator), deferedEvents_(deferedEvents)
    {
    }

    DateTimeT now() const
    {
        return ((nullptr != clock_) && *clock_) ? (*clock_)() : aux::currentDateTime();
    }
};

class Operation
{
public:
    explicit Operation(OperationType type, const IdT &id) : type_(type), id_(id), relatedId_() {}
    explicit Operation(OperationType type, const IdT &id, const IdT &relId) : type_(type), id_(id), relatedId_(relId) {}
    virtual ~Operation() {}
    virtual void execute(const Context &cnxt) = 0;
    virtual void rollback(const Context &cnxt) = 0;

    OperationType type() const noexcept
    {
        return type_;
    }
    const IdT &getObjectId() const noexcept
    {
        return id_;
    }
    const IdT &getRelatedId() const noexcept
    {
        return relatedId_;
    }

    /// Arena-aware allocation: when a TransactionScope arena is active,
    /// allocations are served from the arena's bump allocator (zero heap cost).
    /// Falls back to global operator new when no arena is active or arena is full.
    static void *operator new(size_t size);
    static void operator delete(void *ptr, size_t size) noexcept;

protected:
    OperationType type_;
    IdT id_;
    IdT relatedId_;
};

class Scope
{
public:
    ~Scope() {}

    /// add operation into the transaction
    virtual void addOperation(std::unique_ptr<Operation> &op) = 0;

    /// add operation at the front, so it executes before every operation added so far. Persisting
    /// uses this: an execution report must not reach a client before the order is durable (#28).
    virtual void addOperationFirst(std::unique_ptr<Operation> &op) = 0;

    /// removes last operation from transaction scope
    virtual void removeLastOperation() = 0;

    /// stage is a set of operations - sub transaction
    /// add new stage - all operations before next startNewStage() will be related to the same stage
    virtual size_t startNewStage() = 0;

    /// removes operation's related to the passed stage from transaction
    virtual void removeStage(const size_t &id) = 0;

    /// The scope's operation of this type on this object, or nullptr. Processor persists an order once per scope with
    /// it, which LMDBStorage::update()'s pruning relies on (#148), giving a later decision in the scope to the operation
    /// the first made (#411).
    virtual Operation *findOperation(OperationType type, const IdT &id) const = 0;

    /// true if the scope holds an operation of this type on this object
    bool hasOperation(OperationType type, const IdT &id) const
    {
        return nullptr != findOperation(type, id);
    }
};

class Transaction
{
public:
    virtual ~Transaction() {}

    /// returns id of the transaction
    virtual const TransactionId &transactionId() const = 0;
    virtual void setTransactionId(const TransactionId &id) = 0;
    virtual void getRelatedObjects(ObjectsInTransactionT *obj) const = 0;

    /// executes all operations of the transaction.
    /// returns false if transaction was rolledback
    virtual bool executeTransaction(const Context &cnxt) = 0;
};

class TransactionObserver
{
public:
    virtual ~TransactionObserver() {}

    virtual void onReadyToExecute() = 0;
};

class TransactionIterator
{
public:
    virtual ~TransactionIterator() {}

    virtual bool next(TransactionId *id, Transaction **tr) = 0;
    virtual bool get(TransactionId *id, Transaction **tr) const = 0;
    virtual bool isValid() const = 0;
};

class TransactionProcessor
{
public:
    virtual ~TransactionProcessor() {}

    virtual void process(const TransactionId &id, Transaction *tr) = 0;
};
typedef std::vector<TransactionProcessor *> ProcessorPoolT;

class TransactionManager
{
public:
    virtual ~TransactionManager() {}

    /// attaches event observer
    virtual void attach(TransactionObserver *obs) = 0;

    /// dettaches event observer
    virtual TransactionObserver *detach() = 0;

    /// add new transaction for execution
    virtual void addTransaction(std::unique_ptr<Transaction> &tr) = 0;

    /// remove transaction, do not execute it
    virtual bool removeTransaction(const TransactionId &id, Transaction *t) = 0;

    /// returns ids of all transactions that this depends on
    virtual bool getParentTransactions(const TransactionId &id, TransactionIdsT *parent) const = 0;

    /// returns ids of all transactions that blocked by this one
    virtual bool getRelatedTransactions(const TransactionId &id, TransactionIdsT *related) const = 0;

    virtual TransactionIterator *iterator() = 0;

    /// the transactions added and not yet removed, waiting or running
    virtual size_t pendingTransactions() const = 0;
};

} // namespace ACID
} // namespace COP