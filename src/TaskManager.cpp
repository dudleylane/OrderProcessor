/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include "tbb/task.h"

#include "Logger.h"
#include "TaskManager.h"
#include "Logger.h"
#include "ExchUtils.h"

using namespace std;
using namespace tbb;
using namespace COP;
using namespace aux;
using namespace COP::ACID;
using namespace COP::Tasks;
using namespace COP::Queues;

tbb::task_scheduler_init TaskManager::scheduler_(task_scheduler_init::deferred);//deferred


namespace{
	class TransactionTask: public tbb::task
	{
	public: 
		TransactionTask(const TransactionId &id, Transaction *tr, 
						ACID::TransactionProcessor *proc, TaskManager *mgr): 
			id_(id), tr_(tr), proc_(proc), mgr_(mgr)
		{
			assert(nullptr != tr_);
			assert(nullptr != proc_);
			assert(nullptr != mgr_);
			mgr->taskCreatedTr();
		}
		~TransactionTask(){
			if((nullptr != mgr_)&&(nullptr != proc_)){
				mgr_->finishTransaction(id_, tr_, proc_);
				mgr_->taskFinishedTr();
			}
			proc_ = nullptr;
			mgr_ = nullptr;
		}

	public:
		/// reimplemented from task
		virtual task* execute(){
			assert(nullptr != tr_);
			assert(nullptr != proc_);
			proc_->process(id_, tr_);
			mgr_->taskProcessedTr();

			/// return processor back into the pool
			mgr_->finishTransaction(id_, tr_, proc_);
			proc_ = nullptr;
			tr_ = nullptr;
			mgr_->onReadyToExecute();
			mgr_->taskFinishedTr();
			mgr_ = nullptr;
			return nullptr;
		}

	private:
		TransactionId id_;
		Transaction *tr_;
		ACID::TransactionProcessor *proc_;
		TaskManager *mgr_;
	};

	class EventTask: public tbb::task
	{
	public: 
		EventTask(InQueueProcessor *proc, TaskManager *mgr): 
			proc_(proc), mgr_(mgr)
		{
			assert(nullptr != proc_);
			assert(nullptr != mgr_);
			mgr->taskCreated();
		}
		~EventTask(){
			if((nullptr != mgr_)&&(nullptr != proc_)){
				mgr_->finishEvent(proc_);
				mgr_->taskFinished();
			}
			proc_ = nullptr;
			mgr_ = nullptr;
		}

	public:
		/// reimplemented from task
		virtual task* execute(){
			assert(nullptr != proc_);
			bool rez = proc_->process();
			mgr_->taskProcessed();

			mgr_->finishEvent(proc_);
			proc_ = nullptr;
			if(rez){
				mgr_->onNewEvent();
				mgr_->taskFinished();
			}
			mgr_ = nullptr;
			return nullptr;
		}

	private:
		Queues::InQueueProcessor *proc_;
		TaskManager *mgr_;
	};
}

TaskManager::TaskManager(const TaskManagerParams &params):
	transactMgr_(nullptr), transactIt_(nullptr), lastAvailableTransactProcessor_(), lastAvailableEvntProcessor_()
{
	assert(nullptr != params.transactMgr_);
	transactMgr_ = params.transactMgr_;
	transactIt_ = transactMgr_->iterator();

	assert(!params.transactProcessors_.empty());
	transactProcessors_ = params.transactProcessors_;
	lastAvailableTransactProcessor_.fetch_and_store(static_cast<int>(transactProcessors_.size()) - 1);
	totalAvailableTransactProcessor_.fetch_and_store(static_cast<int>(transactProcessors_.size()) - 1);

	assert(!params.evntProcessors_.empty());
	evntProcessors_ = params.evntProcessors_;
	lastAvailableEvntProcessor_.fetch_and_store(static_cast<int>(evntProcessors_.size()) - 1);
	totalAvailableEvntProcessor_.fetch_and_store(static_cast<int>(evntProcessors_.size()) - 1);

	assert(nullptr != transactIt_);
	transactMgr_->attach(this);

	assert(nullptr != params.inQueues_);
	inQueues_ = params.inQueues_;
	inQueues_->attach(this);

	created_.fetch_and_store(0);
	processed_.fetch_and_store(0);
	finished_.fetch_and_store(0);
	createdTr_.fetch_and_store(0);
	processedTr_.fetch_and_store(0);
	finishedTr_.fetch_and_store(0);

	aux::ExchLogger::instance()->note("TaskManager created.");
}

#include <iostream>
using namespace std;

TaskManager::~TaskManager(void)
{
	assert(nullptr != transactMgr_);
	transactMgr_->detach();
	transactIt_ = nullptr;
	transactMgr_ = nullptr;
	if(lastAvailableTransactProcessor_ != totalAvailableTransactProcessor_){
		cout<< "lastAvailableTransactProcessor_ = "<< lastAvailableTransactProcessor_<<endl;
		cout<< "transactProcessors_ = "<< totalAvailableTransactProcessor_<<endl;
	}
	assert(lastAvailableTransactProcessor_ == totalAvailableTransactProcessor_);
	for(size_t i = 0; i < transactProcessors_.size(); ++i){
		std::unique_ptr<TransactionProcessor> ap(transactProcessors_[i]);
	}
	assert(lastAvailableEvntProcessor_ == totalAvailableEvntProcessor_);
	for(size_t i = 0; i < evntProcessors_.size(); ++i){
		std::unique_ptr<InQueueProcessor> ap(evntProcessors_[i]);
	}

	//EXCH_LOG_("Tasks created: " + itoa(created_.fetch_and_increment()));
	/*cout<< "Tasks processed: "<< processed_.fetch_and_increment()<< endl;
	cout<< "Tasks finished: "<< finished_.fetch_and_increment()<< endl;
	cout<< "TrTasks created: "<< createdTr_.fetch_and_increment()<< endl;
	cout<< "TrTasks processed: "<< processedTr_.fetch_and_increment()<< endl;
	cout<< "TrTasks finished: "<< finishedTr_.fetch_and_increment()<< endl;
*/
	char buf[64];
	buf[0] = 0;
	aux::toStr(buf, processed_.fetch_and_increment());
	aux::ExchLogger::instance()->debug(string("Tasks processed: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, finished_.fetch_and_increment());
	aux::ExchLogger::instance()->debug(string("Tasks finished: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, createdTr_.fetch_and_increment());
	aux::ExchLogger::instance()->debug(string("TrTasks created: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, processedTr_.fetch_and_increment());
	aux::ExchLogger::instance()->debug(string("TrTasks processed: ") + buf);
	buf[0] = 0;
	aux::toStr(buf, finishedTr_.fetch_and_increment());
	aux::ExchLogger::instance()->debug(string("TrTasks finished: ") + buf);

	aux::ExchLogger::instance()->note("TaskManager destroyed.");
}

void TaskManager::init(int workerAmount)
{
	scheduler_.initialize(workerAmount);
	aux::ExchLogger::instance()->note("TaskManager initialized.");
}

void TaskManager::destroy()
{
	scheduler_.terminate();
	aux::ExchLogger::instance()->note("TaskManager deinitialized.");
}

bool TaskManager::waitUntilTransactionsFinished(int waitIntervalSeconds)const
{
	int interval = waitIntervalSeconds;
	bool onceSucceed = false;
	do{
		bool finished = false;
		{
			tbb::mutex::scoped_lock lock(transactLock_);
			tbb::mutex::scoped_lock lock2(eventLock_);
			finished = (lastAvailableTransactProcessor_ == totalAvailableTransactProcessor_)&&
						(lastAvailableEvntProcessor_ == totalAvailableEvntProcessor_)&&
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
	assert(false);
/*	tbb::task *task = new(task::allocate_root()) TransactionTask(id);
	assert(nullptr != task);
    task->spawn(*task);*/
}

void TaskManager::onReadyToExecute()
{
	TransactionProcessor *proc = nullptr;
	TransactionId id;
	Transaction *tr = nullptr;
	{
		tbb::mutex::scoped_lock lock(transactLock_);
		int lastIdx = lastAvailableTransactProcessor_;
		if(0 > lastIdx){
			//aux::ExchLogger::instance()->debug("TaskManager::onReadyToExecute() new TransactionTask not spawned - no workers.");
			return;
		}
		assert(nullptr != transactIt_);
		if(!transactIt_->next(&id, &tr)){
			//aux::ExchLogger::instance()->debug("TaskManager::onReadyToExecute() new TransactionTask not spawned - no transactions.");
			return;
		}
		lastAvailableTransactProcessor_.fetch_and_decrement();
		proc = transactProcessors_[lastIdx];
		transactProcessors_[lastIdx] = nullptr;
	}
	tbb::task *task = new(task::allocate_root()) TransactionTask(id, tr, proc, this);
	assert(nullptr != task);
    task->spawn(*task);
	//aux::ExchLogger::instance()->debug("TaskManager new TransactionTask spawned.");
}

bool TaskManager::finishTransaction(const ACID::TransactionId &id, ACID::Transaction *tr, TransactionProcessor *proc)
{
	assert(nullptr != proc);
	assert(nullptr != tr);
	bool rez = false;
	{
		tbb::mutex::scoped_lock lock(transactLock_);
		int v = lastAvailableTransactProcessor_.fetch_and_increment();
		transactProcessors_[v + 1] = proc;
	}
	assert(nullptr != transactMgr_);
	rez = transactMgr_->removeTransaction(id, tr);
	//aux::ExchLogger::instance()->debug("TaskManager new transaction finished.");
	return rez;
}

void TaskManager::onNewEvent()
{
	InQueueProcessor *proc = nullptr;
	{
		tbb::mutex::scoped_lock lock(eventLock_);
		int lastIdx = lastAvailableEvntProcessor_;
		if(0 > lastIdx){
			//aux::ExchLogger::instance()->debug("TaskManager::onNewEvent() new EventTask not spawned - no workers.");
			return;
		}
		lastAvailableEvntProcessor_.fetch_and_decrement();
		proc = evntProcessors_[lastIdx];
		assert(nullptr != proc);
		evntProcessors_[lastIdx] = nullptr;
	}
	tbb::task *task = new(task::allocate_root()) EventTask(proc, this);
	assert(nullptr != task);
    task->spawn(*task);
	//aux::ExchLogger::instance()->debug("TaskManager new EventTask spawned.");
}

void TaskManager::finishEvent(Queues::InQueueProcessor *proc)
{
	assert(nullptr != proc);
	tbb::mutex::scoped_lock lock(eventLock_);
	int v = lastAvailableEvntProcessor_.fetch_and_increment();
	evntProcessors_[v + 1] = proc;
}

void TaskManager::taskCreated()
{
	created_.fetch_and_increment();
}

void TaskManager::taskProcessed()
{
	processed_.fetch_and_increment();	
}

void TaskManager::taskFinished()
{
	finished_.fetch_and_increment();
}

void TaskManager::taskCreatedTr()
{
	createdTr_.fetch_and_increment();
}

void TaskManager::taskProcessedTr()
{
	processedTr_.fetch_and_increment();	
}

void TaskManager::taskFinishedTr()
{
	finishedTr_.fetch_and_increment();
}
