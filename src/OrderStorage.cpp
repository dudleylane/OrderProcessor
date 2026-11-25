/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include "OrderStorage.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "Logger.h"

using namespace COP;
using namespace std;
using namespace COP::Store;

OrderDataStorage::OrderDataStorage()
    : ordersById_(OrdersByIDT::allocator_type(&ordersByIdNodes_)),
      ordersByClId_(OrdersByClientIDT::allocator_type(&ordersByClIdNodes_)), saver_(nullptr)
{
    aux::ExchLogger::instance()->note("OrderDataStorage created");
}

void OrderDataStorage::attach(OrderSaver *saver)
{
    assert(nullptr == saver_);
    saver_ = saver;
}

OrderDataStorage::~OrderDataStorage(void)
{
    aux::ExchLogger::instance()->note("OrderDataStorage destroying");

    // Clean up orders with exclusive write lock. The temporary takes the map's allocator, so its nodes go back to
    // their pool (#467)
    OrdersByIDT tmp(ordersById_.get_allocator());
    DroppedT dropped;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
        std::swap(tmp, ordersById_);
        ordersByClId_.clear();
        dropped.swap(dropped_);
    }
    for (OrdersByIDT::iterator it = tmp.begin(); it != tmp.end(); ++it)
    {
        delete it->second;
    }
    // And those a running server dropped and kept (#722)
    for (const DroppedT::value_type &day : dropped)
    {
        for (OrderEntry *order : day.second)
        {
            delete order;
        }
    }

    aux::ExchLogger::instance()->note("OrderDataStorage destroyed");
}

// ============================================================================
// Order operations - use reader-writer lock for concurrent reads
// ============================================================================

OrderEntry *OrderDataStorage::locateByClOrderId(const RawDataEntry &clOrderId) const
{
    // Shared read lock - allows concurrent lookups
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
    OrdersByClientIDT::const_iterator it = ordersByClId_.find(clOrderId);
    if (ordersByClId_.end() == it) [[unlikely]]
    {
        return nullptr;
    }
    return it->second;
}

OrderEntry *OrderDataStorage::locateByOrderId(const IdT &orderId) const
{
    // Shared read lock - allows concurrent lookups
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
    OrdersByIDT::const_iterator it = ordersById_.find(orderId);
    if (ordersById_.end() == it) [[unlikely]]
    {
        return nullptr;
    }
    return it->second;
}

OrderEntry *OrderDataStorage::locateByOrderNumber(u64 number, bool *ambiguous) const
{
    if (nullptr != ambiguous)
    {
        *ambiguous = false;
    }
    // Ids order by number, then by creation time, so the orders with this number are adjacent
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
    OrdersByIDT::const_iterator it = ordersById_.lower_bound(IdT(number, 0));
    if ((ordersById_.end() == it) || (number != it->first.id_))
    {
        return nullptr;
    }
    OrdersByIDT::const_iterator next = std::next(it);
    if ((ordersById_.end() != next) && (number == next->first.id_))
    {
        if (nullptr != ambiguous)
        {
            *ambiguous = true;
        }
        return nullptr;
    }
    return it->second;
}

u64 OrderDataStorage::maxOrderNumber() const
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
    return ordersById_.empty() ? 0 : ordersById_.rbegin()->first.id_;
}

OrderEntry *OrderDataStorage::save(const OrderEntry &order, IdTValueGenerator *idGenerator, PublishGuard *publishGuard)
{
    const char *refusal = nullptr;
    OrderEntry *saved = saveUnlessClOrdIdInUse(order, idGenerator, publishGuard, &refusal);
    if (nullptr == saved) [[unlikely]]
    {
        throw std::runtime_error(refusal);
    }
    return saved;
}

OrderEntry *OrderDataStorage::saveUnlessClOrdIdInUse(const OrderEntry &order, IdTValueGenerator *idGenerator,
                                                     PublishGuard *publishGuard, const char **refusal)
{
    assert(nullptr != refusal);
    if (aux::ExchLogger::instance()->isDebugOn())
    {
        aux::ExchLogger::instance()->debug("OrderDataStorage saving order");
    }

    assert(nullptr != idGenerator);

    // Drawn before the lock: a draw can save the next id limit to disk (#81), and lookups must not wait on that. An
    // order refused below wastes the id, which is harmless.
    const IdT newId = order.orderId_.isValid() ? IdT() : idGenerator->getId();
    // Made before the lock too, for the same reason (#713): the copy is over a kilobyte, and its instrument's load
    // takes WideDataStorage's lock. Nothing in it needs the store, nobody can reach it until it is inserted below, and
    // an order refused below frees it.
    std::unique_ptr<OrderEntry> cp(order.clone());
    if (!cp->orderId_.isValid())
    {
        cp->orderId_ = newId;
    }

    OrderEntry *result = nullptr;
    {
        // Exclusive write lock: the checks against both maps, and the insertion into both, at once
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
        if ((order.orderId_.isValid()) && (ordersById_.end() != ordersById_.find(order.orderId_)))
        {
            throw std::runtime_error("Unable to save order - order with same OrderId already exists.");
        }

        if (0 == order.clOrderId_.get().length_)
        {
            throw std::runtime_error("Unable to save order - order contains empty ClOrderId.");
        }
        if (ordersByClId_.end() != ordersByClId_.find(order.clOrderId_.get())) [[unlikely]]
        {
            *refusal = CLORDID_IN_USE;
            return nullptr;
        }

        // Lock the clone before it becomes reachable, so the caller can finish initialising it (status,
        // state machine persistence) before any other thread sees it. Nobody else can hold this entry's
        // lock yet, so taking it inside orderRwLock_ cannot deadlock. Mark it too, until the caller has
        // enqueued the transaction that books it (#83). Set before the insertion, under orderRwLock_, the
        // mark is seen by every lookup that finds the order.
        if (nullptr != publishGuard)
        {
            assert(!publishGuard->holds() && (nullptr == publishGuard->pending_));
            cp->entryMutex_.lock();
            publishGuard->locked_ = cp.get();
            cp->bookingPending_.store(true, std::memory_order_relaxed);
            publishGuard->pending_ = cp.get();
        }
        int st = 0;
        try
        {
            ordersById_.insert(OrdersByIDT::value_type(cp->orderId_, cp.get()));
            st = 1;
            ordersByClId_.insert(OrdersByClientIDT::value_type(cp->clOrderId_.get(), cp.get()));
            st = 2;
            result = cp.release();
        }
        catch (...)
        {
            switch (st)
            {
            case 2:
                ordersByClId_.erase(order.clOrderId_.get());
                [[fallthrough]];
            case 1:
                ordersById_.erase(cp->orderId_);
            }
            if ((nullptr != publishGuard) && (cp.get() == publishGuard->locked_))
            {
                publishGuard->release();
                publishGuard->pending_ = nullptr; // nobody found the clone, and it is freed on the way out
            }
            throw;
        }
    }
    return result;
}

void OrderDataStorage::restore(OrderEntry *order)
{
    if (aux::ExchLogger::instance()->isDebugOn())
    {
        aux::ExchLogger::instance()->debug("OrderDataStorage restoring order");
    }

    {
        // Exclusive write lock - atomic dual-map insert
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
        if ((order->orderId_.isValid()) && (ordersById_.end() != ordersById_.find(order->orderId_)))
        {
            throw std::runtime_error("Unable to restore order - order with same OrderId already exists.");
        }
        if (0 == order->clOrderId_.get().length_)
        {
            throw std::runtime_error("Unable to restore order - order contains empty ClOrderId.");
        }
        if (ordersByClId_.end() != ordersByClId_.find(order->clOrderId_.get()))
        {
            throw std::runtime_error("Unable to restore order - order with same ClOrderId already exists.");
        }
        // Restored from its record, which is durable: readers outside a transaction give it as that (#404). Set before
        // it is reachable.
        order->published_ = OrderFigures(*order);

        int st = 0;
        try
        {
            ordersById_.insert(OrdersByIDT::value_type(order->orderId_, order));
            st = 1;
            ordersByClId_.insert(OrdersByClientIDT::value_type(order->clOrderId_.get(), order));
            st = 2;
        }
        catch (...)
        {
            switch (st)
            {
            case 2:
                ordersByClId_.erase(order->clOrderId_.get());
                [[fallthrough]];
            case 1:
                ordersById_.erase(order->orderId_);
            }
            throw;
        }
    }
}

bool OrderDataStorage::drop(OrderEntry *order, DateTimeT day)
{
    assert(nullptr != order);
    // Its ClOrdID before the lock: save() and restore() read it as they stored the order, so this takes no lock
    const RawDataEntry &clOrderId = order->clOrderId_.get();
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
    const OrdersByIDT::iterator byId = ordersById_.find(order->orderId_);
    if ((ordersById_.end() == byId) || (order != byId->second))
    {
        return false;
    }
    // Room for it first, the steps that can throw, so that a throw changes nothing. Doubled, not grown by one: a
    // day's drops are many.
    std::vector<OrderEntry *> &held = dropped_[day];
    if (held.size() == held.capacity())
    {
        held.reserve(std::max<size_t>(64, 2 * held.capacity()));
    }
    const OrdersByClientIDT::iterator byClId = ordersByClId_.find(clOrderId);
    if ((ordersByClId_.end() != byClId) && (order == byClId->second))
    {
        ordersByClId_.erase(byClId);
    }
    ordersById_.erase(byId);
    held.push_back(order);
    if (droppedMaxOrderNumber_.load(std::memory_order_relaxed) < order->orderId_.id_)
    {
        droppedMaxOrderNumber_.store(order->orderId_.id_, std::memory_order_relaxed);
    }
    return true;
}

bool OrderDataStorage::undrop(OrderEntry *order)
{
    assert(nullptr != order);
    const RawDataEntry &clOrderId = order->clOrderId_.get();
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
    // From the latest: a rollback follows its own drop closely
    std::vector<OrderEntry *> *held = nullptr;
    std::vector<OrderEntry *>::reverse_iterator dropped;
    for (DroppedT::reverse_iterator day = dropped_.rbegin(); (dropped_.rend() != day) && (nullptr == held); ++day)
    {
        dropped = std::find(day->second.rbegin(), day->second.rend(), order);
        if (day->second.rend() != dropped)
        {
            held = &day->second;
        }
    }
    if (nullptr == held)
    {
        return false;
    }
    ordersById_.insert(OrdersByIDT::value_type(order->orderId_, order));
    try
    {
        // A new order that took its ClOrdID since the drop keeps it: insert() leaves the key as it is
        ordersByClId_.insert(OrdersByClientIDT::value_type(clOrderId, order));
    }
    catch (...)
    {
        // Still dropped, in neither map: in one, freeDropped() would free an order a lookup finds
        ordersById_.erase(order->orderId_);
        throw;
    }
    held->erase(std::next(dropped).base());
    return true;
}

size_t OrderDataStorage::freeDropped(DateTimeT before)
{
    DroppedT freed;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
        // The days before, a node at a time, which allocates nothing under the lock
        const DroppedT::iterator kept = dropped_.lower_bound(before);
        while (dropped_.begin() != kept)
        {
            freed.insert(dropped_.extract(dropped_.begin()));
        }
    }
    size_t count = 0;
    for (const DroppedT::value_type &day : freed)
    {
        for (OrderEntry *order : day.second)
        {
            delete order;
        }
        count += day.second.size();
    }
    return count;
}

size_t OrderDataStorage::droppedHeld() const
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
    size_t held = 0;
    for (const DroppedT::value_type &day : dropped_)
    {
        held += day.second.size();
    }
    return held;
}

void OrderDataStorage::eraseRecord(const IdT &orderId)
{
    if (nullptr != saver_)
    {
        saver_->eraseOrder(orderId);
    }
}

bool OrderDataStorage::persist(const OrderEntry &order, const OrderFigures &figures, u32 *version)
{
    assert(nullptr != version);
    if (nullptr == saver_)
    {
        return false;
    }
    *version = saver_->save(order, figures);
    return true;
}

void OrderDataStorage::unpersist(const IdT &orderId, u32 version)
{
    if (nullptr != saver_)
    {
        saver_->erase(orderId, version);
    }
}

bool OrderDataStorage::persistLastTrade(const SourceIdT &instrument, PriceT lastPx, const IdT &execId,
                                        const std::vector<IdT> &pendingReleases, u32 *version)
{
    assert(nullptr != version);
    if (nullptr == saver_)
    {
        return false;
    }
    *version = saver_->saveLastTrade(instrument, lastPx, execId, pendingReleases);
    return true;
}

void OrderDataStorage::unpersistLastTrade(const SourceIdT &instrument, u32 version)
{
    if (nullptr != saver_)
    {
        saver_->eraseLastTrade(instrument, version);
    }
}

void OrderDataStorage::beginPersistGroup()
{
    if (nullptr != saver_)
    {
        saver_->beginGroup();
    }
}

void OrderDataStorage::commitPersistGroup()
{
    if (nullptr != saver_)
    {
        saver_->commitGroup();
    }
}

void OrderDataStorage::abortPersistGroup() noexcept
{
    if (nullptr != saver_)
    {
        saver_->abortGroup();
    }
}
