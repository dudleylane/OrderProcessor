/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <tbb/mutex.h>
#include <tbb/atomic.h>
#include <map>
#include "Singleton.h"
#include "FileStorageDef.h"

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
	mutable tbb::mutex lock_;

	tbb::atomic<u64> subscrCounter_;

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
};

typedef aux::Singleton<WideParamsDataStorage> WideDataStorage;

}}