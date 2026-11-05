/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <algorithm>
#include <stdexcept>
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "FileStorageDef.h"
#include "Logger.h"

using namespace COP;
using namespace tbb;
using namespace std;
using namespace COP::Store;

namespace
{
enum LogMessages
{
    INVALID_MSG = 0,
    ADDORDER_FINAL_MSG,
    ADDORDER_START_MSG,
    REMOVEORDER_FINAL_MSG,
    REMOVEORDER_START_MSG,
    RESTOREORDER_FINAL_MSG,
    RESTOREORDER_START_MSG,

};

class OBLogMessage : public aux::LogMessage
{
public:
    OBLogMessage(LogMessages type, const IdT &id) : type_(type), id_(id) {}

    virtual void prepareMessage()
    {
        switch (type_)
        {
        case ADDORDER_FINAL_MSG:
            msg.reserve(128);
            msg = "OrderBookImpl Added order '";
            id_.toString(msg);
            msg += "' into OrderBook.";
            break;
        case ADDORDER_START_MSG:
            msg.reserve(128);
            msg = "OrderBookImpl Adding order '";
            id_.toString(msg);
            msg += "' into OrderBook.";
            break;
        case REMOVEORDER_START_MSG:
            msg.reserve(128);
            msg = "OrderBookImpl Removing order '";
            id_.toString(msg);
            msg += "' from OrderBook.";
            break;
        case REMOVEORDER_FINAL_MSG:
            msg.reserve(128);
            msg = "OrderBookImpl Finish removing order '";
            id_.toString(msg);
            msg += "' from OrderBook.";
            break;
        case RESTOREORDER_FINAL_MSG:
            msg.reserve(128);
            msg = "OrderBookImpl restored order '";
            id_.toString(msg);
            msg += "' into OrderBook.";
            break;
        case RESTOREORDER_START_MSG:
            msg.reserve(128);
            msg = "OrderBookImpl restoring order '";
            id_.toString(msg);
            msg += "' into OrderBook.";
            break;
        default:
            assert(false);
            throw std::runtime_error("OBLogMessage used with invalid type!");
        };
    }

private:
    OBLogMessage &operator=(const OBLogMessage &);
    LogMessages type_;
    const IdT &id_;
};

} // namespace

OrderBookImpl::OrderBookImpl(void) {}

OrderBookImpl::~OrderBookImpl(void)
{
    OrderGroupsByInstrumentT tmp;
    swap(tmp, orderGroups_);
    for (OrderGroupsByInstrumentT::const_iterator it = tmp.begin(); it != tmp.end(); ++it)
    {
        delete it->second;
    }
}

void OrderBookImpl::init(const InstrumentsT &instr)
{
    for (InstrumentsT::const_iterator it = instr.begin(); it != instr.end(); ++it)
    {
        std::unique_ptr<OrdersGroup> grp(new OrdersGroup);
        orderGroups_.insert(OrderGroupsByInstrumentT::value_type(*it, grp.get()));
        grp.release();
    }
}

void OrderBookImpl::add(const OrderEntry &order)
{
    assert(order.orderId_.isValid());
    assert(order.instrument_.getId().isValid());
    OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
    if (orderGroups_.end() == it)
    {
        throw std::runtime_error("Unable to add order into book - instrument not registered!");
    }
    if (BUY_SIDE == order.side_)
    {
        tbb::mutex::scoped_lock lock(it->second->buyLock_);
        it->second->buyOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));
    }
    else if (SELL_SIDE == order.side_)
    {
        tbb::mutex::scoped_lock lock(it->second->sellLock_);
        it->second->sellOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));
    }
    else
    {
        throw std::runtime_error("Unable to add order into book - side is not supported!");
    }

    if (aux::ExchLogger::instance()->isNoteOn())
    {
        OBLogMessage msg(ADDORDER_FINAL_MSG, order.orderId_);
        aux::ExchLogger::instance()->note(msg);
    }
}

void OrderBookImpl::restore(const OrderEntry &order)
{
    assert(order.orderId_.isValid());
    assert(order.instrument_.getId().isValid());
    OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
    if (orderGroups_.end() == it)
    {
        throw std::runtime_error("Unable to restore order into book - instrument not registered!");
    }
    if (BUY_SIDE == order.side_)
    {
        tbb::mutex::scoped_lock lock(it->second->buyLock_);
        it->second->buyOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));
    }
    else if (SELL_SIDE == order.side_)
    {
        tbb::mutex::scoped_lock lock(it->second->sellLock_);
        it->second->sellOrder_.insert(OrdersByPriceAscT::value_type(order.price_, order.orderId_));
    }
    else
    {
        throw std::runtime_error("Unable to restore order into book - side is not supported!");
    }

    if (aux::ExchLogger::instance()->isNoteOn())
    {
        OBLogMessage msg(RESTOREORDER_FINAL_MSG, order.orderId_);
        aux::ExchLogger::instance()->note(msg);
    }
}

void OrderBookImpl::remove(const OrderEntry &order)
{
    assert(order.instrument_.getId().isValid());
    if (aux::ExchLogger::instance()->isDebugOn())
    {
        OBLogMessage msg(REMOVEORDER_START_MSG, order.orderId_);
        aux::ExchLogger::instance()->debug(msg);
    }

    OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
    if (orderGroups_.end() == it)
    {
        throw std::runtime_error("Unable to remove order from book - instrument not registered!");
    }
    bool found = false;
    if (BUY_SIDE == order.side_)
    {
        tbb::mutex::scoped_lock lock(it->second->buyLock_);
        // buyOrder_ uses descending order (highest price first for buy side)
        // lower_bound finds first element with key <= price in descending order
        OrdersByPriceDescT::iterator oit = it->second->buyOrder_.lower_bound(order.price_);
        while ((it->second->buyOrder_.end() != oit) && (order.price_ == oit->first))
        {
            found = oit->second == order.orderId_;
            if (found)
            {
                it->second->buyOrder_.erase(oit);
                break;
            }
            ++oit;
        }
        if (!found)
        {
            throw std::runtime_error("Unable to remove order from book - order not found!");
        }
    }
    else if (SELL_SIDE == order.side_)
    {
        tbb::mutex::scoped_lock lock(it->second->sellLock_);
        OrdersByPriceAscT::iterator oit = it->second->sellOrder_.lower_bound(order.price_);
        while ((it->second->sellOrder_.end() != oit) && (order.price_ == oit->first))
        {
            found = oit->second == order.orderId_;
            if (found)
            {
                it->second->sellOrder_.erase(oit);
                break;
            }
            ++oit;
        }
        if (!found)
        {
            throw std::runtime_error("Unable to remove order from book - order not found!");
        }
    }
    else
    {
        // No order on another side is in the book: add() and restore() refuse one. An order stored on one before
        // isValid() refused it is restored out of the book, so its cancel or expiry has nothing to take out (#510).
        return;
    }

    if (aux::ExchLogger::instance()->isNoteOn())
    {
        OBLogMessage msg(REMOVEORDER_FINAL_MSG, order.orderId_);
        aux::ExchLogger::instance()->note(msg);
    }
}

namespace
{
/// Erases (price, id) from a multimap of one side's stops, and whether it was there
template <typename StopsT> bool eraseStop(StopsT *stops, PriceT price, const IdT &id)
{
    for (auto range = stops->equal_range(price); range.first != range.second; ++range.first)
    {
        if (id == range.first->second)
        {
            stops->erase(range.first);
            return true;
        }
    }
    return false;
}

/// Whether a multimap of one side's stops holds (price, id)
template <typename StopsT> bool holdsStop(const StopsT &stops, PriceT price, const IdT &id)
{
    for (auto range = stops.equal_range(price); range.first != range.second; ++range.first)
    {
        if (id == range.first->second)
        {
            return true;
        }
    }
    return false;
}
} // namespace

void OrderBookImpl::addStop(const OrderEntry &order)
{
    assert(order.orderId_.isValid());
    OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
    if (orderGroups_.end() == it)
    {
        throw std::runtime_error("Unable to hold stop order - instrument not registered!");
    }
    OrdersGroup &group = *it->second;
    tbb::mutex::scoped_lock lock(group.stopLock_);
    if (BUY_SIDE == order.side_)
    {
        group.buyStops_.insert(OrdersByPriceAscT::value_type(order.stopPx_, order.orderId_));
    }
    else if (SELL_SIDE == order.side_)
    {
        group.sellStops_.insert(OrdersByPriceDescT::value_type(order.stopPx_, order.orderId_));
    }
    else
    {
        throw std::runtime_error("Unable to hold stop order - side is not supported!");
    }
    group.stopCount_.fetch_add(1, std::memory_order_relaxed);
}

void OrderBookImpl::removeStop(const OrderEntry &order)
{
    OrderGroupsByInstrumentT::iterator it = orderGroups_.find(order.instrument_.getId());
    if (orderGroups_.end() == it)
    {
        throw std::runtime_error("Unable to remove stop order - instrument not registered!");
    }
    if ((BUY_SIDE != order.side_) && (SELL_SIDE != order.side_))
    {
        return; // never held: addStop() refuses it, and a restore leaves it out (#510), as remove() says
    }
    OrdersGroup &group = *it->second;
    tbb::mutex::scoped_lock lock(group.stopLock_);
    const bool erased = (BUY_SIDE == order.side_) ? eraseStop(&group.buyStops_, order.stopPx_, order.orderId_)
                                                  : eraseStop(&group.sellStops_, order.stopPx_, order.orderId_);
    if (!erased)
    {
        throw std::runtime_error("Unable to remove stop order - order not held!");
    }
    group.stopCount_.fetch_sub(1, std::memory_order_relaxed);
    // A release pending is pending no longer: it took the stop out, or a cancel or a replace did first (#518)
    const auto pending = std::find(group.pendingReleases_.begin(), group.pendingReleases_.end(), order.orderId_);
    if (group.pendingReleases_.end() != pending)
    {
        group.pendingReleases_.erase(pending);
    }
}

bool OrderBookImpl::markReleasePending(const OrderEntry &stop)
{
    OrderGroupsByInstrumentT::iterator it = orderGroups_.find(stop.instrument_.getId());
    if ((orderGroups_.end() == it) || ((BUY_SIDE != stop.side_) && (SELL_SIDE != stop.side_)))
    {
        return false; // never held: addStop() refuses it
    }
    OrdersGroup &group = *it->second;
    tbb::mutex::scoped_lock lock(group.stopLock_);
    const bool held = (BUY_SIDE == stop.side_) ? holdsStop(group.buyStops_, stop.stopPx_, stop.orderId_)
                                               : holdsStop(group.sellStops_, stop.stopPx_, stop.orderId_);
    if (held && (group.pendingReleases_.end() ==
                 std::find(group.pendingReleases_.begin(), group.pendingReleases_.end(), stop.orderId_)))
    {
        group.pendingReleases_.push_back(stop.orderId_);
    }
    return held;
}

void OrderBookImpl::pendingReleases(const SourceIdT &instrument, std::vector<IdT> *result) const
{
    assert(nullptr != result);
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(instrument);
    if (orderGroups_.end() == it)
    {
        return;
    }
    const OrdersGroup &group = *it->second;
    tbb::mutex::scoped_lock lock(group.stopLock_);
    result->insert(result->end(), group.pendingReleases_.begin(), group.pendingReleases_.end());
}

bool OrderBookImpl::holdsStops(const SourceIdT &instrument) const
{
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(instrument);
    return (orderGroups_.end() != it) && (0 != it->second->stopCount_.load(std::memory_order_relaxed));
}

void OrderBookImpl::triggeredStops(const SourceIdT &instrument, PriceT lastPx, OrdersT *result) const
{
    assert(nullptr != result);
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(instrument);
    if (orderGroups_.end() == it)
    {
        return;
    }
    const OrdersGroup &group = *it->second;
    tbb::mutex::scoped_lock lock(group.stopLock_);
    // A trade at lastPx reaches a buy stop at or below it, and a sell stop at or above it
    for (auto sit = group.buyStops_.begin(); (group.buyStops_.end() != sit) && (sit->first <= lastPx); ++sit)
    {
        result->push_back(sit->second);
    }
    for (auto sit = group.sellStops_.begin(); (group.sellStops_.end() != sit) && (sit->first >= lastPx); ++sit)
    {
        result->push_back(sit->second);
    }
}

IdT OrderBookImpl::find(const OrderFunctor &functor) const
{
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(functor.instrument());
    if (orderGroups_.end() != it)
    {
        if (BUY_SIDE == functor.side())
        {
            bool stop = false;
            tbb::mutex::scoped_lock lock(it->second->buyLock_);
            for (OrdersByPriceDescT::const_iterator oit = it->second->buyOrder_.begin();
                 oit != it->second->buyOrder_.end(); ++oit)
            {
                if (functor.match(oit->second, &stop))
                {
                    return oit->second;
                }
                if (stop)
                {
                    break;
                }
            }
        }
        else if (SELL_SIDE == functor.side())
        {
            bool stop = false;
            tbb::mutex::scoped_lock lock(it->second->sellLock_);
            for (OrdersByPriceAscT::const_iterator oit = it->second->sellOrder_.begin();
                 oit != it->second->sellOrder_.end(); ++oit)
            {
                if (functor.match(oit->second, &stop))
                {
                    return oit->second;
                }
                if (stop)
                {
                    break;
                }
            }
        }
        else
        {
            throw std::runtime_error("Invalid Side for the order's search");
        }
    }
    else
    {
        throw std::runtime_error("Unknown instrument for the order's search");
    }
    return IdT();
}

void OrderBookImpl::findAll(const OrderFunctor &functor, OrdersT *result) const
{
    assert(nullptr != result);
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(functor.instrument());
    if (orderGroups_.end() != it)
    {
        if (BUY_SIDE == functor.side())
        {
            bool stop = false;
            tbb::mutex::scoped_lock lock(it->second->buyLock_);
            for (OrdersByPriceDescT::const_iterator oit = it->second->buyOrder_.begin();
                 oit != it->second->buyOrder_.end(); ++oit)
            {
                if (functor.match(oit->second, &stop))
                {
                    result->push_back(oit->second);
                }
                if (stop)
                {
                    break;
                }
            }
        }
        else if (SELL_SIDE == functor.side())
        {
            bool stop = false;
            tbb::mutex::scoped_lock lock(it->second->sellLock_);
            for (OrdersByPriceAscT::const_iterator oit = it->second->sellOrder_.begin();
                 oit != it->second->sellOrder_.end(); ++oit)
            {
                if (functor.match(oit->second, &stop))
                {
                    result->push_back(oit->second);
                }
                if (stop)
                {
                    break;
                }
            }
        }
        else
        {
            throw std::runtime_error("Invalid Side for the order's search");
        }
    }
    else
    {
        throw std::runtime_error("Unknown instrument for the order's search");
    }
}

IdT OrderBookImpl::getTop(const SourceIdT &instrument, const Side &side) const
{
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(instrument);
    if (orderGroups_.end() != it)
    {
        if (BUY_SIDE == side)
        {
            tbb::mutex::scoped_lock lock(it->second->buyLock_);
            if (0 < it->second->buyOrder_.size())
            {
                return it->second->buyOrder_.begin()->second;
            }
        }
        else if (SELL_SIDE == side)
        {
            tbb::mutex::scoped_lock lock(it->second->sellLock_);
            if (0 < it->second->sellOrder_.size())
            {
                return it->second->sellOrder_.begin()->second;
            }
        }
        else
        {
            throw std::runtime_error("Invalid Side for the order's top");
        }
    }
    else
    {
        throw std::runtime_error("Unknown instrument for the order's top");
    }
    return IdT();
}

namespace
{
/// A side's resting orders, by price, as getSnapshot() copies them under the side's lock (#525)
typedef std::vector<std::pair<PriceT, IdT>> RestingOrdersT;

/// Copies a side's resting orders in one walk: assign() walks the side twice, to count it and to copy it (#525)
template <typename OrdersT> void copyResting(const OrdersT &side, RestingOrdersT *resting)
{
    resting->clear();
    resting->reserve(side.size());
    for (const auto &[price, orderId] : side)
    {
        resting->emplace_back(price, orderId);
    }
}

/// A side's levels, from its resting orders, each with the leaves its last report gave (#404), read under its read lock
/// with no book lock held (#525)
template <typename Compare>
void aggregateLevels(const RestingOrdersT &resting, const OrderDataStorage *storage, std::vector<BookLevel> *levels)
{
    std::map<PriceT, BookLevel, Compare> byPrice;
    for (const auto &[price, orderId] : resting)
    {
        OrderEntry *order = storage->locateByOrderId(orderId);
        if (nullptr == order)
        {
            continue;
        }
        QuantityT leaves = 0;
        {
            oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
            if (order->published_)
            {
                leaves = order->published_->leavesQty_;
            }
        }
        // Its first report isn't out, and the update that report sends follows, or its last report left it nothing to
        // trade: a cancel, an expiry or a replace zeroes the leaves at the decision, and takes the order out of the
        // book in a later operation
        if (0 == leaves)
        {
            continue;
        }
        BookLevel &level = byPrice[price];
        level.price = price;
        level.totalQty += leaves;
        level.orderCount++;
    }
    levels->reserve(byPrice.size());
    for (const auto &[price, level] : byPrice)
    {
        levels->push_back(level);
    }
}
} // namespace

BookSnapshot OrderBookImpl::getSnapshot(const SourceIdT &instrument, const Store::OrderDataStorage *storage) const
{
    BookSnapshot snap;
    OrderGroupsByInstrumentT::const_iterator it = orderGroups_.find(instrument);
    if (orderGroups_.end() == it)
    {
        return snap;
    }

    // Each side's orders are copied under its lock and read once it is released: never a book lock and an order's at
    // once (#525). A fill's decision holds the order's write lock while it takes a filled order out of the book
    // (processFill calls remove()), and the report whose push builds this runs in a transaction that doesn't name the
    // instrument, so beside its fills. The matcher takes the side's lock and then an order's, but only within the
    // instrument's chain, as its fills are decided. So the depth is the book's as of the copy: an order that leaves it
    // before its read shows its last reported leaves, and one booked meanwhile is missed, until the next update.
    // The thread keeps its copy from one snapshot to the next: a side of 128 orders is 3 KB, past glibc's tcache, and
    // a copy made for each report cost it ~30,000 instructions in the allocator (#525)
    static thread_local RestingOrdersT resting;
    {
        tbb::mutex::scoped_lock lock(it->second->buyLock_);
        copyResting(it->second->buyOrder_, &resting);
    }
    aggregateLevels<PriceTDescend>(resting, storage, &snap.bids);
    {
        tbb::mutex::scoped_lock lock(it->second->sellLock_);
        copyResting(it->second->sellOrder_, &resting);
    }
    aggregateLevels<PriceTAscend>(resting, storage, &snap.asks);
    return snap;
}
