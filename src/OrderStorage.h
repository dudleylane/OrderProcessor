/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "Singleton.h"
#include "TypesDef.h"
#include <tbb/mutex.h>
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

	ExecutionEntry *locateByExecId(const IdT &execId)const;
	void save(const ExecutionEntry *exec);
	ExecutionEntry *save(const ExecutionEntry &exec, IdTValueGenerator *idGenerator);

private:
	mutable tbb::mutex orderLock_;

	typedef std::map<IdT, OrderEntry *> OrdersByIDT;
	OrdersByIDT ordersById_;

	typedef std::map<RawDataEntry, OrderEntry *> OrdersByClientIDT;
	OrdersByClientIDT ordersByClId_;

	mutable tbb::mutex execLock_;
	typedef std::map<SourceIdT, ExecutionEntry *> ExecByIDT;
	ExecByIDT executionsById_;

	OrderSaver *saver_;
};

typedef aux::Singleton<OrderDataStorage> OrderStorage;
	}
}