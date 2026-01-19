/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <memory>
#include <deque>
#include <oneapi/tbb/mutex.h>

#include "NLinkedTree.h"
#include "TransactionDef.h"

namespace COP{

	class IdTValueGenerator;

	namespace ACID{

		struct TransactionMgrParams{
			IdTValueGenerator *idGenerator_;

			explicit TransactionMgrParams(IdTValueGenerator *gen): idGenerator_(gen){}
		};

class TransactionMgr: public TransactionManager, public TransactionIterator
{
public:
	TransactionMgr(void);
	~TransactionMgr(void);

	void init(const TransactionMgrParams &params);
	void stop();

public:
	/// reimplemented from TransactionManager
	void attach(TransactionObserver *obs);
	TransactionObserver *detach();
	virtual void addTransaction(std::unique_ptr<Transaction> &tr);
	virtual bool removeTransaction(const TransactionId &id, Transaction *t);
	virtual bool getParentTransactions(const TransactionId &id, TransactionIdsT *parent)const;
	virtual bool getRelatedTransactions(const TransactionId &id, TransactionIdsT *related)const;
	virtual TransactionIterator *iterator();

public:
	/// reimplemented from TransactionIterator
	virtual bool next(TransactionId *id, Transaction **tr);
	virtual bool get(TransactionId *id, Transaction **tr)const;
	virtual bool isValid()const;

private:
	mutable oneapi::tbb::mutex lock_;

	IdTValueGenerator *idGenerator_;
	bool started_;

	aux::NLinkTree transactionTree_;

	TransactionObserver *obs_;
};

	}
}