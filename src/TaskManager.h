/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <vector>
#include <atomic>
#include <memory>
#include <oneapi/tbb/mutex.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/task_group.h>

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
	mutable oneapi::tbb::mutex lock_;
	mutable oneapi::tbb::mutex transactLock_;
	mutable oneapi::tbb::mutex eventLock_;

	static std::unique_ptr<oneapi::tbb::global_control> scheduler_;
	static oneapi::tbb::task_group taskGroup_;

	ACID::TransactionManager *transactMgr_;
	ACID::TransactionIterator *transactIt_;

	Queues::InQueuesContainer *inQueues_;

	ACID::ProcessorPoolT transactProcessors_;
	std::atomic<int> lastAvailableTransactProcessor_;
	std::atomic<int> totalAvailableTransactProcessor_;

	Queues::InQueueProcessorsPoolT evntProcessors_;
	std::atomic<int> lastAvailableEvntProcessor_;
	std::atomic<int> totalAvailableEvntProcessor_;

	std::atomic<int> created_;
	std::atomic<int> processed_;
	std::atomic<int> finished_;

	std::atomic<int> createdTr_;
	std::atomic<int> processedTr_;
	std::atomic<int> finishedTr_;

};

	}
}