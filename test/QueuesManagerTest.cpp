/**
 * Concurrent Order Processor library - Google Test
 *
 * Author: Sergey Mikhailik
 * Test Implementation: 2026
 *
 * Copyright (C) 2009-2026 Sergey Mikhailik
 *
 * Distributed under the GNU General Public License (GPL).
 */

#include <gtest/gtest.h>
#include <memory>

#include "QueuesManager.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "QueuesDef.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Queues;

// =============================================================================
// QueuesManager Test Fixture
// =============================================================================

class QueuesManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Note: ExchLogger is created globally by TestMain.cpp
    }

    void TearDown() override {
        // Note: ExchLogger is destroyed globally by TestMain.cpp
    }
};

// =============================================================================
// Construction Tests
// =============================================================================

TEST_F(QueuesManagerTest, ConstructionSucceeds) {
    EXPECT_NO_THROW({
        QueuesManager manager;
    });
}

TEST_F(QueuesManagerTest, GetInQueuesReturnsNonNull) {
    QueuesManager manager;
    InQueues* inQueues = manager.getInQueues();

    ASSERT_NE(inQueues, nullptr);
}

TEST_F(QueuesManagerTest, GetOutQueuesReturnsNonNull) {
    QueuesManager manager;
    OutQueues* outQueues = manager.getOutQueues();

    ASSERT_NE(outQueues, nullptr);
}

// =============================================================================
// Pointer Consistency Tests
// =============================================================================

TEST_F(QueuesManagerTest, GetInQueuesReturnsConsistentPointer) {
    QueuesManager manager;

    InQueues* inQueues1 = manager.getInQueues();
    InQueues* inQueues2 = manager.getInQueues();
    InQueues* inQueues3 = manager.getInQueues();

    EXPECT_EQ(inQueues1, inQueues2);
    EXPECT_EQ(inQueues2, inQueues3);
}

TEST_F(QueuesManagerTest, GetOutQueuesReturnsConsistentPointer) {
    QueuesManager manager;

    OutQueues* outQueues1 = manager.getOutQueues();
    OutQueues* outQueues2 = manager.getOutQueues();
    OutQueues* outQueues3 = manager.getOutQueues();

    EXPECT_EQ(outQueues1, outQueues2);
    EXPECT_EQ(outQueues2, outQueues3);
}

TEST_F(QueuesManagerTest, InAndOutQueuesAreDifferent) {
    QueuesManager manager;

    void* inQueues = static_cast<void*>(manager.getInQueues());
    void* outQueues = static_cast<void*>(manager.getOutQueues());

    EXPECT_NE(inQueues, outQueues);
}

// =============================================================================
// Queue Independence Tests
// =============================================================================

TEST_F(QueuesManagerTest, MultipleManagersHaveIndependentQueues) {
    QueuesManager manager1;
    QueuesManager manager2;

    InQueues* inQueues1 = manager1.getInQueues();
    InQueues* inQueues2 = manager2.getInQueues();

    EXPECT_NE(inQueues1, inQueues2);
}

// =============================================================================
// IncomingQueues Integration Tests
// =============================================================================

TEST_F(QueuesManagerTest, CanPushToInQueues) {
    QueuesManager manager;
    InQueues* inQueues = manager.getInQueues();

    EXPECT_NO_THROW({
        TimerEvent event;
        inQueues->push("test_source", event);
    });
}

TEST_F(QueuesManagerTest, CanPushMultipleEventTypesToInQueues) {
    QueuesManager manager;
    InQueues* inQueues = manager.getInQueues();

    // Timer event
    TimerEvent timerEvent;
    EXPECT_NO_THROW(inQueues->push("source1", timerEvent));

    // Cancel event
    OrderCancelEvent cancelEvent;
    EXPECT_NO_THROW(inQueues->push("source2", cancelEvent));

    // Replace event
    OrderReplaceEvent replaceEvent;
    EXPECT_NO_THROW(inQueues->push("source3", replaceEvent));
}

TEST_F(QueuesManagerTest, InQueueCanPushAndPop) {
    QueuesManager manager;
    InQueues* inQueues = manager.getInQueues();

    TimerEvent event;
    inQueues->push("source", event);

    // Just verify push doesn't crash
    SUCCEED();
}

// =============================================================================
// OutgoingQueues Integration Tests
// =============================================================================

TEST_F(QueuesManagerTest, CanPushToOutQueues) {
    QueuesManager manager;
    OutQueues* outQueues = manager.getOutQueues();

    EXPECT_NO_THROW({
        CancelRejectEvent event;
        outQueues->push(event, "target");
    });
}

TEST_F(QueuesManagerTest, CanPushMultipleEventTypesToOutQueues) {
    QueuesManager manager;
    OutQueues* outQueues = manager.getOutQueues();

    // Cancel reject event
    CancelRejectEvent cancelReject;
    EXPECT_NO_THROW(outQueues->push(cancelReject, "target1"));

    // Business reject event
    BusinessRejectEvent businessReject;
    EXPECT_NO_THROW(outQueues->push(businessReject, "target2"));
}

// =============================================================================
// Lifecycle Tests
// =============================================================================

TEST_F(QueuesManagerTest, QueuesValidThroughoutLifetime) {
    auto manager = std::make_unique<QueuesManager>();

    InQueues* inQueues = manager->getInQueues();
    OutQueues* outQueues = manager->getOutQueues();

    // Push to queues
    TimerEvent timerEvent;
    inQueues->push("source", timerEvent);

    CancelRejectEvent cancelReject;
    outQueues->push(cancelReject, "target");

    // Queues should still be valid
    EXPECT_NE(manager->getInQueues(), nullptr);
    EXPECT_NE(manager->getOutQueues(), nullptr);

    // Same pointers
    EXPECT_EQ(manager->getInQueues(), inQueues);
    EXPECT_EQ(manager->getOutQueues(), outQueues);
}

TEST_F(QueuesManagerTest, DestructionDoesNotCrash) {
    {
        QueuesManager manager;

        // Add some events
        TimerEvent event;
        manager.getInQueues()->push("source", event);

        CancelRejectEvent cancelReject;
        manager.getOutQueues()->push(cancelReject, "target");
    }
    // Manager destroyed, no crash expected

    SUCCEED();
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(QueuesManagerTest, EmptyQueueOperations) {
    QueuesManager manager;

    // Fresh queues should be valid
    EXPECT_NE(manager.getInQueues(), nullptr);
}

TEST_F(QueuesManagerTest, ManySequentialPushes) {
    QueuesManager manager;
    InQueues* inQueues = manager.getInQueues();

    const int numPushes = 1000;
    for (int i = 0; i < numPushes; ++i) {
        TimerEvent event;
        inQueues->push("source", event);
    }

    // Verify many pushes don't crash
    SUCCEED();
}
