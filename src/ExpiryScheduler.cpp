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
#include <mutex>

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

/// Held for every lookup in the zone database. libstdc++'s time_zone fills in its transitions on first use, under a lock
/// of its own made of atomics inside libstdc++.so (GCC 14's tzdb.cc, RulesCounter), which ThreadSanitizer cannot see.
/// Two threads asking about a day for the first time at once were reported as a race, and TSan gates CI. This mutex is
/// one TSan sees, and with the per-thread day below it is taken once a day per thread.
std::mutex zoneLock;

/// A business day, from one day end to the next: [start, end)
struct BusinessDay
{
    DateTimeT start;
    DateTimeT end;
};

/// The business day the time is in, from the zone database
BusinessDay lookUpDay(DateTimeT when)
{
    using namespace std::chrono;
    const time_zone *const zone = dayZone();
    const sys_time<milliseconds> at{ milliseconds{ when } };
    std::lock_guard<std::mutex> lock(zoneLock);
    // The end of the local day 'when' falls in, or of the next one if it has passed. 17:00 is never skipped or
    // repeated: New York changes its clocks at 02:00.
    local_days day = floor<days>(zone->to_local(at));
    for (;;)
    {
        const sys_time<milliseconds> end = zone->to_sys(day + hours{ ExpiryScheduler::DAY_END_HOUR }, choose::earliest);
        if (at < end)
        {
            // The day end before it is 17:00 of the local day before: there is one each day
            const sys_time<milliseconds> start =
                zone->to_sys(day - days{ 1 } + hours{ ExpiryScheduler::DAY_END_HOUR }, choose::earliest);
            const auto startMs = start.time_since_epoch().count();
            return BusinessDay{ (0 < startMs) ? static_cast<DateTimeT>(startMs) : 0,
                                static_cast<DateTimeT>(end.time_since_epoch().count()) };
        }
        day += days{ 1 };
    }
}

/// The business day the time is in, from the day this thread answered last when it is the same one: every match asks
/// (#244), and every DAY order (#240)
const BusinessDay &dayOf(DateTimeT when)
{
    thread_local BusinessDay day{ 0, 0 };
    if ((day.start <= when) && (when < day.end)) [[likely]]
    {
        return day;
    }
    day = lookUpDay(when);
    return day;
}
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
    return dayOf(when).end;
}

DateTimeT ExpiryScheduler::dayStartOf(DateTimeT when)
{
    return dayOf(when).start;
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

bool ExpiryScheduler::restatementOf(const OrderEntry &order, DateTimeT *when)
{
    assert(nullptr != when);
    if ((DAY_TIF == order.tif_) || (OrderParams::Book::NONE == order.book()))
    {
        return false; // ended by its day, or never rests: a market, IOC or FOK order
    }
    const DateTimeT dayEnd = dayEndAfter(order.dayStart_);
    if ((GTD_TIF == order.tif_) && (order.expireTime_ <= dayEnd))
    {
        return false; // expires first
    }
    *when = dayEnd;
    return true;
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
