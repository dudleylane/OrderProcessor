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
/// The order is also reachable before its creator enqueues the transaction that matches and books it. A cancel or
/// replace enqueued in that gap would get the lower transaction id and be decided before the order was booked. So
/// save() also sets the order's bookingPending_, and Processor waits for it to clear before it enqueues a cancel or
/// replace of the order. The creator calls bookingEnqueued() once that transaction is enqueued. See #83.
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

    /// Writes a new persisted version of the order, out-param receives it. Returns false when no
    /// saver is attached (tests, and the load passes), in which case nothing was written.
    bool persist(const OrderEntry &order, u32 *version);
    /// Erases one persisted version, undoing a persist().
    void unpersist(const IdT &orderId, u32 version);

public:
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

    typedef std::map<IdT, OrderEntry *> OrdersByIDT;
    OrdersByIDT ordersById_;

    typedef std::map<RawDataEntry, OrderEntry *> OrdersByClientIDT;
    OrdersByClientIDT ordersByClId_;

    /// Hash functor for SourceIdT in concurrent_hash_map
    struct SourceIdTHash
    {
        size_t hash(const SourceIdT &key) const
        {
            // Combine id_ and date_ for hash
            return std::hash<u64>()(key.id_) ^ (std::hash<u32>()(key.date_) << 1);
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