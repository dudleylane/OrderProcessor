/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <oneapi/tbb/spin_rw_mutex.h>
#include <atomic>
#include <map>
#include "NodePool.h"
#include "Singleton.h"
#include "FileStorageDef.h"
#include "CacheAlignedAtomic.h"

namespace COP
{

struct InstrumentEntry;
struct AccountEntry;
struct ClearingEntry;

namespace Store
{

class DataSaver;

class WideParamsDataStorage : public DataStorageRestore
{
public:
    WideParamsDataStorage(void);
    ~WideParamsDataStorage(void);

    void bindStorage(DataSaver *storage);

    void get(const SourceIdT &id, StringT *val) const;
    void get(const SourceIdT &id, RawDataEntry *val) const;
    void get(const SourceIdT &id, InstrumentEntry *val) const;
    void get(const SourceIdT &id, AccountEntry *val) const;
    void get(const SourceIdT &id, ClearingEntry *val) const;
    void get(const SourceIdT &id, ExecutionsT **val) const;

    SourceIdT add(InstrumentEntry *val);
    SourceIdT add(StringT *val);
    SourceIdT add(RawDataEntry *val);
    /// An order's own ClOrdID, source and destination, stored as add() stores them but not written to the storage it is
    /// bound to: the order's record carries them (#192). Each was written at once, in a commit of its own, on the
    /// thread that took the order.
    SourceIdT addUnsaved(StringT *val);
    SourceIdT addUnsaved(RawDataEntry *val);
    /// The entry with this id, when the storage has it: get() throws for one it doesn't (#192)
    bool find(const SourceIdT &id, StringT *val) const override;
    bool find(const SourceIdT &id, RawDataEntry *val) const override;
    SourceIdT add(AccountEntry *val);
    SourceIdT add(ClearingEntry *val);
    SourceIdT add(ExecutionsT *val);

public:
    /// reimplementeed from DataStorageRestore
    virtual void restore(InstrumentEntry *val);
    virtual void restore(const IdT &id, StringT *val);
    virtual void restore(RawDataEntry *val);
    virtual void restore(AccountEntry *val);
    virtual void restore(ClearingEntry *val);
    virtual void restore(const IdT &id, ExecutionsT *val);

private:
    /// Reader-writer lock for read-heavy reference data access
    /// Allows concurrent reads, exclusive writes
    mutable oneapi::tbb::spin_rw_mutex rwLock_;

    /// Cache-aligned to prevent false sharing with rwLock_
    CacheAlignedAtomic<u64> subscrCounter_;

    /// The maps an order's lookups walk, its strings, its ClOrdIDs and its executions' list, keep their nodes in pools
    /// of their own, so that they lie together rather than among the orders, whose size moved every lookup (#467).
    /// Declared before the maps, so that they outlive them. The pools aren't synchronised: every change to the maps
    /// runs under rwLock_'s write lock, and so must any copy of one, whose nodes come from the same pool. Reference
    /// data is loaded before any order, so its maps keep the global heap.
    NodePool stringNodes_;
    NodePool rawDataNodes_;
    NodePool executionListNodes_;

    typedef std::map<SourceIdT, InstrumentEntry *> InstrumentsT;
    InstrumentsT instruments_;
    typedef PooledMap<SourceIdT, StringT *> StringsT;
    StringsT strings_;
    typedef std::map<SourceIdT, AccountEntry *> AccountsT;
    AccountsT accounts_;
    typedef std::map<SourceIdT, ClearingEntry *> ClearingsT;
    ClearingsT clearings_;
    typedef PooledMap<SourceIdT, RawDataEntry *> RawDataT;
    RawDataT rawDatas_;

    typedef PooledMap<SourceIdT, ExecutionsT *> ExecutionListsT;
    ExecutionListsT executions_;

    DataSaver *storage_;

    std::map<StringT, SourceIdT> instrumentsBySymbol_;
    std::map<StringT, SourceIdT> accountsByName_;
    std::map<StringT, SourceIdT> clearingsByFirm_;

public:
    template <typename Fn> void forEachInstrument(Fn &&fn) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
        for (const auto &[id, entry] : instruments_)
        {
            fn(id, *entry);
        }
    }

    template <typename Fn> void forEachAccount(Fn &&fn) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
        for (const auto &[id, entry] : accounts_)
        {
            fn(id, *entry);
        }
    }

    template <typename Fn> void forEachClearing(Fn &&fn) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
        for (const auto &[id, entry] : clearings_)
        {
            fn(id, *entry);
        }
    }

    SourceIdT findInstrumentBySymbol(const StringT &symbol) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
        auto it = instrumentsBySymbol_.find(symbol);
        if (it == instrumentsBySymbol_.end())
        {
            return SourceIdT();
        }
        return it->second;
    }

    SourceIdT findAccountByName(const StringT &name) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
        auto it = accountsByName_.find(name);
        if (it == accountsByName_.end())
        {
            return SourceIdT();
        }
        return it->second;
    }

    SourceIdT findClearingByFirm(const StringT &firm) const
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
        auto it = clearingsByFirm_.find(firm);
        if (it == clearingsByFirm_.end())
        {
            return SourceIdT();
        }
        return it->second;
    }
};

typedef aux::Singleton<WideParamsDataStorage> WideDataStorage;

} // namespace Store
} // namespace COP