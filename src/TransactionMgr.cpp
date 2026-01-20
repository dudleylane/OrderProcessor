/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TransactionMgr.h"
#include "IdTGenerator.h"

using namespace std;
using namespace tbb;
using namespace COP;
using namespace COP::ACID;

TransactionMgr::TransactionMgr(void): idGenerator_(nullptr), started_(false), obs_(nullptr)
{
}

TransactionMgr::~TransactionMgr(void)
{
	assert(!started_);
	transactionTree_.dumpTree();
}

void TransactionMgr::attach(TransactionObserver *obs)
{
	assert(nullptr != obs);
	assert(nullptr == obs_);
	tbb::mutex::scoped_lock lock(lock_);
	obs_ = obs;
}

TransactionObserver *TransactionMgr::detach()
{
	tbb::mutex::scoped_lock lock(lock_);
	TransactionObserver *obs = obs_;
	obs_ = nullptr;
	return obs;
}

void TransactionMgr::init(const TransactionMgrParams &params)
{
	assert(!started_);
	assert(nullptr == idGenerator_);
	assert(nullptr != params.idGenerator_);
	idGenerator_ = params.idGenerator_;
	started_ = true;
}

void TransactionMgr::stop()
{
	assert(started_);

	{
		tbb::mutex::scoped_lock lock(lock_);
		started_ = false;
	}
}

void TransactionMgr::addTransaction(std::unique_ptr<Transaction> &tr)
{
	assert(started_);
	assert(nullptr != tr.get());
	assert(!tr->transactionId().isValid());
	assert(nullptr != idGenerator_);

	Transaction *trPtr = tr.get();

	ObjectsInTransactionT objects;
	trPtr->getRelatedObjects(&objects);
	int ready2Exec = 0;
	{
		tbb::mutex::scoped_lock lock(lock_);
		IdT id = idGenerator_->getId(); // should be just before add() call, because tree requires sequential order adding of transactions
		tr->setTransactionId(id);
		transactionTree_.add(id, trPtr, objects, &ready2Exec);
	}
	tr.release();
	if((0 < ready2Exec)&&(nullptr != obs_)){
		obs_->onReadyToExecute();
	}
}

bool TransactionMgr::removeTransaction(const TransactionId &id, Transaction *t)
{
	assert(started_);
	assert(nullptr != t);
	assert(id.isValid());
	std::unique_ptr<Transaction> trans(t);

	int ready2Exec = 0;
	bool rez = false;
	{
		tbb::mutex::scoped_lock lock(lock_);
		rez = transactionTree_.remove(id, &ready2Exec);
	}
	if((0 < ready2Exec)&&(nullptr != obs_)){
		obs_->onReadyToExecute();
	}
	return rez;
}

bool TransactionMgr::getParentTransactions(const TransactionId &id, TransactionIdsT *parent)const
{
	assert(started_);
	{
		tbb::mutex::scoped_lock lock(lock_);
		return transactionTree_.getParents(id, parent);
	}
}

bool TransactionMgr::getRelatedTransactions(const TransactionId &id, TransactionIdsT *related)const
{
	assert(started_);
	{
		tbb::mutex::scoped_lock lock(lock_);
		return transactionTree_.getChildren(id, related);
	}
}

TransactionIterator *TransactionMgr::iterator()
{
	return this;
}

bool TransactionMgr::next(TransactionId *id, Transaction **tr)
{
	tbb::mutex::scoped_lock lock(lock_);
	return transactionTree_.next(id, tr);
}

bool TransactionMgr::get(TransactionId *id, Transaction **tr)const
{
	tbb::mutex::scoped_lock lock(lock_);
	return transactionTree_.current(id, tr);
}

bool TransactionMgr::isValid()const
{
	tbb::mutex::scoped_lock lock(lock_);
	return transactionTree_.isCurrentValid();
}
