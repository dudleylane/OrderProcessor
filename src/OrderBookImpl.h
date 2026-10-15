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
    QuantityT totalQty;
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

    BookSnapshot getSnapshot(const SourceIdT &instrument, const Store::OrderDataStorage *storage) const;

private:
    typedef std::multimap<PriceT, IdT, PriceTAscend> OrdersByPriceAscT;
    typedef std::multimap<PriceT, IdT, PriceTDescend> OrdersByPriceDescT;
    struct OrdersGroup
    {
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
    };

    typedef std::map<IdT, OrdersGroup *> OrderGroupsByInstrumentT;

    OrderGroupsByInstrumentT orderGroups_;
};

} // namespace COP
