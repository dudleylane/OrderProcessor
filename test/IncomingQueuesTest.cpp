/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 Migrated from testIncomingQueues.cpp
*/

#include <gtest/gtest.h>
#include "TestAux.h"
#include "MockQueues.h"
#include "IncomingQueues.h"

using namespace COP;
using namespace COP::Queues;
using namespace test;

namespace {

// =============================================================================
// Test Observer - captures events for verification
// =============================================================================

class TestInQueueObserver : public InQueueProcessor {
public:
    bool process() override { return false; }

    void onEvent(const std::string &source, const OrderEvent &evnt) override {
        orders_.push_back({source, evnt});
    }
    void onEvent(const std::string &source, const OrderCancelEvent &evnt) override {
        orderCancels_.push_back({source, evnt});
    }
    void onEvent(const std::string &source, const OrderReplaceEvent &evnt) override {
        orderReplaces_.push_back({source, evnt});
    }
    void onEvent(const std::string &source, const OrderChangeStateEvent &evnt) override {
        orderStates_.push_back({source, evnt});
    }
    void onEvent(const std::string &source, const ProcessEvent &evnt) override {
        processes_.push_back({source, evnt});
    }
    void onEvent(const std::string &source, const TimerEvent &evnt) override {
        timers_.push_back({source, evnt});
    }

    void clearAll() {
        orders_.clear();
        orderCancels_.clear();
        orderReplaces_.clear();
        orderStates_.clear();
        processes_.clear();
        timers_.clear();
    }

    std::deque<std::pair<std::string, OrderEvent>> orders_;
    std::deque<std::pair<std::string, OrderCancelEvent>> orderCancels_;
    std::deque<std::pair<std::string, OrderReplaceEvent>> orderReplaces_;
    std::deque<std::pair<std::string, OrderChangeStateEvent>> orderStates_;
    std::deque<std::pair<std::string, ProcessEvent>> processes_;
    std::deque<std::pair<std::string, TimerEvent>> timers_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class IncomingQueuesTest : public ::testing::Test {
protected:
    void SetUp() override {
        observer_ = std::make_unique<TestInQueueObserver>();
        queues_ = std::make_unique<IncomingQueues>();
    }

    void TearDown() override {
        queues_.reset();
        observer_.reset();
    }

protected:
    std::unique_ptr<TestInQueueObserver> observer_;
    std::unique_ptr<IncomingQueues> queues_;
};

// =============================================================================
// Basic Queue Operations Tests
// =============================================================================

TEST_F(IncomingQueuesTest, EmptyQueueHasSizeZero) {
    EXPECT_EQ(0u, queues_->size());
}

TEST_F(IncomingQueuesTest, TopOnEmptyQueueReturnsFalse) {
    EXPECT_FALSE(queues_->top(observer_.get()));
}

TEST_F(IncomingQueuesTest, PopOnEmptyQueueNoThrow) {
    queues_->pop();
    SUCCEED();
}

// =============================================================================
// Push OrderEvent Tests
// =============================================================================

TEST_F(IncomingQueuesTest, PushOrderEventIncrementsSize) {
    OrderEvent ord;
    ord.id_ = IdT(1, 1);
    queues_->push("source1", ord);
    EXPECT_EQ(1u, queues_->size());
}

TEST_F(IncomingQueuesTest, TopDeliverOrderEvent) {
    OrderEvent ord;
    ord.id_ = IdT(1, 1);
    queues_->push("source1", ord);

    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->orders_.size());
    EXPECT_EQ("source1", observer_->orders_.front().first);
    EXPECT_EQ(IdT(1, 1), observer_->orders_.front().second.id_);
}

// =============================================================================
// Push Cancel Event Tests
// =============================================================================

TEST_F(IncomingQueuesTest, PushCancelEventIncrementsSize) {
    OrderCancelEvent cncl;
    cncl.id_ = IdT(1, 2);
    queues_->push("source2", cncl);
    EXPECT_EQ(1u, queues_->size());
}

// =============================================================================
// Push Replace Event Tests
// =============================================================================

TEST_F(IncomingQueuesTest, PushReplaceEventIncrementsSize) {
    OrderReplaceEvent rpl;
    rpl.id_ = IdT(1, 3);
    queues_->push("source3", rpl);
    EXPECT_EQ(1u, queues_->size());
}

// =============================================================================
// Push ChangeState Event Tests
// =============================================================================

TEST_F(IncomingQueuesTest, PushChangeStateEventIncrementsSize) {
    OrderChangeStateEvent st;
    st.id_ = IdT(1, 4);
    queues_->push("source4", st);
    EXPECT_EQ(1u, queues_->size());
}

// =============================================================================
// Push Process Event Tests
// =============================================================================

TEST_F(IncomingQueuesTest, PushProcessEventIncrementsSize) {
    ProcessEvent pr;
    pr.id_ = IdT(1, 5);
    queues_->push("source5", pr);
    EXPECT_EQ(1u, queues_->size());
}

// =============================================================================
// Push Timer Event Tests
// =============================================================================

TEST_F(IncomingQueuesTest, PushTimerEventIncrementsSize) {
    TimerEvent tr;
    tr.id_ = IdT(1, 6);
    queues_->push("source6", tr);
    EXPECT_EQ(1u, queues_->size());
}

// =============================================================================
// FIFO Order Tests
// =============================================================================

TEST_F(IncomingQueuesTest, EventsAreDeliveredInFIFOOrder) {
    // Push multiple events
    OrderEvent ord1; ord1.id_ = IdT(1, 1);
    OrderEvent ord2; ord2.id_ = IdT(2, 1);
    OrderCancelEvent cncl1; cncl1.id_ = IdT(1, 2);

    queues_->push("1", ord1);
    queues_->push("2", cncl1);
    queues_->push("3", ord2);

    EXPECT_EQ(3u, queues_->size());

    // First event should be ord1
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->orders_.size());
    EXPECT_EQ("1", observer_->orders_.front().first);
    EXPECT_EQ(IdT(1, 1), observer_->orders_.front().second.id_);
}

TEST_F(IncomingQueuesTest, PopDecrementsSize) {
    OrderEvent ord;
    ord.id_ = IdT(1, 1);
    queues_->push("source", ord);

    EXPECT_EQ(1u, queues_->size());
    queues_->pop();
    EXPECT_EQ(0u, queues_->size());
}

// =============================================================================
// Complete Event Type Sequence Test
// =============================================================================

TEST_F(IncomingQueuesTest, AllEventTypesDeliveredInOrder) {
    // Create and push all event types
    OrderEvent ord1; ord1.id_ = IdT(1, 1);
    OrderCancelEvent cncl1; cncl1.id_ = IdT(1, 2);
    OrderReplaceEvent rpl1; rpl1.id_ = IdT(1, 3);
    OrderChangeStateEvent st1; st1.id_ = IdT(1, 4);
    ProcessEvent pr1; pr1.id_ = IdT(1, 5);
    TimerEvent tr1; tr1.id_ = IdT(1, 6);

    queues_->push("1", ord1);
    queues_->push("2", cncl1);
    queues_->push("3", rpl1);
    queues_->push("4", st1);
    queues_->push("5", pr1);
    queues_->push("6", tr1);

    EXPECT_EQ(6u, queues_->size());

    // Verify FIFO order - OrderEvent first
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->orders_.size());
    EXPECT_EQ("1", observer_->orders_.front().first);
    EXPECT_EQ(IdT(1, 1), observer_->orders_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // CancelEvent second
    EXPECT_EQ(5u, queues_->size());
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->orderCancels_.size());
    EXPECT_EQ("2", observer_->orderCancels_.front().first);
    EXPECT_EQ(IdT(1, 2), observer_->orderCancels_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // ReplaceEvent third
    EXPECT_EQ(4u, queues_->size());
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->orderReplaces_.size());
    EXPECT_EQ("3", observer_->orderReplaces_.front().first);
    EXPECT_EQ(IdT(1, 3), observer_->orderReplaces_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // ChangeStateEvent fourth
    EXPECT_EQ(3u, queues_->size());
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->orderStates_.size());
    EXPECT_EQ("4", observer_->orderStates_.front().first);
    EXPECT_EQ(IdT(1, 4), observer_->orderStates_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // ProcessEvent fifth
    EXPECT_EQ(2u, queues_->size());
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->processes_.size());
    EXPECT_EQ("5", observer_->processes_.front().first);
    EXPECT_EQ(IdT(1, 5), observer_->processes_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // TimerEvent sixth
    EXPECT_EQ(1u, queues_->size());
    EXPECT_TRUE(queues_->top(observer_.get()));
    ASSERT_EQ(1u, observer_->timers_.size());
    EXPECT_EQ("6", observer_->timers_.front().first);
    EXPECT_EQ(IdT(1, 6), observer_->timers_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // Queue should be empty now
    EXPECT_EQ(0u, queues_->size());
}

// =============================================================================
// Multiple Events of Same Type Test
// =============================================================================

TEST_F(IncomingQueuesTest, MultipleOrderEventsDeliveredInOrder) {
    OrderEvent ord1; ord1.id_ = IdT(1, 1);
    OrderEvent ord2; ord2.id_ = IdT(2, 1);

    queues_->push("source1", ord1);
    queues_->push("source2", ord2);

    // First should be ord1
    EXPECT_TRUE(queues_->top(observer_.get()));
    EXPECT_EQ(IdT(1, 1), observer_->orders_.front().second.id_);
    observer_->clearAll();
    queues_->pop();

    // Second should be ord2
    EXPECT_TRUE(queues_->top(observer_.get()));
    EXPECT_EQ(IdT(2, 1), observer_->orders_.front().second.id_);
}

// =============================================================================
// Large Queue Test
// =============================================================================

TEST_F(IncomingQueuesTest, HandlesLargeNumberOfEvents) {
    const size_t numEvents = 1000;

    for (size_t i = 0; i < numEvents; ++i) {
        OrderEvent ord;
        ord.id_ = IdT(1, static_cast<u64>(i));
        queues_->push("source", ord);
    }

    EXPECT_EQ(numEvents, static_cast<size_t>(queues_->size()));

    // Verify all can be popped
    for (size_t i = 0; i < numEvents; ++i) {
        EXPECT_TRUE(queues_->top(observer_.get()));
        observer_->clearAll();
        queues_->pop();
    }

    EXPECT_EQ(0u, queues_->size());
}

// =============================================================================
// Top Is Idempotent Test
// =============================================================================

TEST_F(IncomingQueuesTest, TopIsIdempotentWithoutPop) {
    OrderEvent ord;
    ord.id_ = IdT(1, 1);
    queues_->push("source", ord);

    // Multiple tops should return the same event
    EXPECT_TRUE(queues_->top(observer_.get()));
    EXPECT_EQ(IdT(1, 1), observer_->orders_.front().second.id_);
    observer_->clearAll();

    EXPECT_TRUE(queues_->top(observer_.get()));
    EXPECT_EQ(IdT(1, 1), observer_->orders_.front().second.id_);

    // Size should not change
    EXPECT_EQ(1u, queues_->size());
}

} // namespace
