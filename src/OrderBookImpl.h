/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <map>
#include <tbb/mutex.h>

#include "DataModelDef.h"

namespace COP{

	class OrderBookImpl: public OrderBook
	{
	public:
		typedef std::set<SourceIdT> InstrumentsT;
	public:
		OrderBookImpl(void);
		~OrderBookImpl(void);

		void init(const InstrumentsT &instr, OrderSaver *storage);

	public:
		virtual void add(const OrderEntry& order);
		virtual void remove(const OrderEntry& order);
		virtual IdT find(const OrderFunctor &functor)const;
		virtual void findAll(const OrderFunctor &functor, OrdersT *result)const;

		virtual IdT getTop(const SourceIdT &instrument, const Side &side)const;

		virtual void restore(const OrderEntry& order);
	private:
		typedef std::multimap<PriceT, IdT, PriceTAscend> OrdersByPriceAscT;
		typedef std::multimap<PriceT, IdT, PriceTDescend> OrdersByPriceDescT;
		struct OrdersGroup{
			mutable tbb::mutex buyLock_;
			OrdersByPriceDescT buyOrder_;

			mutable tbb::mutex sellLock_;
			OrdersByPriceAscT sellOrder_;
		};

		typedef std::map<IdT, OrdersGroup *> OrderGroupsByInstrumentT;

		OrderGroupsByInstrumentT orderGroups_;

		OrderSaver *storage_;
	};

}

