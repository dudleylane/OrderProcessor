/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TransactionMgr.h"
#include "IdTGenerator.h"
#include "Logger.h"

using namespace aux;
using namespace std;
using namespace tbb;
using namespace COP;
using namespace COP::ACID;

TransactionMgr::TransactionMgr(void): idGenerator_(NULL), started_(false), obs_(NULL)
{
	//aux::ExchLogger::instance()->note("TransactionMgr created.");
}

TransactionMgr::~TransactionMgr(void)
{
	assert(!started_);
	//aux::ExchLogger::instance()->note("TransactionMgr destroyed.");
	transactionTree_.dumpTree();
}

void TransactionMgr::attach(TransactionObserver *obs)
{
	assert(NULL != obs);
	assert(NULL == obs_);
	mutex::scoped_lock lock(lock_);
	obs_ = obs;
	//aux::ExchLogger::instance()->note("TransactionMgr TransactionObserver attached.");
}

TransactionObserver *TransactionMgr::detach()
{
	mutex::scoped_lock lock(lock_);
	TransactionObserver *obs = obs_;
	obs_ = NULL;
	//aux::ExchLogger::instance()->note("TransactionMgr TransactionObserver detached.");
	return obs;
}

void TransactionMgr::init(const TransactionMgrParams &params)
{
	assert(!started_);
	assert(NULL == idGenerator_);
	assert(NULL != params.idGenerator_);
	idGenerator_ = params.idGenerator_;
	started_ = true;
	//aux::ExchLogger::instance()->note("TransactionMgr initialized.");
}

void TransactionMgr::stop()
{
	assert(started_);

	//aux::ExchLogger::instance()->note("TransactionMgr stopping.");

	{
		mutex::scoped_lock lock(lock_);
		started_ = false;
	}
	//aux::ExchLogger::instance()->note("TransactionMgr stopped.");
}

void TransactionMgr::addTransaction(std::auto_ptr<Transaction> &tr)
{
	assert(started_);
	assert(NULL != tr.get());
	assert(!tr->transactionId().isValid());
	assert(NULL != idGenerator_);

	//aux::ExchLogger::instance()->debug("TransactionMgr adding transaction.");

	Transaction *trPtr = tr.get();

	ObjectsInTransactionT objects;
	trPtr->getRelatedObjects(&objects);
	int ready2Exec = 0;
	{
		mutex::scoped_lock lock(lock_);
		IdT id = idGenerator_->getId(); // should be just before add() call, because tree requires sequential order adding of transactions
		tr->setTransactionId(id);
		transactionTree_.add(id, trPtr, objects, &ready2Exec);
	}
	tr.release();
	if((0 < ready2Exec)&&(NULL != obs_)){
		obs_->onReadyToExecute();
	}
	//aux::ExchLogger::instance()->debug("TransactionMgr added transaction.");
}

bool TransactionMgr::removeTransaction(const TransactionId &id, Transaction *t)
{
	assert(started_);
	assert(NULL != t);
	assert(id.isValid());
	std::auto_ptr<Transaction> trans(t);
	//aux::ExchLogger::instance()->debug("TransactionMgr removing transaction.");

	int ready2Exec = 0;
	bool rez = false;
	{
		mutex::scoped_lock lock(lock_);
		rez = transactionTree_.remove(id, &ready2Exec);
	}
	if((0 < ready2Exec)&&(NULL != obs_)){
		//aux::ExchLogger::instance()->debug("TransactionMgr starts new transaction after remove previous.");
		obs_->onReadyToExecute();
	}
	//aux::ExchLogger::instance()->debug("TransactionMgr removed transaction.");
	return rez;
}

bool TransactionMgr::getParentTransactions(const TransactionId &id, TransactionIdsT *parent)const
{
	assert(started_);
	{
		mutex::scoped_lock lock(lock_);
		return transactionTree_.getParents(id, parent);
	}
}

bool TransactionMgr::getRelatedTransactions(const TransactionId &id, TransactionIdsT *related)const
{
	assert(started_);
	{
		mutex::scoped_lock lock(lock_);
		return transactionTree_.getChildren(id, related);
	}
}

TransactionIterator *TransactionMgr::iterator()
{
	return this;
}

bool TransactionMgr::next(TransactionId *id, Transaction **tr)
{
	mutex::scoped_lock lock(lock_);
	return transactionTree_.next(id, tr);
}

bool TransactionMgr::get(TransactionId *id, Transaction **tr)const
{
	mutex::scoped_lock lock(lock_);
	return transactionTree_.current(id, tr);
}

bool TransactionMgr::isValid()const
{
	mutex::scoped_lock lock(lock_);
	return transactionTree_.isCurrentValid();
}
