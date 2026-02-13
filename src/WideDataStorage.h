/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <oneapi/tbb/spin_rw_mutex.h>
#include <atomic>
#include <map>
#include "Singleton.h"
#include "FileStorageDef.h"
#include "CacheAlignedAtomic.h"

namespace COP{

	struct InstrumentEntry;
	struct AccountEntry;
	struct ClearingEntry;

namespace Store{

	class DataSaver;

class WideParamsDataStorage: public DataStorageRestore
{
public:
	WideParamsDataStorage(void);
	~WideParamsDataStorage(void);

	void bindStorage(DataSaver *storage);

	void get(const SourceIdT &id, StringT *val)const;
	void get(const SourceIdT &id, RawDataEntry *val)const;
	void get(const SourceIdT &id, InstrumentEntry *val)const;
	void get(const SourceIdT &id, AccountEntry *val)const;
	void get(const SourceIdT &id, ClearingEntry *val)const;
	void get(const SourceIdT &id, ExecutionsT **val)const;

	SourceIdT add(InstrumentEntry *val);
	SourceIdT add(StringT *val);
	SourceIdT add(RawDataEntry *val);
	SourceIdT add(AccountEntry *val);
	SourceIdT add(ClearingEntry *val);
	SourceIdT add(ExecutionsT *val);
public:
	/// reimplementeed from DataStorageRestore
	virtual void restore(InstrumentEntry *val);
	virtual void restore(const IdT& id, StringT *val);
	virtual void restore(RawDataEntry *val);
	virtual void restore(AccountEntry *val);
	virtual void restore(ClearingEntry *val);
	virtual void restore(ExecutionsT *val);

private:
	/// Reader-writer lock for read-heavy reference data access
	/// Allows concurrent reads, exclusive writes
	mutable oneapi::tbb::spin_rw_mutex rwLock_;

	/// Cache-aligned to prevent false sharing with rwLock_
	CacheAlignedAtomic<u64> subscrCounter_;

	typedef std::map<SourceIdT, InstrumentEntry *> InstrumentsT;
	InstrumentsT instruments_;
	typedef std::map<SourceIdT, StringT *> StringsT;
	StringsT strings_;
	typedef std::map<SourceIdT, AccountEntry *> AccountsT;
	AccountsT accounts_;
	typedef std::map<SourceIdT, ClearingEntry *> ClearingsT;
	ClearingsT clearings_;
	typedef std::map<SourceIdT, RawDataEntry *> RawDataT;
	RawDataT rawDatas_;

	typedef std::map<SourceIdT, ExecutionsT *> ExecutionListsT;
	ExecutionListsT executions_;

	DataSaver *storage_;

	std::map<StringT, SourceIdT> instrumentsBySymbol_;
	std::map<StringT, SourceIdT> accountsByName_;

public:
	template<typename Fn> void forEachInstrument(Fn&& fn) const {
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
		for (const auto& [id, entry] : instruments_)
			fn(id, *entry);
	}

	template<typename Fn> void forEachAccount(Fn&& fn) const {
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
		for (const auto& [id, entry] : accounts_)
			fn(id, *entry);
	}

	SourceIdT findInstrumentBySymbol(const StringT& symbol) const {
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
		auto it = instrumentsBySymbol_.find(symbol);
		if (it == instrumentsBySymbol_.end())
			return SourceIdT();
		return it->second;
	}

	SourceIdT findAccountByName(const StringT& name) const {
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
		auto it = accountsByName_.find(name);
		if (it == accountsByName_.end())
			return SourceIdT();
		return it->second;
	}
};

typedef aux::Singleton<WideParamsDataStorage> WideDataStorage;

}}