/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

#include "TypesDef.h"

namespace COP
{
struct OrderEntry;

namespace Store
{
class OrderDataStorage;
}

namespace ACID
{
class TransactionManager;
}

namespace Proc
{

/// Drops a running server's closed orders of the business days before the kept ones (#722), as a start drops them
/// (#597): at each business day's start, when the expiry scheduler's DROP_CLOSED_ORDERS timer fires
/// (ExpiryScheduler::scheduleClosedOrderDrops()), with --keep-closed-days above 0.
///
/// - Which: each order closed for good in a business day before the kept ones (closedBefore()), by what its last report
///   published (OrderEntry::published_), never by its own figures: a decision changes those before the transaction
///   that persists and reports it runs (#404). Each is read under its own lock once the store's is released, as the
///   order snapshot reads them (#628).
/// - How: each in a transaction of its own that names the order (ACID::DropClosedOrderTrOperation), so every
///   transaction about it enqueued before has run, and enqueued only once the order's booking is (#83, #625). The
///   transaction tests the order again, takes it out of the store, and erases its record in one LMDB commit.
/// - Paced: at most maxInFlight drops are enqueued and not yet done, and the next is enqueued as one is done. A day's
///   drops enqueued at once would count in the engine's work in flight (#519), past which the WebSocket sessions stop
///   reading, and live orders would wait behind them for the transaction workers.
/// - Kept: a dropped order's entry stays allocated until the next day's sweep frees it (OrderDataStorage::drop()),
///   since a reader outside a transaction, or a transaction about the order enqueued before the drop, may hold its
///   pointer.
///
/// start() runs on an event worker, and dropFinished() on the transaction workers, each at once with the others.
class ClosedOrderSweep
{
public:
    /// The drops a sweep keeps in flight at once
    static constexpr size_t MAX_IN_FLIGHT = 8;

    /// keepDays: --keep-closed-days, the business days whose closed orders are kept, the current one counted. With 0
    /// nothing is dropped.
    ClosedOrderSweep(Store::OrderDataStorage *storage, ACID::TransactionManager *transactMgr, u32 keepDays,
                     size_t maxInFlight = MAX_IN_FLIGHT);
    ClosedOrderSweep(const ClosedOrderSweep &) = delete;
    ClosedOrderSweep &operator=(const ClosedOrderSweep &) = delete;

    /// A business day starts, at now: frees the orders dropped in the days before, finds the orders to drop, and
    /// enqueues the first drops. Returns how many orders it found. If the last sweep's drops aren't all done, that
    /// sweep goes on, and this one does nothing, not even free.
    size_t start(DateTimeT now);
    /// A drop's transaction is done with its order, which it dropped or found it no longer may: the next drop is
    /// enqueued. Never throws, since it runs at the end of the drop's operation, whose rollback a throw would run.
    void dropFinished(bool wasDropped) noexcept;

    /// Whether a running server drops the order: what its last report published says it is closed for good, in a
    /// business day before keptFrom (closedBefore()). An order whose first report isn't out isn't dropped. Call under
    /// the order's lock.
    static bool droppable(const OrderEntry &order, DateTimeT keptFrom) noexcept;

    /// Whether a sweep runs: drops of it are left to enqueue, or not yet done
    bool running() const;
    /// The drops enqueued and not yet done
    size_t inFlight() const;
    /// The orders the current or last sweep dropped, and those whose drops found they no longer may be
    size_t dropped() const;
    size_t skipped() const;

private:
    /// Enqueues drops while fewer than maxInFlight_ are in flight and any are left, then ends the sweep if all its
    /// drops are done
    void pump() noexcept;
    /// Enqueues the drop of the order with this id, in the business day that starts at today: false if the store no
    /// longer holds it
    bool enqueueDrop(const IdT &orderId, DateTimeT keptFrom, DateTimeT today);

    Store::OrderDataStorage *storage_;
    ACID::TransactionManager *transactMgr_;
    const u32 keepDays_;
    const size_t maxInFlight_;

    /// Guards what follows. A leaf in the lock order (#636; docs/ARCHITECTURE.md section 6.2): it is held only to take
    /// the next drop or count one done, and no other lock, the logger's included, is taken under it.
    mutable std::mutex lock_;
    bool running_ = false;
    /// The orders to drop, in id order, and the next one's place
    std::vector<IdT> candidates_;
    size_t next_ = 0;
    size_t inFlight_ = 0;
    DateTimeT keptFrom_ = 0;
    /// The start of the business day the sweep runs in
    DateTimeT today_ = 0;
    size_t dropped_ = 0;
    size_t skipped_ = 0;
    size_t freed_ = 0;
};

} // namespace Proc
} // namespace COP
