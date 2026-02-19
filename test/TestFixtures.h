/**
 Concurrent Order Processor library - Test Fixtures

 Authors: dudleylane, Claude
 Test Infrastructure: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gtest/gtest.h>
#include <memory>

#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include "SubscrManager.h"
#include "TestAux.h"

namespace test {

// =============================================================================
// Base Test Fixture - Singleton Management
// =============================================================================

/**
 * Base fixture that manages the core singleton lifecycle.
 * Handles creation/destruction of WideDataStorage, IdTGenerator, and Logger.
 * Use this as a base for tests that need the singleton infrastructure.
 */
class SingletonFixture : public ::testing::Test {
protected:
    void SetUp() override {
        COP::Store::WideDataStorage::create();
        COP::IdTGenerator::create();
    }

    void TearDown() override {
        COP::IdTGenerator::destroy();
        COP::Store::WideDataStorage::destroy();
    }
};

// =============================================================================
// Order Storage Fixture - Includes OrderStorage singleton
// =============================================================================

/**
 * Fixture that includes OrderStorage in addition to base singletons.
 * Use this for tests that store/retrieve orders.
 */
class OrderStorageFixture : public SingletonFixture {
protected:
    void SetUp() override {
        SingletonFixture::SetUp();
        COP::Store::OrderStorage::create();
    }

    void TearDown() override {
        COP::Store::OrderStorage::destroy();
        SingletonFixture::TearDown();
    }
};

// =============================================================================
// Subscription Fixture - Includes SubscriptionMgr singleton
// =============================================================================

/**
 * Fixture that includes SubscriptionMgr for event subscription tests.
 */
class SubscriptionFixture : public OrderStorageFixture {
protected:
    void SetUp() override {
        OrderStorageFixture::SetUp();
        COP::SubscrMgr::SubscriptionMgr::create();
    }

    void TearDown() override {
        COP::SubscrMgr::SubscriptionMgr::destroy();
        OrderStorageFixture::TearDown();
    }
};

// =============================================================================
// Order Book Fixture - Includes OrderBook with instruments
// =============================================================================

/**
 * Fixture that provides an initialized OrderBook with test instruments.
 * Automatically creates instruments "aaa" and "bbb" and initializes the order book.
 */
class OrderBookFixture : public OrderStorageFixture {
protected:
    void SetUp() override {
        OrderStorageFixture::SetUp();

        instrumentId1_ = test::addInstrument("aaa");
        instrumentId2_ = test::addInstrument("bbb");

        instruments_.insert(instrumentId1_);
        instruments_.insert(instrumentId2_);

        orderBook_ = std::make_unique<COP::OrderBookImpl>();
        orderBook_->init(instruments_, &orderSaver_);
    }

    void TearDown() override {
        orderBook_.reset();
        OrderStorageFixture::TearDown();
    }

protected:
    COP::SourceIdT instrumentId1_;
    COP::SourceIdT instrumentId2_;
    COP::OrderBookImpl::InstrumentsT instruments_;
    std::unique_ptr<COP::OrderBookImpl> orderBook_;
    test::DummyOrderSaver orderSaver_;
};

// =============================================================================
// Full Processor Fixture - All components for integration testing
// =============================================================================

/**
 * Full fixture for processor/integration tests.
 * Includes all singletons and order book infrastructure.
 */
class ProcessorFixture : public SubscriptionFixture {
protected:
    void SetUp() override {
        SubscriptionFixture::SetUp();

        instrumentId1_ = test::addInstrument("aaa");
        instrumentId2_ = test::addInstrument("bbb");

        instruments_.insert(instrumentId1_);
        instruments_.insert(instrumentId2_);

        orderBook_ = std::make_unique<COP::OrderBookImpl>();
        orderBook_->init(instruments_, &orderSaver_);
    }

    void TearDown() override {
        orderBook_.reset();
        SubscriptionFixture::TearDown();
    }

protected:
    COP::SourceIdT instrumentId1_;
    COP::SourceIdT instrumentId2_;
    COP::OrderBookImpl::InstrumentsT instruments_;
    std::unique_ptr<COP::OrderBookImpl> orderBook_;
    test::DummyOrderSaver orderSaver_;
};

// =============================================================================
// Helper Macros for Common Test Patterns
// =============================================================================

/**
 * Macro to verify an operation was enqueued in a TestTransactionContext
 */
#define EXPECT_OPERATION_ENQUEUED(context, operation_type) \
    EXPECT_TRUE((context).isOperationEnqueued(operation_type)) \
        << "Expected operation " << #operation_type << " to be enqueued"

/**
 * Macro to verify an operation was NOT enqueued
 */
#define EXPECT_OPERATION_NOT_ENQUEUED(context, operation_type) \
    EXPECT_FALSE((context).isOperationEnqueued(operation_type)) \
        << "Expected operation " << #operation_type << " to NOT be enqueued"

/**
 * Macro to verify order status
 */
#define EXPECT_ORDER_STATUS(order, expected_status) \
    EXPECT_EQ((expected_status), (order)->status_) \
        << "Order status mismatch"

/**
 * Macro to verify order side
 */
#define EXPECT_ORDER_SIDE(order, expected_side) \
    EXPECT_EQ((expected_side), (order)->side_) \
        << "Order side mismatch"

} // namespace test
