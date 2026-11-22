/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <map>
#include <vector>
#include <oneapi/tbb/mutex.h>

#include "DataModelDef.h"

namespace COP
{

namespace Store
{
class OrderDataStorage;
}

struct BookLevel
{
    PriceT price;
    /// A sum of the orders' leaves, each a QuantityT: in one, a level past 4,294,967,295 wrapped (#525)
    u64 totalQty;
    u32 orderCount;
};

struct BookSnapshot
{
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

class OrderBookImpl : public OrderBook
{
public:
    typedef std::set<SourceIdT> InstrumentsT;

public:
    OrderBookImpl(void);
    ~OrderBookImpl(void);

    void init(const InstrumentsT &instr);

public:
    virtual void add(const OrderEntry &order);
    virtual void remove(const OrderEntry &order);
    virtual IdT find(const OrderFunctor &functor) const;
    virtual void findAll(const OrderFunctor &functor, OrdersT *result) const;

    virtual IdT getTop(const SourceIdT &instrument, const Side &side) const;

    virtual void restore(const OrderEntry &order);

    virtual void addStop(const OrderEntry &order);
    virtual void removeStop(const OrderEntry &order);
    virtual bool holdsStops(const SourceIdT &instrument) const;
    virtual void triggeredStops(const SourceIdT &instrument, PriceT lastPx, OrdersT *result) const;
    virtual bool markReleasePending(const OrderEntry &stop);
    virtual void pendingReleases(const SourceIdT &instrument, std::vector<IdT> *result) const;

    /// The instrument's depth as its orders' last reports give it (#404): each resting order's reported leaves, read
    /// under the order's read lock once the side's lock is released (#525). An order whose first report isn't out, or
    /// whose last report left it nothing to trade, is left out.
    BookSnapshot getSnapshot(const SourceIdT &instrument, const Store::OrderDataStorage *storage) const;

private:
    typedef std::multimap<PriceT, IdT, PriceTAscend> OrdersByPriceAscT;
    typedef std::multimap<PriceT, IdT, PriceTDescend> OrdersByPriceDescT;
    struct OrdersGroup
    {
        /// Each side's lock, and the stop book's below. The lock order (#636; docs/ARCHITECTURE.md section 6.2):
        /// inside the instrument's chain a book lock and an order's come either way round (the matcher reads a
        /// contra under its side's lock; a fill's decision takes its order out of the book under the order's write
        /// lock), since the chain runs them one at a time. Outside it, never hold one with an order's lock (#525).
        mutable oneapi::tbb::mutex buyLock_;
        OrdersByPriceDescT buyOrder_;

        mutable oneapi::tbb::mutex sellLock_;
        OrdersByPriceAscT sellOrder_;

        /// Held stop orders, by stop price (#223): buy stops from the lowest, which a rising trade reaches first, and
        /// sell stops from the highest. Orders at one stop price keep the order they arrived in.
        mutable oneapi::tbb::mutex stopLock_;
        OrdersByPriceAscT buyStops_;
        OrdersByPriceDescT sellStops_;
        /// How many stops are held, read without the lock: transactions on the instrument run one at a time, and the
        /// lock comes in only when there is a stop to find
        std::atomic<u32> stopCount_{ 0 };
        /// The held stops whose releases are pending, in the order they were marked (#518). Each is held: removeStop()
        /// drops it.
        std::vector<IdT> pendingReleases_;
    };

    typedef std::map<IdT, OrdersGroup *> OrderGroupsByInstrumentT;

    OrderGroupsByInstrumentT orderGroups_;
};

} // namespace COP
