/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "ClosedOrderSweep.h"

#include <cassert>
#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include <oneapi/tbb/spin_rw_mutex.h>

#include "DataModelDef.h"
#include "ExpiryScheduler.h"
#include "Logger.h"
#include "OrderStorage.h"
#include "TransactionDef.h"
#include "TransactionScope.h"
#include "TrOperations.h"

using namespace COP;
using namespace COP::Proc;

namespace
{
/// Points this thread's active scope, the arena Operation::operator new takes an operation from, at a scope while this
/// lives, and puts back the one before. A drop can be enqueued where another scope is active, in a deferred event's
/// drain: its operation would go into that scope's arena, and its own scope would free it as the heap's.
class ActiveScope
{
public:
    explicit ActiveScope(ACID::TransactionScope *scope) noexcept : previous_(ACID::TransactionScope::s_activeScope)
    {
        ACID::TransactionScope::s_activeScope = scope;
    }
    ~ActiveScope()
    {
        ACID::TransactionScope::s_activeScope = previous_;
    }
    ActiveScope(const ActiveScope &) = delete;
    ActiveScope &operator=(const ActiveScope &) = delete;

private:
    ACID::TransactionScope *const previous_;
};

/// The sweep whose drops this thread is enqueueing, in pump()
thread_local const ClosedOrderSweep *pumping = nullptr;

enum class Level
{
    NOTE,
    WARN,
    ERROR
};

/// Logs the line build() makes, and never throws: where a throw would roll back a drop, a line lost, e.g. to
/// bad_alloc, is the lesser harm
template <typename Build> void logQuietly(Level level, Build &&build) noexcept
{
    try
    {
        switch (level)
        {
        case Level::NOTE:
            aux::ExchLogger::instance()->note(build());
            break;
        case Level::WARN:
            aux::ExchLogger::instance()->warn(build());
            break;
        case Level::ERROR:
            aux::ExchLogger::instance()->error(build());
            break;
        }
    }
    catch (...)
    {
    }
}

std::string timeName(DateTimeT when)
{
    return std::format("{:%FT%TZ}", std::chrono::sys_seconds(std::chrono::seconds(when / 1000)));
}
} // namespace

ClosedOrderSweep::ClosedOrderSweep(Store::OrderDataStorage *storage, ACID::TransactionManager *transactMgr,
                                   u32 keepDays, size_t maxInFlight)
    : storage_(storage), transactMgr_(transactMgr), keepDays_(keepDays), maxInFlight_(maxInFlight)
{
    assert(nullptr != storage_);
    assert(nullptr != transactMgr_);
    assert(0 < maxInFlight_);
}

bool ClosedOrderSweep::droppable(const OrderEntry &order, DateTimeT keptFrom) noexcept
{
    // What its last report published, never its own figures: a decision changes those before the transaction that
    // persists and reports it runs, and a drop enqueued then would run before that transaction, whose persist would
    // write the record back (#404, as ruled on #722)
    return order.published_ &&
           closedBefore(order.published_->status_, order.published_->dayStart_, order.creationTime_, keptFrom);
}

size_t ClosedOrderSweep::start(DateTimeT now)
{
    bool stillRunning = false;
    {
        std::lock_guard<std::mutex> lock(lock_);
        stillRunning = running_;
        if (!stillRunning)
        {
            running_ = true;
            candidates_.clear();
            next_ = 0;
            keptFrom_ = 0;
            today_ = 0;
            dropped_ = 0;
            skipped_ = 0;
            freed_ = 0;
        }
    }
    if (stillRunning)
    {
        logQuietly(Level::WARN,
                   []
                   {
                       return std::string("The drop of closed orders a business day ago is still running: this day's "
                                          "waits for the next day's start (#722)");
                   });
        return 0;
    }
    size_t found = 0;
    try
    {
        // Those dropped in the days before: a business day after its drop, every reader that took an order's pointer
        // has long finished with it
        const DateTimeT today = ExpiryScheduler::dayStartOf(now);
        const size_t freed = storage_->freeDropped(today);
        const DateTimeT keptFrom = ExpiryScheduler::keptDaysStart(now, keepDays_);
        std::vector<IdT> candidates;
        if (0 != keptFrom)
        {
            // The orders under the store's lock, and each read under its own once that is released: a replace holds its
            // original's lock while it stores the replacement, which takes the store's (#404, #628)
            std::vector<const OrderEntry *> orders;
            storage_->forEachOrder(
                [&orders](const IdT &, const OrderEntry &order)
                {
                    orders.push_back(&order);
                });
            for (const OrderEntry *order : orders)
            {
                oneapi::tbb::spin_rw_mutex::scoped_lock orderLock(order->entryMutex_, false);
                if (droppable(*order, keptFrom))
                {
                    candidates.push_back(order->orderId_);
                }
            }
        }
        found = candidates.size();
        std::lock_guard<std::mutex> lock(lock_);
        candidates_.swap(candidates);
        keptFrom_ = keptFrom;
        today_ = today;
        freed_ = freed;
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(lock_);
        running_ = false;
        throw;
    }
    pump();
    return found;
}

void ClosedOrderSweep::dropFinished(bool wasDropped) noexcept
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        assert(0 < inFlight_);
        if (0 < inFlight_)
        {
            --inFlight_;
        }
        if (wasDropped)
        {
            ++dropped_;
        }
        else
        {
            ++skipped_;
        }
    }
    pump();
}

void ClosedOrderSweep::pump() noexcept
{
    // A drop that runs as it is enqueued, as the tests' and the benchmarks' transaction managers run each transaction,
    // is done before enqueueDrop() returns, and its dropFinished() pumps again on this thread: that call returns at
    // once, and this loop takes the place the drop freed, so drops never nest one in another
    if (this == pumping)
    {
        return;
    }
    const ClosedOrderSweep *const outer = pumping;
    pumping = this;
    for (;;)
    {
        IdT orderId;
        DateTimeT keptFrom = 0;
        DateTimeT today = 0;
        {
            std::lock_guard<std::mutex> lock(lock_);
            if ((maxInFlight_ <= inFlight_) || (candidates_.size() <= next_))
            {
                break;
            }
            orderId = candidates_[next_++];
            keptFrom = keptFrom_;
            today = today_;
            ++inFlight_;
        }
        bool enqueued = false;
        try
        {
            enqueued = enqueueDrop(orderId, keptFrom, today);
        }
        catch (const std::exception &ex)
        {
            logQuietly(Level::ERROR,
                       [&]
                       {
                           return "Closed order " + std::to_string(orderId.id_) +
                                  "'s drop couldn't be enqueued: " + ex.what() + ". It is kept (#722)";
                       });
        }
        catch (...)
        {
            logQuietly(Level::ERROR,
                       [&]
                       {
                           return "Closed order " + std::to_string(orderId.id_) +
                                  "'s drop couldn't be enqueued: an exception that isn't a std::exception. It is "
                                  "kept (#722)";
                       });
        }
        if (!enqueued)
        {
            // Done with no transaction: the store no longer holds the order, or its drop couldn't be enqueued
            std::lock_guard<std::mutex> lock(lock_);
            --inFlight_;
            ++skipped_;
        }
    }
    pumping = outer;

    // The sweep ends when its last drop is done, whichever thread sees it first
    bool ended = false;
    size_t droppedOrders = 0;
    size_t keptOrders = 0;
    size_t freedOrders = 0;
    DateTimeT keptFromDay = 0;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (running_ && (0 == inFlight_) && (candidates_.size() <= next_))
        {
            running_ = false;
            ended = true;
            droppedOrders = dropped_;
            keptOrders = skipped_;
            freedOrders = freed_;
            keptFromDay = keptFrom_;
            std::vector<IdT>().swap(candidates_);
            next_ = 0;
        }
    }
    // One line a day, as a start writes one (#597), not one an order (#584). With no day to keep from, it looked for
    // nothing, and says nothing.
    if (ended && (0 != keptFromDay))
    {
        logQuietly(Level::NOTE,
                   [&]
                   {
                       std::string line = "Dropped " + std::to_string(droppedOrders) +
                                          " closed orders of business days before " + timeName(keptFromDay) +
                                          " (--keep-closed-days " + std::to_string(keepDays_) + ", #722)";
                       if (0 < keptOrders)
                       {
                           line += ", and kept " + std::to_string(keptOrders) + " no longer closed by their turn";
                       }
                       return line + "; freed the " + std::to_string(freedOrders) + " dropped a business day before";
                   });
    }
}

bool ClosedOrderSweep::enqueueDrop(const IdT &orderId, DateTimeT keptFrom, DateTimeT today)
{
    // Found again by its id, which the sweep keeps rather than the order's pointer
    OrderEntry *order = storage_->locateByOrderId(orderId);
    if (nullptr == order)
    {
        return false;
    }
    // A handler that finds an order and enqueues a transaction about it waits for the order's booking first (#83,
    // #625): enqueued before the transaction that books the order, this one would run first. A refused order's report
    // can be out before its creator has enqueued that transaction.
    order->waitUntilBookingEnqueued();
    auto scope = std::make_unique<ACID::TransactionScope>();
    {
        ActiveScope active(scope.get());
        std::unique_ptr<ACID::Operation> drop(new ACID::DropClosedOrderTrOperation(order, keptFrom, today, this));
        scope->addOperation(drop);
    }
    std::unique_ptr<ACID::Transaction> tr(std::move(scope));
    try
    {
        transactMgr_->addTransaction(tr);
    }
    catch (...)
    {
        if (nullptr != tr)
        {
            throw; // not enqueued
        }
        // Enqueued, and its drop says when it is done: only a worker's start for it failed, and the next transaction
        // done starts it
        logQuietly(Level::ERROR,
                   [&]
                   {
                       return "Closed order " + std::to_string(orderId.id_) +
                              "'s drop was enqueued, and no worker started at once for it (#722)";
                   });
    }
    return true;
}

bool ClosedOrderSweep::running() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return running_;
}

size_t ClosedOrderSweep::inFlight() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return inFlight_;
}

size_t ClosedOrderSweep::dropped() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return dropped_;
}

size_t ClosedOrderSweep::skipped() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return skipped_;
}
