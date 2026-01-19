/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <vector>
#include <tbb/mutex.h>
#include <tbb/atomic.h>
#include <tbb/task_scheduler_init.h>

#include "TasksDef.h"
#include "QueuesDef.h"
#include "TransactionDef.h"

namespace COP{

	namespace Tasks{

		struct TaskManagerParams{
			ACID::TransactionManager *transactMgr_;
			ACID::ProcessorPoolT transactProcessors_;
			Queues::InQueueProcessorsPoolT evntProcessors_;
			Queues::InQueuesContainer *inQueues_;

			TaskManagerParams(): transactMgr_(nullptr), transactProcessors_(), evntProcessors_(), inQueues_(nullptr){}
		};

class TaskManager: public ExecTaskManager, public ACID::TransactionObserver, public Queues::InQueuesObserver
{
public:
	explicit TaskManager(const TaskManagerParams &params);
	~TaskManager(void);

	static void init(int workerAmount = 0);
	static void destroy();

	/// method waits until all incoming events and transactions finished processing 
	/// returns false, if events and transactions not finished during waitIntervalSeconds
	/// waitIntervalSeconds = -1, means infinite
	bool waitUntilTransactionsFinished(int waitIntervalSeconds)const;

	bool finishTransaction(const ACID::TransactionId &id, ACID::Transaction *tr, ACID::TransactionProcessor *proc);
	void finishEvent(Queues::InQueueProcessor *proc);
public:
	/// reimplemented from ExecTaskManager
	virtual void addTask(const ACID::TransactionId &id);
public:
	/// reimplemented from TransactionObserver
	virtual void onReadyToExecute();

public:
	/// reimplemented from InQueuesObserver
	virtual void onNewEvent();

public:
	void taskCreated();
	void taskProcessed();
	void taskFinished();
	void taskCreatedTr();
	void taskProcessedTr();
	void taskFinishedTr();

private:
	mutable tbb::mutex lock_;
	mutable tbb::mutex transactLock_;
	mutable tbb::mutex eventLock_;

	static tbb::task_scheduler_init scheduler_;

	ACID::TransactionManager *transactMgr_;
	ACID::TransactionIterator *transactIt_;

	Queues::InQueuesContainer *inQueues_;

	ACID::ProcessorPoolT transactProcessors_;
	tbb::atomic<int> lastAvailableTransactProcessor_;
	tbb::atomic<int> totalAvailableTransactProcessor_;

	Queues::InQueueProcessorsPoolT evntProcessors_;
	tbb::atomic<int> lastAvailableEvntProcessor_;
	tbb::atomic<int> totalAvailableEvntProcessor_;

	tbb::atomic<int> created_;
	tbb::atomic<int> processed_;
	tbb::atomic<int> finished_;

	tbb::atomic<int> createdTr_;
	tbb::atomic<int> processedTr_;
	tbb::atomic<int> finishedTr_;

};

	}
}