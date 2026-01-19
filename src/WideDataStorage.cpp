/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/


#include "WideDataStorage.h"
#include "DataModelDef.h"
#include "Logger.h"

using namespace tbb;
using namespace COP;
using namespace COP::Store;

WideParamsDataStorage::WideParamsDataStorage(void): storage_(NULL)
{
	subscrCounter_.fetch_and_store(1);

	aux::ExchLogger::instance()->note("WideParamsDataStorage created");
}

WideParamsDataStorage::~WideParamsDataStorage(void)
{
	InstrumentsT tmp;
	{
		mutex::scoped_lock lock(lock_);
		std::swap(tmp, instruments_);
	}
	for(InstrumentsT::iterator it = tmp.begin(); it != tmp.end(); ++it)
		delete it->second;

	aux::ExchLogger::instance()->note("WideParamsDataStorage destroyed");
}

void WideParamsDataStorage::bindStorage(DataSaver *storage)
{
	assert(NULL == storage_);
	assert(NULL != storage);

	storage_ = storage;
}

void WideParamsDataStorage::get(const SourceIdT &id, StringT *val)const
{
	mutex::scoped_lock lock(lock_);
	StringsT::const_iterator it = strings_.find(id);
	if(strings_.end() == it)
		throw std::exception("WideParamsDataStorage::get(StringT): string not found!");
	*val = *(const_cast<StringT *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, RawDataEntry *val)const
{
	mutex::scoped_lock lock(lock_);
	RawDataT::const_iterator it = rawDatas_.find(id);
	if(rawDatas_.end() == it)
		throw std::exception("WideParamsDataStorage::get(RawDataEntry): rawData not found!");
	*val = *(const_cast<RawDataEntry *>(it->second));
}

void WideParamsDataStorage::get(const SourceIdT &id, InstrumentEntry *val)const
{
	{
		mutex::scoped_lock lock(lock_);
		InstrumentsT::const_iterator it = instruments_.find(id);
		if(instruments_.end() == it)
			throw std::exception("WideParamsDataStorage::get(InstrumentEntry): instrument not found!");
		*val = *(const_cast<InstrumentEntry *>(it->second));
	}
}

void WideParamsDataStorage::get(const SourceIdT &id, AccountEntry *val)const
{
	mutex::scoped_lock lock(lock_);
	AccountsT::const_iterator it = accounts_.find(id);
	if(accounts_.end() == it)
		throw std::exception("WideParamsDataStorage::get(AccountEntry): account not found!");
	*val = *(const_cast<AccountEntry *>(it->second));
}
void WideParamsDataStorage::get(const SourceIdT &id, ClearingEntry *val)const
{
	mutex::scoped_lock lock(lock_);
	ClearingsT::const_iterator it = clearings_.find(id);
	if(clearings_.end() == it)
		throw std::exception("WideParamsDataStorage::get(ClearingEntry): clearing not found!");
	*val = *(const_cast<ClearingEntry *>(it->second));
}

SourceIdT WideParamsDataStorage::add(InstrumentEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_and_increment(), 1);
	{
		mutex::scoped_lock lock(lock_);
		instruments_.insert(InstrumentsT::value_type(id, val));
		val->id_ = id;
	}
	if(NULL != storage_)
		storage_->save(*val);
	//aux::ExchLogger::instance()->debug("WideParamsDataStorage InstrumentEntry added");
	return id;
}

SourceIdT WideParamsDataStorage::add(StringT *val)
{
//	assert(false);

	SourceIdT id(subscrCounter_.fetch_and_increment(), 0);
	{
		mutex::scoped_lock lock(lock_);
		strings_.insert(StringsT::value_type(id, val));
	}
	if(NULL != storage_)
		storage_->save(id, *val);
	//aux::ExchLogger::instance()->debug("WideParamsDataStorage String added");
	return id;
}

SourceIdT WideParamsDataStorage::add(RawDataEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_and_increment(), 1);
	{
		mutex::scoped_lock lock(lock_);
		rawDatas_.insert(RawDataT::value_type(id, val));
		val->id_ = id;
	}
	if(NULL != storage_)
		storage_->save(*val);
	//aux::ExchLogger::instance()->debug("WideParamsDataStorage RawDataEntry added");
	return id;
}

SourceIdT WideParamsDataStorage::add(AccountEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_and_increment(), 1);
	{
		mutex::scoped_lock lock(lock_);
		accounts_.insert(AccountsT::value_type(id, val));
		val->id_ = id;
	}
	if(NULL != storage_)
		storage_->save(*val);
	//aux::ExchLogger::instance()->debug("WideParamsDataStorage AccountEntry added");
	return id;
}

SourceIdT WideParamsDataStorage::add(ClearingEntry *val)
{
	SourceIdT id(subscrCounter_.fetch_and_increment(), 1);
	{
		mutex::scoped_lock lock(lock_);
		clearings_.insert(ClearingsT::value_type(id, val));
		val->id_ = id;
	}
	if(NULL != storage_)
		storage_->save(*val);
	//aux::ExchLogger::instance()->debug("WideParamsDataStorage ClearingEntry added");
	return id;
}

void WideParamsDataStorage::get(const SourceIdT &id, ExecutionsT **val)const
{
	mutex::scoped_lock lock(lock_);
	ExecutionListsT::const_iterator it = executions_.find(id);
	if(executions_.end() == it)
		throw std::exception("WideParamsDataStorage::get(ExecutionsT): execution list not found!");
	*val = (const_cast<ExecutionsT *>(it->second));
}

SourceIdT WideParamsDataStorage::add(ExecutionsT *val)
{
	SourceIdT id(subscrCounter_.fetch_and_increment(), 1);
	{
		mutex::scoped_lock lock(lock_);
		executions_.insert(ExecutionListsT::value_type(id, val));
		//val->id_ = id;
	}
	if(NULL != storage_)
		storage_->save(*val);
	//aux::ExchLogger::instance()->debug("WideParamsDataStorage ExecutionsT added");
	return id;
}

void WideParamsDataStorage::restore(InstrumentEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.fetch_and_store(val->id_.id_ + 1);
	{
		mutex::scoped_lock lock(lock_);
		instruments_.insert(InstrumentsT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(const IdT& id, StringT *val)
{
	if(subscrCounter_ < id.id_)
		subscrCounter_.fetch_and_store(id.id_ + 1);
	{
		mutex::scoped_lock lock(lock_);
		strings_.insert(StringsT::value_type(id, val));
	}
}

void WideParamsDataStorage::restore(RawDataEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.fetch_and_store(val->id_.id_ + 1);
	{
		mutex::scoped_lock lock(lock_);
		rawDatas_.insert(RawDataT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(AccountEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.fetch_and_store(val->id_.id_ + 1);
	{
		mutex::scoped_lock lock(lock_);
		accounts_.insert(AccountsT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(ClearingEntry *val)
{
	if(subscrCounter_ < val->id_.id_)
		subscrCounter_.fetch_and_store(val->id_.id_ + 1);
	{
		mutex::scoped_lock lock(lock_);
		clearings_.insert(ClearingsT::value_type(val->id_, val));
	}
}

void WideParamsDataStorage::restore(ExecutionsT * /*val*/)
{
	///todo: implement
}
