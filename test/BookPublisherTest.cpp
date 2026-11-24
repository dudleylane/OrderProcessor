/**
 Concurrent Order Processor library - a watched book's depth, sent from the IO thread (#694)

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BookPublisher.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "TestAux.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::App;
using namespace COP::Store;
using namespace std::chrono;

namespace
{

const milliseconds INTERVAL(50);

/// A BookPublisher on an io_context the test runs itself, with a book of two instruments, and a send that keeps what
/// it is given, and when
class BookPublisherTest : public ::testing::Test
{
protected:
    struct Sent
    {
        std::string symbol;
        nlohmann::json json;
        steady_clock::time_point at;
    };

    void SetUp() override
    {
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();
        aapl_ = test::addInstrument("AAPL", "US0378331005");
        msft_ = test::addInstrument("MSFT", "US5949181045");
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(aapl_);
        instruments.insert(msft_);
        book_.init(instruments);
        account_ = test::addAccount("TRADING-1");
        clearing_ = test::addClearing("HOUSE-CLEARING");
    }

    void TearDown() override
    {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    /// A publisher whose every book has a subscriber
    std::shared_ptr<BookPublisher> publisher(milliseconds interval)
    {
        return publisher(interval,
                         [](const std::string &)
                         {
                             return true;
                         });
    }

    /// A publisher that sends to sent_, and asks watched whether a book has a subscriber
    std::shared_ptr<BookPublisher> publisher(milliseconds interval, BookPublisher::Watched watched)
    {
        return BookPublisher::create(
            ioc_, std::move(watched),
            [this](const std::string &symbol, const std::string &json)
            {
                std::lock_guard<std::mutex> lock(sentLock_);
                sent_.push_back({ symbol, nlohmann::json::parse(json), steady_clock::now() });
            },
            &book_, OrderStorage::instance(), interval);
    }

    /// A buy resting in the instrument's book, its leaves published as its last report gave them
    OrderEntry *rest(u64 number, const SourceIdT &instrument, QuantityT leaves)
    {
        const std::string clOrdId = "C-" + std::to_string(number);
        const SourceIdT clOrderId = WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size())));
        auto *order = new OrderEntry(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrument, account_, clearing_,
                                     SourceIdT());
        order->orderId_ = IdT(number, 20261124);
        order->side_ = BUY_SIDE;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->price_ = 150.0;
        order->orderQty_ = 1000;
        order->leavesQty_ = leaves;
        order->cumQty_ = 1000 - leaves;
        order->status_ = NEW_ORDSTATUS;
        OrderStorage::instance()->restore(order); // publishes its figures
        book_.add(*order);
        return order;
    }

    /// A fill's figures published, as its report publishes them before its push
    static void publish(OrderEntry *order, QuantityT leaves)
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, true);
        order->leavesQty_ = leaves;
        order->cumQty_ = order->orderQty_ - leaves;
        order->status_ = PARTFILL_ORDSTATUS;
        order->published_ = OrderFigures(*order);
    }

    /// Runs the context on this thread for up to d, or until it has nothing left to do: the handlers it ran
    std::size_t runFor(steady_clock::duration d)
    {
        ioc_.restart();
        return ioc_.run_for(d);
    }

    std::vector<Sent> sent()
    {
        std::lock_guard<std::mutex> lock(sentLock_);
        return sent_;
    }

    /// The best bid's quantity in a sent book
    static u64 bidQty(const Sent &sent)
    {
        return sent.json["data"]["bids"].at(0)["qty"].get<u64>();
    }

    boost::asio::io_context ioc_;
    OrderBookImpl book_;
    SourceIdT aapl_;
    SourceIdT msft_;
    SourceIdT account_;
    SourceIdT clearing_;
    std::mutex sentLock_;
    std::vector<Sent> sent_;
};

} // namespace

// A change is sent once, an interval after it, with the book as it is then: the IO thread's work, not the reporting
// thread's
TEST_F(BookPublisherTest, AChangeIsSentOnceAnIntervalAfterIt)
{
    rest(1, aapl_, 100);
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL);

    const steady_clock::time_point changedAt = steady_clock::now();
    books->changed(aapl_, "AAPL");
    EXPECT_TRUE(sent().empty()) << "sent on the thread that reported the change";
    runFor(seconds(5));

    const std::vector<Sent> got = sent();
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ("AAPL", got[0].symbol);
    EXPECT_EQ("book_update", got[0].json["type"]);
    EXPECT_EQ(100u, bidQty(got[0]));
    EXPECT_GE(got[0].at - changedAt, INTERVAL) << "sent before the interval ended";
}

// A trade's two reports are pushed back to back: the second's change, within the interval, adds no send, and the one
// send shows the book as both left it. A change after a send goes out an interval after that send at the earliest.
TEST_F(BookPublisherTest, TwoChangesWithinAnIntervalSendOneUpdateWithTheLastState)
{
    OrderEntry *order = rest(1, aapl_, 100);
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL);

    books->changed(aapl_, "AAPL");
    runFor(milliseconds(5)); // the IO thread arms the send
    publish(order, 40);
    books->changed(aapl_, "AAPL");
    runFor(seconds(5));
    std::vector<Sent> got = sent();
    ASSERT_EQ(1u, got.size()) << "the second change was sent on its own";
    EXPECT_EQ(40u, bidQty(got[0])) << "the update showed the book before the last change";

    publish(order, 10);
    books->changed(aapl_, "AAPL");
    runFor(seconds(5));
    got = sent();
    ASSERT_EQ(2u, got.size()) << "a change after the send wasn't sent";
    EXPECT_EQ(10u, bidQty(got[1]));
    EXPECT_GE(got[1].at - got[0].at, INTERVAL) << "two sends within one interval";
}

// Each book changed in an interval goes out at its end, with its own depth, and only a changed one
TEST_F(BookPublisherTest, EachChangedBookIsSentWithItsOwnDepth)
{
    rest(1, aapl_, 100);
    OrderEntry *msft = rest(2, msft_, 70);
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL);

    books->changed(aapl_, "AAPL");
    books->changed(msft_, "MSFT");
    runFor(seconds(5));

    std::vector<Sent> got = sent();
    ASSERT_EQ(2u, got.size());
    std::sort(got.begin(), got.end(),
              [](const Sent &a, const Sent &b)
              {
                  return a.symbol < b.symbol;
              });
    EXPECT_EQ("AAPL", got[0].symbol);
    EXPECT_EQ(100u, bidQty(got[0]));
    EXPECT_EQ("MSFT", got[1].symbol);
    EXPECT_EQ(70u, bidQty(got[1]));

    publish(msft, 30);
    books->changed(msft_, "MSFT");
    runFor(seconds(5));
    got = sent();
    ASSERT_EQ(3u, got.size()) << "a book that didn't change was sent again";
    EXPECT_EQ("MSFT", got[2].symbol);
    EXPECT_EQ(30u, bidQty(got[2]));
}

// A book whose last subscriber left before its send isn't built or sent
TEST_F(BookPublisherTest, ABookNobodyWatchesByItsSendIsntSent)
{
    rest(1, aapl_, 100);
    std::atomic<bool> watched{ true };
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL,
                                                     [&watched](const std::string &)
                                                     {
                                                         return watched.load();
                                                     });

    books->changed(aapl_, "AAPL");
    watched = false;
    runFor(seconds(5));
    EXPECT_TRUE(sent().empty());
}

// Stopped as the server's IO thread ends: a send that was due does nothing
TEST_F(BookPublisherTest, ASendDueWhenItStopsIsntMade)
{
    rest(1, aapl_, 100);
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL);

    books->changed(aapl_, "AAPL");
    runFor(milliseconds(5)); // armed
    books->stop();
    runFor(INTERVAL * 4);
    EXPECT_TRUE(sent().empty()) << "the send that was due went out after stop()";
}

// A change after it stops, which a transaction worker can still report while the server shuts down, gives the IO
// thread nothing to do: stopped once a send has gone out, so that the change would arm another
TEST_F(BookPublisherTest, AChangeAfterItStopsPostsNothing)
{
    rest(1, aapl_, 100);
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL);
    books->changed(aapl_, "AAPL");
    runFor(seconds(5));
    ASSERT_EQ(1u, sent().size());

    books->stop();
    books->changed(aapl_, "AAPL");
    EXPECT_EQ(0u, runFor(INTERVAL * 4)) << "a change after stop() gave the IO thread work";
    EXPECT_EQ(1u, sent().size());
}

// Freed with its WsOutQueues before the context runs again, it leaves nothing that runs: what it posted holds a weak
// reference
TEST_F(BookPublisherTest, WhatItPostedDoesNothingOnceItIsFreed)
{
    rest(1, aapl_, 100);
    std::shared_ptr<BookPublisher> books = publisher(INTERVAL);
    books->changed(aapl_, "AAPL");
    books.reset();
    runFor(INTERVAL * 4);
    EXPECT_TRUE(sent().empty());
}

// Transaction workers report changes while the IO thread sends: each change reaches it, sends are an interval apart,
// and the last state goes out. A worker-to-IO-thread handoff, which the TSan build checks.
TEST_F(BookPublisherTest, ChangesFromSeveralThreadsReachTheIoThreadAndTheLastStateGoesOut)
{
    const milliseconds interval(5);
    OrderEntry *order = rest(1, aapl_, 1000);
    rest(2, msft_, 500);
    std::shared_ptr<BookPublisher> books = publisher(interval);
    auto work = boost::asio::make_work_guard(ioc_);
    std::thread io(
        [this]
        {
            ioc_.run();
        });

    const int changes = 200;
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w)
    {
        workers.emplace_back(
            [&, w]
            {
                for (int i = 0; i < changes; ++i)
                {
                    // One worker fills the AAPL order, publishing each fill before its push, as a report does
                    if (0 == w)
                    {
                        publish(order, static_cast<QuantityT>(1000 - i - 1));
                    }
                    books->changed((0 == (w % 2)) ? aapl_ : msft_, (0 == (w % 2)) ? "AAPL" : "MSFT");
                    std::this_thread::sleep_for(microseconds(100));
                }
            });
    }
    for (std::thread &worker : workers)
    {
        worker.join();
    }

    // The last fill's state goes out, after the last change
    const QuantityT last = 1000 - changes;
    const steady_clock::time_point deadline = steady_clock::now() + seconds(10);
    bool shown = false;
    while (!shown && (steady_clock::now() < deadline))
    {
        for (const Sent &s : sent())
        {
            shown = shown || (("AAPL" == s.symbol) && (last == bidQty(s)));
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    work.reset();
    ioc_.stop();
    io.join();
    books->stop();

    EXPECT_TRUE(shown) << "the last fill's state was never sent";
    const std::vector<Sent> got = sent();
    ASSERT_FALSE(got.empty());
    // A book goes out at most once an interval: the next interval starts once a send has ended
    std::map<std::string, steady_clock::time_point> lastOf;
    for (const Sent &s : got)
    {
        const auto last = lastOf.find(s.symbol);
        if (lastOf.end() != last)
        {
            EXPECT_GE(s.at - last->second, interval) << s.symbol << " sent twice within an interval";
        }
        lastOf[s.symbol] = s.at;
    }
    EXPECT_EQ(1u, lastOf.count("MSFT")) << "the other book's changes were never sent";
}
