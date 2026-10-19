/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include <iostream>

#include <oneapi/tbb/task_arena.h>

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

/// Global atomic counter for distributing TBB workers across CPU cores.
/// Each TBB worker thread pins itself to the next core on first task pickup.
static std::atomic<int> g_nextWorkerCore{ 0 };

/// Whether this thread is running one of a TaskManager's tasks, which decides how submit() starts the next (#364)
static thread_local bool g_inTask = false;

namespace
{
/// Marks this thread as in a task for the task body's extent
class InTask final
{
public:
    InTask() noexcept : was_(g_inTask)
    {
        g_inTask = true;
    }
    ~InTask()
    {
        g_inTask = was_;
    }
    InTask(const InTask &) = delete;
    InTask &operator=(const InTask &) = delete;

private:
    bool was_;
};
} // namespace

TaskManager::TaskManager(const TaskManagerParams &params)
    : transactMgr_(nullptr), transactIt_(nullptr), cpuAffinityStart_(-1), lastAvailableTransactProcessor_(0),
      totalAvailableTransactProcessor_(0), lastAvailableEvntProcessor_(0), totalAvailableEvntProcessor_(0), created_(0),
      processed_(0), finished_(0), createdTr_(0), processedTr_(0), finishedTr_(0), failed_(0)
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

    cpuAffinityStart_ = params.cpuAffinityStart_;
    if (cpuAffinityStart_ >= 0)
    {
        // Reserve cores starting after the main IO thread's core
        g_nextWorkerCore.store(cpuAffinityStart_ + 1, std::memory_order_relaxed);
    }

    aux::ExchLogger::instance()->note("TaskManager created.");
}

TaskManager::~TaskManager(void) noexcept
{
    // Detach before waiting, so that no producer can start another task once the wait is over. This does
    // not cut the drain short: a task's tail calls onReadyToExecute() and onNewEvent() on this manager
    // directly, and both poll (the transaction iterator, the event queues) rather than waiting to be
    // notified through these observers (#21).
    assert(nullptr != transactMgr_);
    transactMgr_->detach();
    assert(nullptr != inQueues_);
    inQueues_->detach();

    // Wait for every task this manager started, including tasks their tails spawned. A task returns its
    // processor to the pool before its last statements (onReadyToExecute() or onNewEvent(), then a counter
    // increment), so waitUntilTransactionsFinished() can report done while a task still uses `this`. The
    // wait must precede the clearing below: the tails also read transactIt_ and transactMgr_ (#21).
    // wait() rethrows a task's exception once every task has finished; a destructor cannot propagate it.
    try
    {
        taskGroup_.wait();
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("TaskManager: a task failed before shutdown: ") + ex.what());
    }
    catch (...)
    {
        aux::ExchLogger::instance()->error(std::string("TaskManager: a task failed before shutdown"));
    }

    transactIt_ = nullptr;
    transactMgr_ = nullptr;
    if (lastAvailableTransactProcessor_.load() != totalAvailableTransactProcessor_.load())
    {
        cout << "lastAvailableTransactProcessor_ = " << lastAvailableTransactProcessor_.load() << endl;
        cout << "transactProcessors_ = " << totalAvailableTransactProcessor_.load() << endl;
    }
    assert(lastAvailableTransactProcessor_.load() == totalAvailableTransactProcessor_.load());
    for (size_t i = 0; i < transactProcessors_.size(); ++i)
    {
        std::unique_ptr<TransactionProcessor> ap(transactProcessors_[i]);
    }
    assert(lastAvailableEvntProcessor_.load() == totalAvailableEvntProcessor_.load());
    for (size_t i = 0; i < evntProcessors_.size(); ++i)
    {
        std::unique_ptr<InQueueProcessor> ap(evntProcessors_[i]);
    }

    char buf[64];
    buf[0] = 0;
    aux::toStr(buf, processed_.load(std::memory_order_relaxed));
    aux::ExchLogger::instance()->debug(string("Events processed: ") + buf);
    buf[0] = 0;
    aux::toStr(buf, finished_.load(std::memory_order_relaxed));
    aux::ExchLogger::instance()->debug(string("Tasks finished: ") + buf);
    buf[0] = 0;
    aux::toStr(buf, createdTr_.load(std::memory_order_relaxed));
    aux::ExchLogger::instance()->debug(string("TrTasks created: ") + buf);
    buf[0] = 0;
    aux::toStr(buf, processedTr_.load(std::memory_order_relaxed));
    aux::ExchLogger::instance()->debug(string("TrTasks processed: ") + buf);
    buf[0] = 0;
    aux::toStr(buf, finishedTr_.load(std::memory_order_relaxed));
    aux::ExchLogger::instance()->debug(string("TrTasks finished: ") + buf);

    aux::ExchLogger::instance()->note("TaskManager destroyed.");
}

void TaskManager::init(int workerAmount)
{
    if (workerAmount > 0)
    {
        scheduler_ = std::make_unique<oneapi::tbb::global_control>(oneapi::tbb::global_control::max_allowed_parallelism,
                                                                   workerAmount);
    }
    aux::ExchLogger::instance()->note("TaskManager initialized.");
}

void TaskManager::destroy()
{
    // Each TaskManager waits for its own tasks in its destructor; this only releases the scheduler limit.
    scheduler_.reset();
    aux::ExchLogger::instance()->note("TaskManager deinitialized.");
}

bool TaskManager::waitUntilTransactionsFinished(int waitIntervalSeconds) const
{
    int interval = waitIntervalSeconds;
    bool onceSucceed = false;
    do
    {
        bool finished = false;
        {
            // An idle pool is not enough. A task hands its processor back before it removes its transaction, whose
            // removal releases the transactions ordered after it, so for that moment the pool is idle with work left
            // (#263); the transaction is still in the manager until then. The manager's lock is taken under these two,
            // as onReadyToExecute() takes it under transactLock_, and the manager calls no observer while holding it.
            oneapi::tbb::mutex::scoped_lock lock(transactLock_);
            oneapi::tbb::mutex::scoped_lock lock2(eventLock_);
            finished = (lastAvailableTransactProcessor_.load() == totalAvailableTransactProcessor_.load()) &&
                       (lastAvailableEvntProcessor_.load() == totalAvailableEvntProcessor_.load()) &&
                       (0 == inQueues_->size()) && (0 == transactMgr_->pendingTransactions());
        }
        if (finished && !onceSucceed && (0 != waitIntervalSeconds))
        {
            finished = false;
            onceSucceed = true;
        }
        if ((finished) || (0 == waitIntervalSeconds))
        {
            return finished;
        }
        WaitInterval(1000);
        if (-1 != waitIntervalSeconds)
        {
            --interval;
        }
    } while (0 <= interval);
    return false;
}

// A spawn (task_group::run) waits for a worker to steal it, and oneTBB doesn't promise to wake one: its arena.h says the
// spawn's double check is "deliberately sloppy about memory fences" (2021.11, advertise_new_work()). A worker that
// spawns runs its own spawn when it returns to the dispatcher, so a task started from inside one of these tasks, as
// almost every task is (a task's tail starts the next), is spawned, the cheaper start. The threads that push events and
// transactions here don't return to the dispatcher: the IO thread, the FIX gateway, the expiry scheduler, restore, a
// test. Their spawn could miss its wakeup, and then the task held an event processor and never ran, so with one
// processor nothing ran after it (#364). So a task they start is enqueued: an enqueued task takes the fenced path
// oneTBB promises to run, with a worker even where the arena has none. Enqueuing every task instead cost the pool
// benchmarks about 10% (#364). defer() keeps the task in taskGroup_, so the destructor's wait() still covers it.
template <typename F> void TaskManager::submit(F &&task)
{
    if (g_inTask)
    {
        taskGroup_.run(std::forward<F>(task));
    }
    else
    {
        oneapi::tbb::this_task_arena::enqueue(taskGroup_.defer(std::forward<F>(task)));
    }
}

void TaskManager::addTask(const TransactionId & /*id*/)
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
        if (0 > lastIdx)
        {
            return;
        }
        assert(nullptr != transactIt_);
        if (!transactIt_->next(&id, &tr))
        {
            return;
        }
        lastAvailableTransactProcessor_.fetch_sub(1);
        proc = transactProcessors_[lastIdx];
        transactProcessors_[lastIdx] = nullptr;
    }

    taskCreatedTr();

    submit(
        [this, id, tr, proc]()
        {
            InTask inTask;
            // Pin this TBB worker thread to a dedicated core (once per thread)
            if (cpuAffinityStart_ >= 0)
            {
                static thread_local bool pinned = false;
                if (!pinned)
                {
                    int core =
                        g_nextWorkerCore.fetch_add(1, std::memory_order_relaxed) % CpuAffinity::getAvailableCores();
                    CpuAffinity::pinThreadToCore(core);
                    pinned = true;
                }
            }

            assert(nullptr != tr);
            assert(nullptr != proc);
            // As for events below (#57): a throw would stop all processing, and would skip finishTransaction(), so
            // the transactions ordered after this one would never run.
            try
            {
                proc->process(id, tr);
            }
            catch (const std::exception &ex)
            {
                taskFailed("transaction", ex.what());
            }
            catch (...)
            {
                taskFailed("transaction", "unknown exception");
            }
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
        if (0 > lastIdx)
        {
            return;
        }
        lastAvailableEvntProcessor_.fetch_sub(1);
        proc = evntProcessors_[lastIdx];
        assert(nullptr != proc);
        evntProcessors_[lastIdx] = nullptr;
    }

    taskCreated();

    submit(
        [this, proc]()
        {
            InTask inTask;
            // Pin this TBB worker thread to a dedicated core (once per thread)
            if (cpuAffinityStart_ >= 0)
            {
                static thread_local bool pinned = false;
                if (!pinned)
                {
                    int core =
                        g_nextWorkerCore.fetch_add(1, std::memory_order_relaxed) % CpuAffinity::getAvailableCores();
                    CpuAffinity::pinThreadToCore(core);
                    pinned = true;
                }
            }

            assert(nullptr != proc);
            // A throw out of this task would cancel taskGroup_, so that no later task ran, and would keep this
            // processor out of the pool: one bad event stopped all processing until restart (#57). The event that
            // threw has been consumed, so log it and carry on with the next.
            bool rez = true;
            try
            {
                rez = proc->process();
            }
            catch (const std::exception &ex)
            {
                taskFailed("event", ex.what());
            }
            catch (...)
            {
                taskFailed("event", "unknown exception");
            }
            // An event taken counts, one that threw included; a pop that found the queue empty took none. The
            // dashboard shows this count per second as events per second (#298).
            if (rez)
            {
                taskProcessed();
            }

            finishEvent(proc);
            // Look again after an empty pop too: a push meanwhile found no processor free and started nothing (#298).
            // The push made the queue non-empty before taking eventLock_, which finishEvent() has taken since, so this
            // sees its event. An empty queue still ends the chain.
            if (rez || (0 != inQueues_->size()))
            {
                onNewEvent();
            }
            taskFinished();
        });
}

void TaskManager::finishEvent(Queues::InQueueProcessor *proc)
{
    assert(nullptr != proc);
    oneapi::tbb::mutex::scoped_lock lock(eventLock_);
    int v = lastAvailableEvntProcessor_.fetch_add(1);
    evntProcessors_[v + 1] = proc;
}

void TaskManager::taskFailed(const char *kind, const char *what)
{
    const int failed = failed_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Runs inside a task's catch handler, where a throw would undo the containment (#57). Losing one log line, e.g.
    // to bad_alloc, is the lesser harm; the count above still records the failure.
    try
    {
        aux::ExchLogger::instance()->error("TaskManager: contained exception #" + std::to_string(failed) + " in " +
                                           kind + " task: " + what);
    }
    catch (...)
    {
    }
}

void TaskManager::taskCreated()
{
    // Relaxed ordering - statistics counters don't need memory synchronization
    created_.fetch_add(1, std::memory_order_relaxed);
}

void TaskManager::taskProcessed()
{
    // Relaxed ordering - statistics counters don't need memory synchronization
    processed_.fetch_add(1, std::memory_order_relaxed);
}

void TaskManager::taskFinished()
{
    // Relaxed ordering - statistics counters don't need memory synchronization
    finished_.fetch_add(1, std::memory_order_relaxed);
}

void TaskManager::taskCreatedTr()
{
    // Relaxed ordering - statistics counters don't need memory synchronization
    createdTr_.fetch_add(1, std::memory_order_relaxed);
}

void TaskManager::taskProcessedTr()
{
    // Relaxed ordering - statistics counters don't need memory synchronization
    processedTr_.fetch_add(1, std::memory_order_relaxed);
}

void TaskManager::taskFinishedTr()
{
    // Relaxed ordering - statistics counters don't need memory synchronization
    finishedTr_.fetch_add(1, std::memory_order_relaxed);
}
