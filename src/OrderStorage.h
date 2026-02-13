/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "Singleton.h"
#include "TypesDef.h"
#include <oneapi/tbb/spin_rw_mutex.h>
#include <oneapi/tbb/concurrent_hash_map.h>
#include <map>
#include "DataModelDef.h"

namespace COP{
	struct OrderEntry;
	struct ExecutionEntry;
	class IdTValueGenerator;

	namespace Store{

class OrderDataStorage
{
public:
	explicit OrderDataStorage();
	~OrderDataStorage(void);

	void attach(OrderSaver *saver);
public:
	OrderEntry *locateByClOrderId(const RawDataEntry &clOrderId)const;
	OrderEntry *locateByOrderId(const IdT &orderId)const;
	OrderEntry *save(const OrderEntry &order, IdTValueGenerator *idGenerator);
	void restore(OrderEntry *order);

	template<typename Fn> void forEachOrder(Fn&& fn) const {
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
		for (const auto& [id, entry] : ordersById_)
			fn(id, *entry);
	}

	ExecutionEntry *locateByExecId(const IdT &execId)const;
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
	struct SourceIdTHash {
		size_t hash(const SourceIdT &key) const {
			// Combine id_ and date_ for hash
			return std::hash<u64>()(key.id_) ^ (std::hash<u32>()(key.date_) << 1);
		}
		bool equal(const SourceIdT &a, const SourceIdT &b) const {
			return a == b;
		}
	};

	/// Lock-free concurrent hash map for executions (single-map operations)
	typedef oneapi::tbb::concurrent_hash_map<SourceIdT, ExecutionEntry*, SourceIdTHash> ExecByIDT;
	ExecByIDT executionsById_;

	OrderSaver *saver_;
};

typedef aux::Singleton<OrderDataStorage> OrderStorage;
	}
}