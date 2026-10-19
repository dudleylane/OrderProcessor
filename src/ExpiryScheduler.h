/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "QueuesDef.h"
#include "TypesDef.h"

namespace COP
{
struct OrderEntry;

namespace Proc
{

/// Expires DAY and GTD orders at their time (#240): a GTD order at its expireTime_, and a DAY order at the end of the
/// business day it was created in, or a replacement's, replaced in, 17:00 New York, the FX market's roll, in daylight and standard time alike (ruled
/// 2026-10-16). At each it pushes the order's TimerEvent into the incoming queues, EXPIRATION for a GTD order and
/// DAY_END for a DAY order, and the processor decides it on the transaction worker with the order's other
/// transactions: an order filled, canceled or replaced since is left as it is.
///
/// It restates the orders that outlive a business day, at each day's start (#244): the RESTATEMENT timer, at the day's
/// end, begins the order's figures for the next one (OrderParams::startDay()) and sends a RESTATED report.
class ExpiryScheduler
{
public:
    /// Milliseconds since the epoch, UTC. Tests give a clock of their own.
    using Clock = std::function<DateTimeT()>;

    /// Throws if the time zone database has no DAY_ZONE: DAY orders would expire at the wrong time
    ExpiryScheduler(Queues::InQueues *inQueues, Clock clock);
    ~ExpiryScheduler();
    ExpiryScheduler(const ExpiryScheduler &) = delete;
    ExpiryScheduler &operator=(const ExpiryScheduler &) = delete;

    /// Where the business day ends, and when: 17:00 New York (ruled 2026-10-16), the FX market's roll
    static constexpr const char *DAY_ZONE = "America/New_York";
    static constexpr int DAY_END_HOUR = 17;

    /// The first business day end after the time given: when a DAY order created then expires. 21:00 UTC while New
    /// York keeps daylight time, 22:00 UTC while it keeps standard time.
    static DateTimeT dayEndAfter(DateTimeT when);
    /// The start of the business day the time is in: the last day end at or before it, or 0 before the first (#244).
    /// A day end itself starts the next day. Every match asks, so a thread keeps the day it last answered, for
    /// dayEndAfter() too, and reads the zone database, under a lock of this class's, only when the day changes.
    static DateTimeT dayStartOf(DateTimeT when);
    /// The business day that began at dayStart, as FIX's TradeDate (75) names it: YYYYMMDD, the date it ends on (#258).
    /// It began at 17:00 New York the evening before, 21:00 or 22:00 UTC, so twelve hours on is that date in either
    /// season. 0 for no day.
    static u32 tradeDateOf(DateTimeT dayStart);
    /// When the order expires, and with which timer, if its time in force has an end: false for any other order
    static bool expiryOf(const OrderEntry &order, DateTimeT *when, Queues::TimerEvent::TimerType *type);
    /// When the order's day figures next begin again, if it outlives the business day they are for (#244): an order
    /// that rests, in the price book or the stop book, unless its day ends it first, as it does a DAY order and a GTD
    /// order that expires by then. False for any other order.
    static bool restatementOf(const OrderEntry &order, DateTimeT *when);

    /// Fires the timer at the time given, or at once if it has passed. Thread-safe.
    void schedule(const IdT &orderId, DateTimeT when, Queues::TimerEvent::TimerType type);

    /// Starts the thread that fires the timers as they come due. stop() joins it, and the destructor stops it.
    void start();
    void stop();

    /// Fires every timer due by the clock's time, on the caller's thread, and returns how many: for tests, which don't
    /// start the thread
    size_t fireDue();
    size_t pending() const;

private:
    struct Timer
    {
        DateTimeT when_;
        IdT orderId_;
        Queues::TimerEvent::TimerType type_;
        bool operator>(const Timer &other) const
        {
            return when_ > other.when_;
        }
    };
    void run();
    /// Takes the timers due by now off the queue; the caller holds lock_
    void takeDue(DateTimeT now, std::vector<Timer> *due);

    Queues::InQueues *inQueues_;
    Clock clock_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> timers_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace Proc
} // namespace COP
