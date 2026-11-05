/**
 Concurrent Order Processor library - Engine work in flight gauge Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "InFlightGauge.h"
#include "MockQueues.h"
#include "MockTransaction.h"
#include "TestAux.h"

using namespace COP;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::ReturnPointee;

namespace
{

const std::size_t BOUND = 10;

/// A gauge on a queue and a transaction manager whose counts each test sets (#519)
class InFlightGaugeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ON_CALL(queue_, size()).WillByDefault(ReturnPointee(&queued_));
        ON_CALL(transactions_, pendingTransactions())
            .WillByDefault(Invoke(
                [this]
                {
                    ++reads_;
                    return pending_;
                }));
    }

    /// Asks once the gauge's last count is surely too old, so that it reads the counts again
    bool askAfterRefresh()
    {
        std::this_thread::sleep_for(App::EngineInFlightGauge::REFRESH + std::chrono::milliseconds(1));
        return gauge_.overBound();
    }

    u32 queued_ = 0;
    std::size_t pending_ = 0;
    std::atomic<std::size_t> reads_{ 0 };
    NiceMock<test::MockInQueuesContainer> queue_;
    NiceMock<test::MockTransactionManager> transactions_;
    App::EngineInFlightGauge gauge_{ &queue_, &transactions_, BOUND };
};

// The work in flight is the events waiting and the transactions pending, and reaching the bound is past it
TEST_F(InFlightGaugeTest, CountsTheQueuedEventsAndThePendingTransactions)
{
    queued_ = 4;
    pending_ = 5;
    EXPECT_FALSE(gauge_.overBound());
    pending_ = 6;
    EXPECT_TRUE(askAfterRefresh());
    queued_ = 3;
    EXPECT_FALSE(askAfterRefresh());
    queued_ = 0;
    pending_ = BOUND;
    EXPECT_TRUE(askAfterRefresh());
}

// Counting the transactions takes the manager's lock, so a session asking before each read doesn't take it each time
TEST_F(InFlightGaugeTest, ReadsTheCountsAtMostOnceARefresh)
{
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::milliseconds(20);
    std::size_t asked = 0;
    while (std::chrono::steady_clock::now() < end)
    {
        EXPECT_FALSE(gauge_.overBound());
        ++asked;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LE(1u, reads_.load());
    EXPECT_GE(static_cast<std::size_t>(elapsed / App::EngineInFlightGauge::REFRESH) + 1, reads_.load())
        << asked << " questions";
    EXPECT_LT(reads_.load(), asked);
}

// More than one thread may ask: the one that moves the time on reads the counts, and the others get the last ones
TEST_F(InFlightGaugeTest, ReadsTheCountsAtMostOnceARefreshWhateverThreadAsks)
{
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::milliseconds(50);
    std::vector<std::thread> askers;
    for (int i = 0; i < 4; ++i)
    {
        askers.emplace_back(
            [this, end]
            {
                while (std::chrono::steady_clock::now() < end)
                {
                    EXPECT_FALSE(gauge_.overBound());
                }
            });
    }
    for (std::thread &asker : askers)
    {
        asker.join();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LE(1u, reads_.load());
    EXPECT_GE(static_cast<std::size_t>(elapsed / App::EngineInFlightGauge::REFRESH) + 1, reads_.load());
}

// Once each time the bound is reached, a warning, and once each time the work is back under, a note: not one a question
TEST_F(InFlightGaugeTest, LogsOnceEachTimeTheBoundIsReachedAndLeft)
{
    test::CapturedLog log;
    pending_ = BOUND;
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(askAfterRefresh());
    }
    pending_ = 0;
    queued_ = 2;
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(askAfterRefresh());
    }
    queued_ = BOUND + 1;
    EXPECT_TRUE(askAfterRefresh());

    const std::vector<std::string> lines = log.lines();
    std::vector<std::string> reached;
    std::vector<std::string> left;
    for (const std::string &line : lines)
    {
        if (std::string::npos != line.find("work in flight reached the bound"))
        {
            reached.push_back(line);
        }
        if (std::string::npos != line.find("work in flight is back under the bound"))
        {
            left.push_back(line);
        }
    }
    ASSERT_EQ(2u, reached.size());
    EXPECT_NE(std::string::npos, reached[0].find("10 (--max-in-flight), with 0 events waiting and 10 transactions"))
        << reached[0];
    EXPECT_NE(std::string::npos, reached[1].find("with 11 events waiting and 0 transactions")) << reached[1];
    ASSERT_EQ(1u, left.size());
    EXPECT_NE(std::string::npos, left[0].find("with 2 events waiting and 0 transactions")) << left[0];
}

} // namespace
