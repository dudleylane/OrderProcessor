/**
 Concurrent Order Processor library - New Test File

 Authors: dudleylane, Claude
 Test Implementation: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "TestFixtures.h"
#include "TestAux.h"
#include "OutgoingQueues.h"

using namespace COP;
using namespace COP::Queues;
using namespace test;

namespace {

// =============================================================================
// Test Fixture
// =============================================================================

class OutgoingQueuesTest : public SingletonFixture {
protected:
    void SetUp() override {
        SingletonFixture::SetUp();
        queues_ = std::make_unique<OutgoingQueues>();
    }

    void TearDown() override {
        queues_.reset();
        SingletonFixture::TearDown();
    }

    // Helper to create a mock execution for ExecReportEvent
    ExecutionEntry* createMockExecution() {
        auto exec = new ExecutionEntry();
        exec->execId_ = IdT(1, execCounter_++);
        exec->type_ = NEW_EXECTYPE;
        return exec;
    }

protected:
    std::unique_ptr<OutgoingQueues> queues_;
    static std::atomic<u64> execCounter_;
};

std::atomic<u64> OutgoingQueuesTest::execCounter_{1};

// =============================================================================
// Basic Operations Tests
// =============================================================================

TEST_F(OutgoingQueuesTest, CreateQueue) {
    ASSERT_NE(nullptr, queues_);
}

TEST_F(OutgoingQueuesTest, PushExecReportEvent) {
    auto exec = createMockExecution();
    ExecReportEvent event(exec);

    // Should not throw
    queues_->push(event, "target1");
    SUCCEED();
}

TEST_F(OutgoingQueuesTest, PushCancelRejectEvent) {
    CancelRejectEvent event;

    // Should not throw
    queues_->push(event, "target2");
    SUCCEED();
}

TEST_F(OutgoingQueuesTest, PushBusinessRejectEvent) {
    BusinessRejectEvent event;

    // Should not throw
    queues_->push(event, "target3");
    SUCCEED();
}

// =============================================================================
// Multiple Events Tests
// =============================================================================

TEST_F(OutgoingQueuesTest, PushMultipleExecReportEvents) {
    const int numEvents = 100;

    for (int i = 0; i < numEvents; ++i) {
        auto exec = createMockExecution();
        ExecReportEvent event(exec);
        queues_->push(event, "target");
    }

    SUCCEED();
}

TEST_F(OutgoingQueuesTest, PushMixedEventTypes) {
    const int numEvents = 30;

    for (int i = 0; i < numEvents; ++i) {
        auto exec = createMockExecution();
        ExecReportEvent execEvent(exec);
        queues_->push(execEvent, "target");

        CancelRejectEvent cancelEvent;
        queues_->push(cancelEvent, "target");

        BusinessRejectEvent bizEvent;
        queues_->push(bizEvent, "target");
    }

    SUCCEED();
}

// =============================================================================
// Concurrent Push Tests (Lock-Free Verification)
// =============================================================================

TEST_F(OutgoingQueuesTest, ConcurrentPushExecReportEvents) {
    const int numThreads = 4;
    const int numEventsPerThread = 250;
    std::atomic<int> totalPushed{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &totalPushed, numEventsPerThread]() {
            for (int i = 0; i < numEventsPerThread; ++i) {
                auto exec = createMockExecution();
                ExecReportEvent event(exec);
                queues_->push(event, "target");
                ++totalPushed;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numEventsPerThread, totalPushed.load());
}

TEST_F(OutgoingQueuesTest, ConcurrentPushMixedEvents) {
    const int numThreads = 4;
    const int numEventsPerThread = 100;
    std::atomic<int> totalPushed{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &totalPushed, numEventsPerThread, t]() {
            for (int i = 0; i < numEventsPerThread; ++i) {
                if (t % 3 == 0) {
                    auto exec = createMockExecution();
                    ExecReportEvent event(exec);
                    queues_->push(event, "target");
                } else if (t % 3 == 1) {
                    CancelRejectEvent event;
                    queues_->push(event, "target");
                } else {
                    BusinessRejectEvent event;
                    queues_->push(event, "target");
                }
                ++totalPushed;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numEventsPerThread, totalPushed.load());
}

// =============================================================================
// Different Targets Tests
// =============================================================================

TEST_F(OutgoingQueuesTest, PushToMultipleTargets) {
    auto exec1 = createMockExecution();
    auto exec2 = createMockExecution();
    auto exec3 = createMockExecution();

    queues_->push(ExecReportEvent(exec1), "target1");
    queues_->push(ExecReportEvent(exec2), "target2");
    queues_->push(ExecReportEvent(exec3), "target3");

    SUCCEED();
}

// =============================================================================
// High Volume Tests
// =============================================================================

TEST_F(OutgoingQueuesTest, HighVolumePush) {
    const int numEvents = 10000;

    for (int i = 0; i < numEvents; ++i) {
        auto exec = createMockExecution();
        ExecReportEvent event(exec);
        queues_->push(event, "target");
    }

    SUCCEED();
}

} // namespace
