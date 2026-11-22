/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

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
#include "CacheAlignedAtomic.h"
#include "CpuAffinity.h"

namespace COP
{

namespace Tasks
{

/// The task group a TaskManager's tasks run in, which starts a task only the way the manager must (#364, #630): spawned
/// from inside one of the manager's own tasks, enqueued from any other thread. A spawn waits for a worker to steal it,
/// and oneTBB doesn't promise to wake one; the threads that push events never return to its dispatcher, so a task they
/// spawned could sit unrun holding the only event processor. The group itself is private here, so no code of the
/// manager's can call its run(), which spawns from any thread.
class TaskStarter final
{
public:
    /// Spawns the task from inside one of the manager's tasks, and enqueues it from any other thread
    template <typename F> void start(F &&task);
    /// Waits for every task started here, and rethrows a task's exception once they have all finished
    void wait()
    {
        group_.wait();
    }

private:
    oneapi::tbb::task_group group_;
};

struct TaskManagerParams
{
    ACID::TransactionManager *transactMgr_;
    ACID::ProcessorPoolT transactProcessors_;
    Queues::InQueueProcessorsPoolT evntProcessors_;
    Queues::InQueuesContainer *inQueues_;

    int cpuAffinityStart_ = -1;

    TaskManagerParams() : transactMgr_(nullptr), transactProcessors_(), evntProcessors_(), inQueues_(nullptr) {}
};

class TaskManager : public ExecTaskManager, public ACID::TransactionObserver, public Queues::InQueuesObserver
{
public:
    explicit TaskManager(const TaskManagerParams &params);
    /// Waits for every task this manager started before tearing down (#21). Explicitly noexcept: the
    /// task_group member's destructor is noexcept(false), which would otherwise loosen the specification
    /// of the overridden ExecTaskManager destructor.
    ~TaskManager(void) noexcept;

    static void init(int workerAmount = 0);
    static void destroy();

    /// method waits until all incoming events and transactions finished processing: every processor back in its pool,
    /// the incoming queues empty, and no transaction left in the transaction manager (#263)
    /// returns false, if events and transactions not finished during waitIntervalSeconds
    /// waitIntervalSeconds = -1, means infinite; 0 checks once and returns at once. A wait checks every 10 ms, and
    /// returns at the first check that finds the work done (#587)
    bool waitUntilTransactionsFinished(int waitIntervalSeconds) const;

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

    // Read-only accessors for monitoring (relaxed ordering — statistics only)
    /// Event tasks started
    u64 eventsCreated() const noexcept
    {
        return created_.load(std::memory_order_relaxed);
    }
    /// Events taken off the incoming queues, those that threw included: a task that found them empty took none (#298)
    u64 eventsProcessed() const noexcept
    {
        return processed_.load(std::memory_order_relaxed);
    }
    /// Event tasks finished: each task started, once its last statement has run (#298)
    u64 eventsFinished() const noexcept
    {
        return finished_.load(std::memory_order_relaxed);
    }
    u64 transactionsCreated() const noexcept
    {
        return createdTr_.load(std::memory_order_relaxed);
    }
    u64 transactionsProcessed() const noexcept
    {
        return processedTr_.load(std::memory_order_relaxed);
    }
    u64 transactionsFinished() const noexcept
    {
        return finishedTr_.load(std::memory_order_relaxed);
    }
    int availableEventProcessors() const noexcept
    {
        return lastAvailableEvntProcessor_.load(std::memory_order_relaxed);
    }
    int totalEventProcessors() const noexcept
    {
        return totalAvailableEvntProcessor_.load(std::memory_order_relaxed);
    }
    int availableTransactProcessors() const noexcept
    {
        return lastAvailableTransactProcessor_.load(std::memory_order_relaxed);
    }
    int totalTransactProcessors() const noexcept
    {
        return totalAvailableTransactProcessor_.load(std::memory_order_relaxed);
    }
    /// Tasks whose exception was logged and contained, rather than stopping this manager (#57)
    u64 tasksFailed() const noexcept
    {
        return failed_.load(std::memory_order_relaxed);
    }

private:
    void taskFailed(const char *kind, const char *what);
    /// Starts a task of this manager's: spawned from inside one of its tasks, enqueued from any other thread (#364)
    template <typename F> void submit(F &&task);
    /// Every processor back in its pool, nothing queued, and no transaction left in the manager (#263)
    bool allWorkDone() const;
    /// Pins the calling worker to the next core, once per thread, when the manager was given a first core (#536)
    void pinThisWorker() const;

    mutable oneapi::tbb::mutex lock_;
    mutable oneapi::tbb::mutex transactLock_;
    mutable oneapi::tbb::mutex eventLock_;

    static std::unique_ptr<oneapi::tbb::global_control> scheduler_;
    /// Tasks started by this manager. Per instance, so ~TaskManager can wait for exactly its own tasks (#21). Started
    /// only by submit(), through the TaskStarter's start(): the group's own run() is out of reach (#630).
    TaskStarter taskGroup_;

    ACID::TransactionManager *transactMgr_;
    ACID::TransactionIterator *transactIt_;

    int cpuAffinityStart_;

    Queues::InQueuesContainer *inQueues_;

    ACID::ProcessorPoolT transactProcessors_;
    // Cache-aligned to prevent false sharing between threads
    CacheAlignedAtomic<int> lastAvailableTransactProcessor_;
    CacheAlignedAtomic<int> totalAvailableTransactProcessor_;

    Queues::InQueueProcessorsPoolT evntProcessors_;
    // Cache-aligned to prevent false sharing between threads
    CacheAlignedAtomic<int> lastAvailableEvntProcessor_;
    CacheAlignedAtomic<int> totalAvailableEvntProcessor_;

    // Statistics counters - cache aligned for independent access. 64-bit (#537): as int they wrapped after 2^31 tasks,
    // about 50 days at 500 events a second, and went negative in metrics_update.
    CacheAlignedAtomic<u64> created_;
    CacheAlignedAtomic<u64> processed_;
    CacheAlignedAtomic<u64> finished_;

    CacheAlignedAtomic<u64> createdTr_;
    CacheAlignedAtomic<u64> processedTr_;
    CacheAlignedAtomic<u64> finishedTr_;

    CacheAlignedAtomic<u64> failed_;
};

} // namespace Tasks
} // namespace COP