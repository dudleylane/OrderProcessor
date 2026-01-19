/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/


#include <stdexcept>
#include "WideDataStorage.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;

WideParamsDataStorage::WideParamsDataStorage(void): storage_(nullptr)
{
	subscrCounter_.store(1);

	aux::ExchLogger::instance()->note("WideParamsDataStorage created");
}

WideParamsDataStorage::~WideParamsDataStorage(void)
{
	InstrumentsT tmp;
	{
		// Exclusive lock for destruction
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		std::swap(tmp, instruments_);
	}
	for(InstrumentsT::iterator it = tmp.begin(); it != tmp.end(); ++it)
		delete it->second;

	aux::ExchLogger::instance()->note("WideParamsDataStorage destroyed");
}

void WideParamsDataStorage::bindStorage(DataSaver *storage)
{
	assert(nullptr == storage_);
	assert(nullptr != storage);

	storage_ = storage;
}

// ============================================================================
// Read operations - use shared (read) locks for concurrent access
// ============================================================================

void WideParamsDataStorage::get(const SourceIdT &id, StringT *val)const
{
	// Shared read lock - allows concurrent readers
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
	StringsT::const_iterator it = strings_.find(id);
	if(strings_.end() == it)
		throw std::runtime_error("WideParamsDataStorage::get(StringT): string not found!");
	*val = *(const_cast<StringT *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, RawDataEntry *val)const
{
	// Shared read lock - allows concurrent readers
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
	RawDataT::const_iterator it = rawDatas_.find(id);
	if(rawDatas_.end() == it)
		throw std::runtime_error("WideParamsDataStorage::get(RawDataEntry): rawData not found!");
	*val = *(const_cast<RawDataEntry *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, InstrumentEntry *val)const
{
	// Shared read lock - allows concurrent readers
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
	InstrumentsT::const_iterator it = instruments_.find(id);
	if(instruments_.end() == it)
		throw std::runtime_error("WideParamsDataStorage::get(InstrumentEntry): instrument not found!");
	*val = *(const_cast<InstrumentEntry *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, AccountEntry *val)const
{
	// Shared read lock - allows concurrent readers
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
	AccountsT::const_iterator it = accounts_.find(id);
	if(accounts_.end() == it)
		throw std::runtime_error("WideParamsDataStorage::get(AccountEntry): account not found!");
	*val = *(const_cast<AccountEntry *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, ClearingEntry *val)const
{
	// Shared read lock - allows concurrent readers
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
	ClearingsT::const_iterator it = clearings_.find(id);
	if(clearings_.end() == it)
		throw std::runtime_error("WideParamsDataStorage::get(ClearingEntry): clearing not found!");
	*val = *(const_cast<ClearingEntry *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, ExecutionsT **val)const
{
	// Shared read lock - allows concurrent readers
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
	ExecutionListsT::const_iterator it = executions_.find(id);
	if(executions_.end() == it)
		throw std::runtime_error("WideParamsDataStorage::get(ExecutionsT): execution list not found!");
	*val = (const_cast<ExecutionsT *>(it->second));
}

// ============================================================================
// Write operations - use exclusive (write) locks
// ============================================================================

SourceIdT WideParamsDataStorage::add(InstrumentEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_add(1), 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		instruments_.insert(InstrumentsT::value_type(id, val));
		val->id_ = id;
	}
	if(nullptr != storage_)
		storage_->save(*val);
	return id;
}

SourceIdT WideParamsDataStorage::add(StringT *val)
{
	SourceIdT id(subscrCounter_.fetch_add(1), 0);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		strings_.insert(StringsT::value_type(id, val));
	}
	if(nullptr != storage_)
		storage_->save(id, *val);
	return id;
}

SourceIdT WideParamsDataStorage::add(RawDataEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_add(1), 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		rawDatas_.insert(RawDataT::value_type(id, val));
		val->id_ = id;
	}
	if(nullptr != storage_)
		storage_->save(*val);
	return id;
}

SourceIdT WideParamsDataStorage::add(AccountEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_add(1), 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		accounts_.insert(AccountsT::value_type(id, val));
		val->id_ = id;
	}
	if(nullptr != storage_)
		storage_->save(*val);
	return id;
}

SourceIdT WideParamsDataStorage::add(ClearingEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_add(1), 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		clearings_.insert(ClearingsT::value_type(id, val));
		val->id_ = id;
	}
	if(nullptr != storage_)
		storage_->save(*val);
	return id;
}

SourceIdT WideParamsDataStorage::add(ExecutionsT *val)
{
	SourceIdT id(subscrCounter_.fetch_add(1), 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		executions_.insert(ExecutionListsT::value_type(id, val));
	}
	if(nullptr != storage_)
		storage_->save(*val);
	return id;
}

// ============================================================================
// Restore operations - use exclusive (write) locks
// ============================================================================

void WideParamsDataStorage::restore(InstrumentEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.store(val->id_.id_ + 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		instruments_.insert(InstrumentsT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(const IdT& id, StringT *val)
{
	if(subscrCounter_ < id.id_)
		subscrCounter_.store(id.id_ + 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		strings_.insert(StringsT::value_type(id, val));
	}
}

void WideParamsDataStorage::restore(RawDataEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.store(val->id_.id_ + 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		rawDatas_.insert(RawDataT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(AccountEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.store(val->id_.id_ + 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		accounts_.insert(AccountsT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(ClearingEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.store(val->id_.id_ + 1);
	{
		// Exclusive write lock
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
		clearings_.insert(ClearingsT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(ExecutionsT * /*val*/)
{
	///todo: implement
}
