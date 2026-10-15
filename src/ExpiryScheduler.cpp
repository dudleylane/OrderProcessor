/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "ExpiryScheduler.h"

#include <algorithm>
#include <cassert>
#include <chrono>

#include "DataModelDef.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;

namespace
{
/// The business day's zone, looked up once; it throws if the time zone database lacks it
const std::chrono::time_zone *dayZone()
{
    static const std::chrono::time_zone *const zone = std::chrono::locate_zone(ExpiryScheduler::DAY_ZONE);
    return zone;
}
/// The source the processor sees the timers' events from
const char *const TIMER_SOURCE = "ExpiryScheduler";
} // namespace

ExpiryScheduler::ExpiryScheduler(InQueues *inQueues, Clock clock) : inQueues_(inQueues), clock_(std::move(clock))
{
    assert(nullptr != inQueues_);
    assert(clock_);
    dayZone(); // a server that can't place the day's end doesn't start
}

ExpiryScheduler::~ExpiryScheduler()
{
    stop();
}

DateTimeT ExpiryScheduler::dayEndAfter(DateTimeT when)
{
    using namespace std::chrono;
    const time_zone *const zone = dayZone();
    const sys_time<milliseconds> at{ milliseconds{ when } };
    // The end of the local day 'when' falls in, or of the next one if it has passed. 17:00 is never skipped or
    // repeated: New York changes its clocks at 02:00.
    local_days day = floor<days>(zone->to_local(at));
    for (;;)
    {
        const sys_time<milliseconds> end = zone->to_sys(day + hours{ DAY_END_HOUR }, choose::earliest);
        if (at < end)
        {
            return static_cast<DateTimeT>(end.time_since_epoch().count());
        }
        day += days{ 1 };
    }
}

bool ExpiryScheduler::expiryOf(const OrderEntry &order, DateTimeT *when, TimerEvent::TimerType *type)
{
    assert(nullptr != when);
    assert(nullptr != type);
    switch (order.tif_)
    {
    case GTD_TIF:
        *when = order.expireTime_;
        *type = TimerEvent::EXPIRATION;
        return true;
    case DAY_TIF:
        // The day it was created in, or a replacement's, replaced in: a replacement keeps its original's
        // creationTime_, and the gateways set its lastUpdateTime_ to the replace's
        *when = dayEndAfter(std::max(order.creationTime_, order.lastUpdateTime_));
        *type = TimerEvent::DAY_END;
        return true;
    default:
        return false;
    }
}

void ExpiryScheduler::schedule(const IdT &orderId, DateTimeT when, TimerEvent::TimerType type)
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        timers_.push(Timer{ when, orderId, type });
    }
    wake_.notify_one();
}

void ExpiryScheduler::start()
{
    std::lock_guard<std::mutex> lock(lock_);
    if (thread_.joinable())
    {
        return;
    }
    stopping_ = false;
    thread_ = std::thread(&ExpiryScheduler::run, this);
}

void ExpiryScheduler::stop()
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

size_t ExpiryScheduler::pending() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return timers_.size();
}

void ExpiryScheduler::takeDue(DateTimeT now, std::vector<Timer> *due)
{
    while (!timers_.empty() && (timers_.top().when_ <= now))
    {
        due->push_back(timers_.top());
        timers_.pop();
    }
}

size_t ExpiryScheduler::fireDue()
{
    std::vector<Timer> due;
    {
        std::lock_guard<std::mutex> lock(lock_);
        takeDue(clock_(), &due);
    }
    for (const Timer &timer : due)
    {
        inQueues_->push(TIMER_SOURCE, TimerEvent(timer.orderId_, timer.type_));
    }
    return due.size();
}

void ExpiryScheduler::run()
{
    std::unique_lock<std::mutex> lock(lock_);
    while (!stopping_)
    {
        std::vector<Timer> due;
        takeDue(clock_(), &due);
        if (!due.empty())
        {
            // Pushed without the lock: schedule() may be called meanwhile, from the threads the queue wakes
            lock.unlock();
            for (const Timer &timer : due)
            {
                inQueues_->push(TIMER_SOURCE, TimerEvent(timer.orderId_, timer.type_));
            }
            lock.lock();
            continue;
        }
        if (timers_.empty())
        {
            wake_.wait(lock);
        }
        else
        {
            // The clock is the system's in the server: wait until the next timer's time, or a sooner one's schedule()
            const auto next = std::chrono::system_clock::time_point(std::chrono::milliseconds(timers_.top().when_));
            wake_.wait_until(lock, next);
        }
    }
}
