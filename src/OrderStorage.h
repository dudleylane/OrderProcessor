/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "Singleton.h"
#include "TypesDef.h"
#include <oneapi/tbb/spin_rw_mutex.h>
#include <oneapi/tbb/concurrent_hash_map.h>
#include <map>
#include "NodePool.h"
#include "DataModelDef.h"

namespace COP
{
struct OrderEntry;
struct ExecutionEntry;
class IdTValueGenerator;

namespace Store
{

/// Keeps a newly saved order's entry lock until its creator has finished initialising it, and keeps the order marked
/// until its creator has enqueued the transaction that books it.
///
/// OrderDataStorage::save() makes an order reachable through locateByOrderId() and
/// locateByClOrderId() while the state machine action that created it is still running; the
/// creator then sets its status and state machine persistence. Passing a guard makes save()
/// write-lock the order before it is inserted into the lookup maps, so any other thread that finds
/// it blocks until release(). See #13.
///
/// The order is also reachable before its creator enqueues the transaction that matches and books it. A transaction
/// about the order enqueued in that gap would get the lower transaction id and run first: a cancel would be decided
/// before the order was booked. So save() also sets the order's bookingPending_, and the engine waits for it to clear
/// before it enqueues a cancel, a replace or a state change of the order, a timer's included (#96), or a stop's release
/// (#223). The creator calls bookingEnqueued() once that transaction is enqueued. See #83.
///
/// A guard serves one save().
class PublishGuard
{
public:
    PublishGuard() = default;
    /// Clears the mark too, so that a creator that throws before it enqueues leaves nobody waiting
    ~PublishGuard()
    {
        release();
        bookingEnqueued();
    }
    PublishGuard(const PublishGuard &) = delete;
    PublishGuard &operator=(const PublishGuard &) = delete;

    /// Unlocks the order, if one is held. Call once the order is fully initialised.
    void release() noexcept
    {
        if (nullptr != locked_)
        {
            locked_->entryMutex_.unlock();
            locked_ = nullptr;
        }
    }
    /// Clears the order's bookingPending_, if this guard set it. Call once the transaction that books the order is
    /// enqueued.
    void bookingEnqueued() noexcept
    {
        if (nullptr != pending_)
        {
            pending_->bookingPending_.store(false, std::memory_order_release);
            pending_ = nullptr;
        }
    }
    bool holds() const noexcept
    {
        return nullptr != locked_;
    }

private:
    friend class OrderDataStorage;
    OrderEntry *locked_ = nullptr;
    OrderEntry *pending_ = nullptr;
};

class OrderDataStorage
{
public:
    explicit OrderDataStorage();
    ~OrderDataStorage(void);

    void attach(OrderSaver *saver);

    /// Writes a new persisted version of the order, with the figures a decision left it with (#411), out-param
    /// receives it. Returns false when no saver is attached (tests, and the load passes), in which case nothing was
    /// written.
    bool persist(const OrderEntry &order, const OrderFigures &figures, u32 *version);
    /// Erases one persisted version, undoing a persist().
    void unpersist(const IdT &orderId, u32 version);
    /// Writes a new persisted version of the instrument's last trade (#237), with the releases pending on it (#518), as
    /// persist() writes an order's. Returns false when no saver is attached, in which case nothing was written.
    bool persistLastTrade(const SourceIdT &instrument, PriceT lastPx, const IdT &execId,
                          const std::vector<IdT> &pendingReleases, u32 *version);
    /// Erases one version of it, undoing a persistLastTrade()
    void unpersistLastTrade(const SourceIdT &instrument, u32 version);
    /// The persists this thread makes between beginPersistGroup() and commitPersistGroup() become durable together, or
    /// none of them do (#163). Without a saver, these do nothing.
    void beginPersistGroup();
    void commitPersistGroup();
    void abortPersistGroup() noexcept;

public:
    /// An order is found here from the moment save() stores it, a little before its creator enqueues the transaction
    /// that books it, and a transaction about the order enqueued in that gap would run before the booking. So a handler
    /// that finds an order from an event and enqueues a transaction about it calls the order's
    /// waitUntilBookingEnqueued() first, as Processor's cancel, replace and state change handlers do, and gets a
    /// ProcessorTest case that holds the order's mark open, as each of theirs has (#83, #625). An order found through
    /// the book was booked already.
    ///
    /// A lookup finds the order itself, as the engine's decisions leave it. A reader outside the transactions that
    /// decide about it, such as a WebSocket session's order snapshot or the book's depth, gives it as its last report
    /// left it: the fields set when it was stored, and published_ under the order's read lock. It never gives the
    /// figures a decision changes, which can hold what no report has said nor made durable (#404). That is a
    /// convention, as ruled on #628, so each such reader gets a test that holds a decision's report back and fails if
    /// the reader gives the order as it is, as the snapshot's and the depth's do. A replace copies the whole order to
    /// make its replacement, whose figures the engine decides anew.
    OrderEntry *locateByClOrderId(const RawDataEntry &clOrderId) const;
    OrderEntry *locateByOrderId(const IdT &orderId) const;
    /// The order with this number, the part of its id that clients see (#58). Null when no order has it, or when more
    /// than one does, which only a data directory written before #58 can hold; ambiguous, when given, says which.
    OrderEntry *locateByOrderNumber(u64 number, bool *ambiguous = nullptr) const;
    /// The largest order number stored, or 0 when there are none. After a restore, new numbers start above it (#58).
    u64 maxOrderNumber() const;
    /// With a guard, the new order is write-locked before it becomes reachable; the caller releases
    /// the guard once the order is fully initialised.
    OrderEntry *save(const OrderEntry &order, IdTValueGenerator *idGenerator, PublishGuard *publishGuard = nullptr);
    void restore(OrderEntry *order);

    /// Calls fn with each stored order, under the store's read lock. A reader outside a transaction gives each order
    /// as the lookups above say, and its fn takes no order's lock: a replace holds its original's write lock while it
    /// stores the replacement, which takes the store's, so an order's lock taken under the store's could deadlock with
    /// it. The order snapshot gathers the orders here and reads each one under its own lock once this returns (#404).
    template <typename Fn> void forEachOrder(Fn &&fn) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
        for (const auto &[id, entry] : ordersById_)
        {
            fn(id, *entry);
        }
    }

    ExecutionEntry *locateByExecId(const IdT &execId) const;
    void save(const ExecutionEntry *exec);
    ExecutionEntry *save(const ExecutionEntry &exec, IdTValueGenerator *idGenerator);

private:
    /// Reader-writer lock for order maps (dual-map inserts require atomicity)
    /// Allows concurrent reads, exclusive writes
    mutable oneapi::tbb::spin_rw_mutex orderRwLock_;

    /// Each map's nodes in a pool of its own, so that they lie together rather than among the orders, whose size moved
    /// every lookup (#467). Declared before the maps, so that they outlive them. The pools aren't synchronised: every
    /// change to the maps runs under orderRwLock_'s write lock, and so must any copy of one, whose nodes come from the
    /// same pool.
    NodePool ordersByIdNodes_;
    NodePool ordersByClIdNodes_;

    typedef PooledMap<IdT, OrderEntry *> OrdersByIDT;
    OrdersByIDT ordersById_;

    typedef PooledMap<RawDataEntry, OrderEntry *> OrdersByClientIDT;
    OrdersByClientIDT ordersByClId_;

    /// Hash functor for SourceIdT in concurrent_hash_map
    struct SourceIdTHash
    {
        size_t hash(const SourceIdT &key) const
        {
            // The id alone (#505): every execution id comes from IdTGenerator, whose ids never repeat (#58, #81), so
            // date_, the second the id was drawn in, added nothing to the key but the clock: which bucket an insert
            // reached, and the lazy rehashing that took, moved with it. equal() still compares both.
            return std::hash<u64>()(key.id_);
        }
        bool equal(const SourceIdT &a, const SourceIdT &b) const
        {
            return a == b;
        }
    };

    /// Lock-free concurrent hash map for executions (single-map operations)
    typedef oneapi::tbb::concurrent_hash_map<SourceIdT, ExecutionEntry *, SourceIdTHash> ExecByIDT;
    ExecByIDT executionsById_;

    OrderSaver *saver_;
};

typedef aux::Singleton<OrderDataStorage> OrderStorage;
} // namespace Store
} // namespace COP