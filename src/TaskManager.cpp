/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include <iostream>

#include "Logger.h"
#include "TaskManager.h"
#include "ExchUtils.h"

using namespace std;
using namespace COP;
using namespace aux;
using namespace COP::ACID;
using namespace COP::Tasks;
using namespace COP::Queues;

std::unique_ptr<oneapi::tbb::global_control> TaskManager::scheduler_;
oneapi::tbb::task_group TaskManager::taskGroup_;

TaskManager::TaskManager(const TaskManagerParams &params):
	transactMgr_(nullptr), transactIt_(nullptr),
	lastAvailableTransactProcessor_(0), lastAvailableEvntProcessor_(0),
	totalAvailableTransactProcessor_(0), totalAvailableEvntProcessor_(0),
	created_(0), processed_(0), finished_(0),
	createdTr_(0), processedTr_(0), finishedTr_(0)
{
	assert(nullptr != params.transactMgr_);
	transactMgr_ = params.transactMgr_;
	transactIt_ = transactMgr_->iterator();

	assert(!params.transactProcessors_.empty());
	transactProcessors_ = params.transactProcessors_;
	lastAvailableTransactProcessor_.store(static_cast<int>(transactProcessors_.size()) - 1);
	totalAvailableTransactProcessor_.store(static_cast<int>(transactProcessors_.size()) - 1);

	assert(!params.evntProcessors_.empty());
	evntProcessors_ = params.evntProcessors_;
	lastAvailableEvntProcessor_.store(static_cast<int>(evntProcessors_.size()) - 1);
	totalAvailableEvntProcessor_.store(static_cast<int>(evntProcessors_.size()) - 1);

	assert(nullptr != transactIt_);
	transactMgr_->attach(this);

	assert(nullptr != params.inQueues_);
	inQueues_ = params.inQueues_;
	inQueues_->attach(this);

	aux::ExchLogger::instance()->note("TaskManager created.");
}

TaskManager::~TaskManager(void)
{
	assert(nullptr != transactMgr_);
	transactMgr_->detach();
	transactIt_ = nullptr;
	transactMgr_ = nullptr;
	if(lastAvailableTransactProcessor_.load() != totalAvailableTransactProcessor_.load()){
		cout<< "lastAvailableTransactProcessor_ = "<< lastAvailableTransactProcessor_.load()<<endl;
		cout<< "transactProcessors_ = "<< totalAvailableTransactProcessor_.load()<<endl;
	}
	assert(lastAvailableTransactProcessor_.load() == totalAvailableTransactProcessor_.load());
	for(size_t i = 0; i < transactProcessors_.size(); ++i){
		std::unique_ptr<TransactionProcessor> ap(transactProcessors_[i]);
	}
	assert(lastAvailableEvntProcessor_.load() == totalAvailableEvntProcessor_.load());
	for(size_t i = 0; i < evntProcessors_.size(); ++i){
		std::unique_ptr<InQueueProcessor> ap(evntProcessors_[i]);
	}

	char buf[64];
	buf[0] = 0;
	aux::toStr(buf, processed_.fetch_add(1));
	aux::ExchLogger::instance()->debug(string("Tasks processed: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, finished_.fetch_add(1));
	aux::ExchLogger::instance()->debug(string("Tasks finished: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, createdTr_.fetch_add(1));
	aux::ExchLogger::instance()->debug(string("TrTasks created: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, processedTr_.fetch_add(1));
	aux::ExchLogger::instance()->debug(string("TrTasks processed: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, finishedTr_.fetch_add(1));
	aux::ExchLogger::instance()->debug(string("TrTasks finished: ") + buf);

	aux::ExchLogger::instance()->note("TaskManager destroyed.");
}

void TaskManager::init(int workerAmount)
{
	if (workerAmount > 0) {
		scheduler_ = std::make_unique<oneapi::tbb::global_control>(
			oneapi::tbb::global_control::max_allowed_parallelism, workerAmount);
	}
	aux::ExchLogger::instance()->note("TaskManager initialized.");
}

void TaskManager::destroy()
{
	taskGroup_.wait();
	scheduler_.reset();
	aux::ExchLogger::instance()->note("TaskManager deinitialized.");
}

bool TaskManager::waitUntilTransactionsFinished(int waitIntervalSeconds)const
{
	int interval = waitIntervalSeconds;
	bool onceSucceed = false;
	do{
		bool finished = false;
		{
			oneapi::tbb::mutex::scoped_lock lock(transactLock_);
			oneapi::tbb::mutex::scoped_lock lock2(eventLock_);
			finished = (lastAvailableTransactProcessor_.load() == totalAvailableTransactProcessor_.load())&&
						(lastAvailableEvntProcessor_.load() == totalAvailableEvntProcessor_.load())&&
						(0 == inQueues_->size());
		}
		if(finished && !onceSucceed && (0 != waitIntervalSeconds)){
			finished = false;
			onceSucceed = true;
		}
		if((finished)||(0 == waitIntervalSeconds))
			return finished;
		WaitInterval(1000);
		if(-1 != waitIntervalSeconds)
			--interval;
	}while(0 <= interval);
	return false;
}

void TaskManager::addTask(const TransactionId &/*id*/)
{
	onReadyToExecute();
}

void TaskManager::onReadyToExecute()
{
	TransactionProcessor *proc = nullptr;
	TransactionId id;
	Transaction *tr = nullptr;
	{
		oneapi::tbb::mutex::scoped_lock lock(transactLock_);
		int lastIdx = lastAvailableTransactProcessor_.load();
		if(0 > lastIdx){
			return;
		}
		assert(nullptr != transactIt_);
		if(!transactIt_->next(&id, &tr)){
			return;
		}
		lastAvailableTransactProcessor_.fetch_sub(1);
		proc = transactProcessors_[lastIdx];
		transactProcessors_[lastIdx] = nullptr;
	}

	taskCreatedTr();

	taskGroup_.run([this, id, tr, proc]() {
		assert(nullptr != tr);
		assert(nullptr != proc);
		proc->process(id, tr);
		taskProcessedTr();

		finishTransaction(id, tr, proc);
		onReadyToExecute();
		taskFinishedTr();
	});
}

bool TaskManager::finishTransaction(const ACID::TransactionId &id, ACID::Transaction *tr, TransactionProcessor *proc)
{
	assert(nullptr != proc);
	assert(nullptr != tr);
	bool rez = false;
	{
		oneapi::tbb::mutex::scoped_lock lock(transactLock_);
		int v = lastAvailableTransactProcessor_.fetch_add(1);
		transactProcessors_[v + 1] = proc;
	}
	assert(nullptr != transactMgr_);
	rez = transactMgr_->removeTransaction(id, tr);
	return rez;
}

void TaskManager::onNewEvent()
{
	InQueueProcessor *proc = nullptr;
	{
		oneapi::tbb::mutex::scoped_lock lock(eventLock_);
		int lastIdx = lastAvailableEvntProcessor_.load();
		if(0 > lastIdx){
			return;
		}
		lastAvailableEvntProcessor_.fetch_sub(1);
		proc = evntProcessors_[lastIdx];
		assert(nullptr != proc);
		evntProcessors_[lastIdx] = nullptr;
	}

	taskCreated();

	taskGroup_.run([this, proc]() {
		assert(nullptr != proc);
		bool rez = proc->process();
		taskProcessed();

		finishEvent(proc);
		if(rez){
			onNewEvent();
			taskFinished();
		}
	});
}

void TaskManager::finishEvent(Queues::InQueueProcessor *proc)
{
	assert(nullptr != proc);
	oneapi::tbb::mutex::scoped_lock lock(eventLock_);
	int v = lastAvailableEvntProcessor_.fetch_add(1);
	evntProcessors_[v + 1] = proc;
}

void TaskManager::taskCreated()
{
	created_.fetch_add(1);
}

void TaskManager::taskProcessed()
{
	processed_.fetch_add(1);
}

void TaskManager::taskFinished()
{
	finished_.fetch_add(1);
}

void TaskManager::taskCreatedTr()
{
	createdTr_.fetch_add(1);
}

void TaskManager::taskProcessedTr()
{
	processedTr_.fetch_add(1);
}

void TaskManager::taskFinishedTr()
{
	finishedTr_.fetch_add(1);
}
