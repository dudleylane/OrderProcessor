/**
 Concurrent Order Processor library - New Test File

 Authors: dudleylane, Claude
 Test Implementation: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <chrono>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "TestFixtures.h"
#include "TestAux.h"
#include "ExpiryScheduler.h"
#include "ExchUtils.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::Queues;

namespace
{
const DateTimeT DAY = 24ULL * 60 * 60 * 1000;

/// The incoming queues, keeping the timers' events in the order they came
class TimerRecorder final : public InQueues
{
public:
    void push(const std::string &, const OrderEvent &) override {}
    void push(const std::string &, const OrderCancelEvent &) override {}
    void push(const std::string &, const OrderReplaceEvent &) override {}
    void push(const std::string &, const OrderChangeStateEvent &) override {}
    void push(const std::string &, const ProcessEvent &) override {}
    void push(const std::string &, const TimerEvent &evnt) override
    {
        std::lock_guard<std::mutex> lock(lock_);
        fired_.emplace_back(evnt.id_, evnt.timerType_);
    }
    std::vector<std::pair<IdT, TimerEvent::TimerType>> fired() const
    {
        std::lock_guard<std::mutex> lock(lock_);
        return fired_;
    }

private:
    mutable std::mutex lock_;
    std::vector<std::pair<IdT, TimerEvent::TimerType>> fired_;
};

/// Runs the call on a thread of its own, and says whether it returned within five seconds. One that didn't is left
/// running, detached, so a test that finds a call hung fails instead of hanging: it must leave what the call uses alive.
bool returnsWithinFiveSeconds(std::function<void()> call)
{
    auto returned = std::make_shared<std::promise<void>>();
    std::future<void> done = returned->get_future();
    std::thread(
        [call = std::move(call), returned]
        {
            call();
            returned->set_value();
        })
        .detach();
    return std::future_status::ready == done.wait_for(std::chrono::seconds(5));
}
} // namespace

TEST(ExpirySchedulerTest, ADayEndsAt1700NewYork)
{
    // 17:00 New York, the FX market's roll (ruled 2026-10-16): 21:00 UTC in daylight time, 22:00 UTC in standard time.
    // An order made after a day's end belongs to the next day, though UTC's date hasn't changed yet.
    const DateTimeT hour = 60ULL * 60 * 1000;
    const DateTimeT oct15 = 1792022400000ULL; // 2026-10-15 00:00 UTC, 20:00 EDT on the 14th
    EXPECT_EQ(oct15 + 21 * hour, ExpiryScheduler::dayEndAfter(oct15));
    EXPECT_EQ(oct15 + 21 * hour, ExpiryScheduler::dayEndAfter(oct15 + 13 * hour));
    EXPECT_EQ(oct15 + 21 * hour, ExpiryScheduler::dayEndAfter(oct15 + 21 * hour - 1));
    EXPECT_EQ(oct15 + 45 * hour, ExpiryScheduler::dayEndAfter(oct15 + 21 * hour)) << "an order made at the end";
    EXPECT_EQ(oct15 + 45 * hour, ExpiryScheduler::dayEndAfter(oct15 + 23 * hour)) << "19:00 EDT is the next day's";
    const DateTimeT dec15 = 1797292800000ULL; // 2026-12-15 00:00 UTC
    EXPECT_EQ(dec15 + 22 * hour, ExpiryScheduler::dayEndAfter(dec15 + 13 * hour)) << "standard time";
}

TEST(ExpirySchedulerTest, ADayEndsAt1700NewYorkAcrossTheClockChanges)
{
    // New York's clocks change at 02:00 local: the day they do is 23 or 25 hours long, and still ends at 17:00
    const DateTimeT hour = 60ULL * 60 * 1000;
    const DateTimeT mar8 = 1772928000000ULL; // 2026-03-08 00:00 UTC; at 07:00 UTC, 02:00 EST becomes 03:00 EDT
    EXPECT_EQ(mar8 + 21 * hour, ExpiryScheduler::dayEndAfter(mar8 - hour))
        << "the Saturday's evening, to Sunday 17:00 EDT";
    const DateTimeT nov1 = 1793491200000ULL; // 2026-11-01 00:00 UTC; at 06:00 UTC, 02:00 EDT becomes 01:00 EST
    EXPECT_EQ(nov1 + 22 * hour, ExpiryScheduler::dayEndAfter(nov1)) << "the Saturday's evening, to Sunday 17:00 EST";
}

TEST(ExpirySchedulerTest, ADayStartsWhereTheOneBeforeEnds)
{
    // A business day runs from one 17:00 New York to the next (#244), and a day's end starts the next one
    const DateTimeT hour = 60ULL * 60 * 1000;
    const DateTimeT oct15 = 1792022400000ULL; // 2026-10-15 00:00 UTC, 20:00 EDT on the 14th
    EXPECT_EQ(oct15 - 3 * hour, ExpiryScheduler::dayStartOf(oct15 + 13 * hour)) << "17:00 EDT on the 14th";
    EXPECT_EQ(oct15 - 3 * hour, ExpiryScheduler::dayStartOf(oct15 + 21 * hour - 1));
    EXPECT_EQ(oct15 + 21 * hour, ExpiryScheduler::dayStartOf(oct15 + 21 * hour)) << "a day's end starts the next";
    const DateTimeT dec15 = 1797292800000ULL; // 2026-12-15 00:00 UTC
    EXPECT_EQ(dec15 - 2 * hour, ExpiryScheduler::dayStartOf(dec15 + 13 * hour)) << "standard time";
    // Asked about days apart in turn: a thread keeps the day it last answered
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(oct15 - 3 * hour, ExpiryScheduler::dayStartOf(oct15));
        EXPECT_EQ(dec15 - 2 * hour, ExpiryScheduler::dayStartOf(dec15));
    }
}

TEST(ExpirySchedulerTest, ADayStartsWhereTheOneBeforeEndsAcrossTheClockChanges)
{
    // The days New York's clocks change are 23 and 25 hours long
    const DateTimeT hour = 60ULL * 60 * 1000;
    const DateTimeT mar8 = 1772928000000ULL; // 2026-03-08 00:00 UTC; at 07:00 UTC, 02:00 EST becomes 03:00 EDT
    EXPECT_EQ(mar8 - 2 * hour, ExpiryScheduler::dayStartOf(mar8 + 12 * hour)) << "Saturday 17:00 EST";
    EXPECT_EQ(mar8 + 21 * hour, ExpiryScheduler::dayStartOf(mar8 + 21 * hour)) << "Sunday 17:00 EDT";
    const DateTimeT nov1 = 1793491200000ULL; // 2026-11-01 00:00 UTC; at 06:00 UTC, 02:00 EDT becomes 01:00 EST
    EXPECT_EQ(nov1 - 3 * hour, ExpiryScheduler::dayStartOf(nov1 + 12 * hour)) << "Saturday 17:00 EDT";
    EXPECT_EQ(nov1 + 22 * hour, ExpiryScheduler::dayStartOf(nov1 + 22 * hour)) << "Sunday 17:00 EST";
}

TEST(ExpirySchedulerTest, BeforeTheFirstDayEndADayStartsAt0)
{
    // The first day end after the epoch was 1970-01-01 17:00 EST, 22:00 UTC. A time before it, such as a test order's
    // creationTime_, is in no day, rather than one before the epoch, which DateTimeT can't hold.
    const DateTimeT firstDayEnd = 22ULL * 60 * 60 * 1000;
    EXPECT_EQ(0u, ExpiryScheduler::dayStartOf(0));
    EXPECT_EQ(0u, ExpiryScheduler::dayStartOf(100));
    EXPECT_EQ(firstDayEnd, ExpiryScheduler::dayStartOf(firstDayEnd));
}

TEST(ExpirySchedulerTest, ADaysTradeDateIsTheDateItEndsOn)
{
    // FIX's TradeDate (75) for a business day: YYYYMMDD of the date it ends on, at 17:00 New York (#258). A trade after
    // 17:00 takes the next day's date, though New York's date hasn't changed.
    const DateTimeT hour = 60ULL * 60 * 1000;
    auto dateAt = [](DateTimeT when)
    {
        return ExpiryScheduler::tradeDateOf(ExpiryScheduler::dayStartOf(when));
    };
    const DateTimeT oct15 = 1792022400000ULL; // 2026-10-15 00:00 UTC, 20:00 EDT on the 14th
    EXPECT_EQ(20261015u, dateAt(oct15)) << "20:00 EDT on the 14th is in the day that ends on the 15th";
    EXPECT_EQ(20261015u, dateAt(oct15 + 21 * hour - 1));
    EXPECT_EQ(20261016u, dateAt(oct15 + 21 * hour)) << "17:00 EDT starts the 16th's day";
    const DateTimeT dec15 = 1797292800000ULL; // 2026-12-15 00:00 UTC
    EXPECT_EQ(20261215u, dateAt(dec15 + 22 * hour - 1)) << "standard time";
    EXPECT_EQ(20261216u, dateAt(dec15 + 22 * hour));
    // The days New York's clocks change are 23 and 25 hours long, and have their dates as any other
    const DateTimeT mar8 = 1772928000000ULL; // 2026-03-08 00:00 UTC
    EXPECT_EQ(20260308u, dateAt(mar8 + 12 * hour));
    EXPECT_EQ(20260309u, dateAt(mar8 + 21 * hour));
    const DateTimeT nov1 = 1793491200000ULL; // 2026-11-01 00:00 UTC
    EXPECT_EQ(20261101u, dateAt(nov1 + 22 * hour - 1));
    EXPECT_EQ(20261102u, dateAt(nov1 + 22 * hour));
    EXPECT_EQ(0u, ExpiryScheduler::tradeDateOf(0)) << "no day";
}

TEST(ExpirySchedulerTest, DaysLookedUpFromSeveralThreadsAtOnceAgree)
{
    // Each thread's first look at a day reads the zone database, which fills in its transitions on first use. The
    // lookups hold a lock TSan can see (#244): without it the TSan job reports this test, though libstdc++'s own lock,
    // made of atomics TSan can't see, keeps the answers right. Days a year apart send every thread to the database.
    const DateTimeT first = 631152000000ULL; // 1990-01-01 00:00 UTC
    const DateTimeT year = 365 * DAY;
    constexpr size_t years = 41;
    std::vector<std::vector<std::pair<DateTimeT, DateTimeT>>> seen(4);
    std::vector<std::thread> threads;
    for (auto &answers : seen)
    {
        threads.emplace_back(
            [&answers, first, year]
            {
                for (size_t i = 0; i < years; ++i)
                {
                    const DateTimeT when = first + i * year;
                    answers.emplace_back(ExpiryScheduler::dayStartOf(when), ExpiryScheduler::dayEndAfter(when));
                }
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    const DateTimeT hour = 60ULL * 60 * 1000;
    for (size_t i = 0; i < years; ++i)
    {
        const DateTimeT when = first + i * year;
        const DateTimeT start = ExpiryScheduler::dayStartOf(when);
        const DateTimeT end = ExpiryScheduler::dayEndAfter(when);
        EXPECT_LE(start, when);
        EXPECT_LT(when, end);
        EXPECT_LE(23 * hour, end - start) << "a day is 23, 24 or 25 hours long";
        EXPECT_GE(25 * hour, end - start) << "a day is 23, 24 or 25 hours long";
        for (const auto &answers : seen)
        {
            EXPECT_EQ(start, answers[i].first);
            EXPECT_EQ(end, answers[i].second);
        }
    }
}

/// The singletons an order needs
class ExpirySchedulerOrderTest : public test::SingletonFixture
{
};

TEST_F(ExpirySchedulerOrderTest, OnlyADayOrAGtdOrderHasAnExpiry)
{
    auto order = test::createCorrectOrder(test::addInstrument("EXPIRY"));
    order->creationTime_ = 1792022400000ULL + 1000;
    order->expireTime_ = 1792022400000ULL + 5000;
    DateTimeT when = 0;
    TimerEvent::TimerType type = TimerEvent::INVALID_TIMER;

    order->tif_ = GTD_TIF;
    ASSERT_TRUE(ExpiryScheduler::expiryOf(*order, &when, &type));
    EXPECT_EQ(order->expireTime_, when);
    EXPECT_EQ(TimerEvent::EXPIRATION, type);

    order->tif_ = DAY_TIF;
    ASSERT_TRUE(ExpiryScheduler::expiryOf(*order, &when, &type));
    EXPECT_EQ(1792022400000ULL + 21 * 60 * 60 * 1000, when) << "17:00 New York, 21:00 UTC in October";
    EXPECT_EQ(TimerEvent::DAY_END, type);

    for (const TimeInForce tif : { GTC_TIF, IOC_TIF, FOK_TIF, OPG_TIF, ATCLOSE_TIF })
    {
        order->tif_ = tif;
        EXPECT_FALSE(ExpiryScheduler::expiryOf(*order, &when, &type)) << tif;
    }
}

TEST_F(ExpirySchedulerOrderTest, AnOrderThatRestsPastItsDayIsRestatedAtTheDaysEnd)
{
    // #244: an order of any time in force but DAY, if it rests, unless it expires first
    const DateTimeT hour = 60ULL * 60 * 1000;
    const DateTimeT oct15 = 1792022400000ULL; // 2026-10-15 00:00 UTC
    auto order = test::createCorrectOrder(test::addInstrument("RESTATE"));
    order->dayStart_ = oct15 - 3 * hour; // its figures are for the day that began 17:00 EDT on the 14th
    DateTimeT when = 0;
    for (const TimeInForce tif : { GTC_TIF, OPG_TIF, ATCLOSE_TIF })
    {
        order->tif_ = tif;
        ASSERT_TRUE(ExpiryScheduler::restatementOf(*order, &when)) << tif;
        EXPECT_EQ(oct15 + 21 * hour, when) << tif;
    }
    order->tif_ = GTD_TIF;
    order->expireTime_ = oct15 + 21 * hour;
    EXPECT_FALSE(ExpiryScheduler::restatementOf(*order, &when)) << "a GTD order that expires as its day ends";
    order->expireTime_ = oct15 + 21 * hour + 1;
    EXPECT_TRUE(ExpiryScheduler::restatementOf(*order, &when));
    for (const TimeInForce tif : { DAY_TIF, IOC_TIF, FOK_TIF })
    {
        order->tif_ = tif;
        EXPECT_FALSE(ExpiryScheduler::restatementOf(*order, &when)) << tif;
    }
    order->tif_ = GTC_TIF;
    order->ordType_ = MARKET_ORDERTYPE;
    EXPECT_FALSE(ExpiryScheduler::restatementOf(*order, &when)) << "a market order never rests";
    order->ordType_ = STOPLIMIT_ORDERTYPE;
    order->stopPx_ = 1.4;
    EXPECT_TRUE(ExpiryScheduler::restatementOf(*order, &when)) << "a held stop rests in the stop book";
    order->dayStart_ = 0; // an order from before format version 4: restated at once
    ASSERT_TRUE(ExpiryScheduler::restatementOf(*order, &when));
    EXPECT_EQ(22ULL * 60 * 60 * 1000, when);
}

TEST(ExpirySchedulerTest, TimersFireWhenTheyComeDueInTheirOrder)
{
    TimerRecorder queues;
    DateTimeT now = 100;
    ExpiryScheduler expiry(&queues,
                           [&now]
                           {
                               return now;
                           });
    expiry.schedule(IdT(3, 1792022400), 300, TimerEvent::EXPIRATION);
    expiry.schedule(IdT(1, 1792022400), 100, TimerEvent::DAY_END);
    expiry.schedule(IdT(2, 1792022400), 200, TimerEvent::EXPIRATION);
    EXPECT_EQ(3u, expiry.pending());

    EXPECT_EQ(1u, expiry.fireDue()) << "a timer due now fires";
    now = 250;
    EXPECT_EQ(1u, expiry.fireDue());
    EXPECT_EQ(0u, expiry.fireDue()) << "a timer fired twice";
    now = 1000;
    EXPECT_EQ(1u, expiry.fireDue());
    EXPECT_EQ(0u, expiry.pending());

    const auto fired = queues.fired();
    ASSERT_EQ(3u, fired.size());
    EXPECT_EQ(IdT(1, 1792022400), fired[0].first);
    EXPECT_EQ(TimerEvent::DAY_END, fired[0].second);
    EXPECT_EQ(IdT(2, 1792022400), fired[1].first);
    EXPECT_EQ(IdT(3, 1792022400), fired[2].first);
    EXPECT_EQ(TimerEvent::EXPIRATION, fired[2].second);
}

TEST(ExpirySchedulerTest, TheThreadFiresAPastTimerAtOnceAndASoonerOneBeforeALaterOne)
{
    // A restored order whose time passed while the server was down expires as soon as the timers start
    TimerRecorder queues;
    ExpiryScheduler expiry(&queues,
                           []
                           {
                               return aux::currentDateTime();
                           });
    const DateTimeT now = aux::currentDateTime();
    expiry.schedule(IdT(9, 1792022400), now + DAY, TimerEvent::EXPIRATION);
    expiry.start();
    expiry.schedule(IdT(7, 1792022400), now - 1000, TimerEvent::EXPIRATION);
    expiry.schedule(IdT(8, 1792022400), now + 200, TimerEvent::EXPIRATION);
    for (int i = 0; (i < 500) && (queues.fired().size() < 2); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expiry.stop();

    const auto fired = queues.fired();
    ASSERT_EQ(2u, fired.size()) << "the thread waited for the later timer, or fired the next day's";
    EXPECT_EQ(IdT(7, 1792022400), fired[0].first);
    EXPECT_EQ(IdT(8, 1792022400), fired[1].first);
    EXPECT_EQ(1u, expiry.pending());
}

TEST(ExpirySchedulerTest, ATimePastWhatTheSystemClockCanHoldHasNoDeadline)
{
    // The thread waits on the system clock, nanoseconds in an int64. Past its last whole millisecond a timer's time
    // overflowed as it was converted, and the wait returned at once without letting go of the lock (#511)
    using namespace std::chrono;
    EXPECT_EQ(sys_days{ year(2262) / April / 11 } + hours(23) + minutes(47) + seconds(16) + milliseconds(854),
              sys_time<milliseconds>(milliseconds(ExpiryScheduler::LATEST_TIME)));

    for (const DateTimeT when : { DateTimeT(1792022400000ULL), ExpiryScheduler::LATEST_TIME })
    {
        const std::optional<system_clock::time_point> deadline = ExpiryScheduler::deadlineOf(when);
        ASSERT_TRUE(deadline.has_value()) << when;
        EXPECT_EQ(milliseconds(when), deadline->time_since_epoch()) << when;
    }
    const DateTimeT lastSecondOf9999 = 253402300799000ULL; // 9999-12-31T23:59:59Z, which a datetime-local input allows
    for (const DateTimeT when :
         { ExpiryScheduler::LATEST_TIME + 1, lastSecondOf9999, std::numeric_limits<DateTimeT>::max() })
    {
        EXPECT_FALSE(ExpiryScheduler::deadlineOf(when).has_value()) << when;
    }
}

TEST(ExpirySchedulerTest, ATimerPastWhatTheSystemClockCanHoldLetsTheThreadFireTheOthersAndStop)
{
    // A GTD order's expireTime past what the system clock can hold, at the timers' top, made the thread spin holding its
    // lock (#511): every schedule() after it, an event worker's for each new order among them, and stop() waited for
    // good. The thread now waits for it with no deadline: a timer scheduled meanwhile fires, and the thread stops. At the
    // bound itself it waits until that deadline, the latest the clock holds, and the same holds.
    const DateTimeT lastSecondOf9999 = 253402300799000ULL; // 9999-12-31T23:59:59Z
    for (const DateTimeT far : { ExpiryScheduler::LATEST_TIME, ExpiryScheduler::LATEST_TIME + 1, lastSecondOf9999,
                                 std::numeric_limits<DateTimeT>::max() })
    {
        auto queues = std::make_unique<TimerRecorder>();
        auto expiry = std::make_unique<ExpiryScheduler>(queues.get(),
                                                        []
                                                        {
                                                            return aux::currentDateTime();
                                                        });
        ExpiryScheduler *const scheduler = expiry.get();
        scheduler->schedule(IdT(1, 1792022400), far, TimerEvent::EXPIRATION);
        scheduler->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the thread waits, the far timer at the top

        const DateTimeT passed = aux::currentDateTime() - 1000;
        if (!returnsWithinFiveSeconds(
                [scheduler, passed]
                {
                    scheduler->schedule(IdT(2, 1792022400), passed, TimerEvent::EXPIRATION);
                }))
        {
            // Left to the threads that use them: the one that hung, and the scheduler's own
            (void)queues.release();
            (void)expiry.release();
            FAIL() << "schedule() didn't return, with a timer at " << far << " waiting";
        }
        for (int i = 0; (i < 500) && queues->fired().empty(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!returnsWithinFiveSeconds(
                [scheduler]
                {
                    scheduler->stop();
                }))
        {
            (void)queues.release();
            (void)expiry.release();
            FAIL() << "stop() didn't return, with a timer at " << far << " waiting";
        }

        EXPECT_EQ(
            (std::vector<std::pair<IdT, TimerEvent::TimerType>>{ { IdT(2, 1792022400), TimerEvent::EXPIRATION } }),
            queues->fired())
            << "the timer scheduled after one at " << far;
        EXPECT_EQ(1u, scheduler->pending()) << "the one at " << far << " waits on";
    }
}
