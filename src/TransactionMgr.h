/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <memory>
#include <deque>
#include <oneapi/tbb/mutex.h>

#include "NLinkedTree.h"
#include "TransactionDef.h"

namespace COP
{

class IdTValueGenerator;

namespace ACID
{

struct TransactionMgrParams
{
    IdTValueGenerator *idGenerator_;

    explicit TransactionMgrParams(IdTValueGenerator *gen) : idGenerator_(gen) {}
};

class TransactionMgr : public TransactionManager, public TransactionIterator
{
public:
    TransactionMgr(void);
    ~TransactionMgr(void);

    void init(const TransactionMgrParams &params);
    void stop();

public:
    /// reimplemented from TransactionManager
    void attach(TransactionObserver *obs) override;
    TransactionObserver *detach() override;
    void addTransaction(std::unique_ptr<Transaction> &tr) override;
    bool removeTransaction(const TransactionId &id, Transaction *t) override;
    bool getParentTransactions(const TransactionId &id, TransactionIdsT *parent) const override;
    bool getRelatedTransactions(const TransactionId &id, TransactionIdsT *related) const override;
    TransactionIterator *iterator() override;
    /// The transactions added and not yet removed, waiting or running. A transaction's deferred events add theirs
    /// before it is removed, so this is 0 only once every transaction and each it led to is done (#223).
    size_t pendingTransactions() const override;

public:
    /// reimplemented from TransactionIterator
    bool next(TransactionId *id, Transaction **tr) override;
    bool get(TransactionId *id, Transaction **tr) const override;
    bool isValid() const override;

private:
    mutable oneapi::tbb::mutex lock_;

    IdTValueGenerator *idGenerator_;
    bool started_;

    aux::NLinkTree transactionTree_;

    TransactionObserver *obs_;
};

} // namespace ACID
} // namespace COP