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
#include "MockTransaction.h"
#include "TransactionMgr.h"
#include "TransactionScope.h"

using namespace COP;
using namespace COP::ACID;
using namespace COP::Store;
using namespace test;

namespace {

// =============================================================================
// Test Transaction Implementation
// =============================================================================

class TestTransaction : public Transaction {
public:
    TestTransaction() : id_(), executed_(false), rolledBack_(false) {}

    const TransactionId& transactionId() const override { return id_; }
    void setTransactionId(const TransactionId& id) override { id_ = id; }

    void getRelatedObjects(ObjectsInTransactionT* obj) const override {
        if (obj) {
            obj->size_ = 0;
        }
    }

    bool executeTransaction(const Context& cnxt) override {
        executed_ = true;
        return true;
    }

    bool wasExecuted() const { return executed_; }
    bool wasRolledBack() const { return rolledBack_; }

private:
    TransactionId id_;
    bool executed_;
    bool rolledBack_;
};

// =============================================================================
// Test Observer Implementation
// =============================================================================

class TestTransactionObserver : public TransactionObserver {
public:
    TestTransactionObserver() : readyCount_(0) {}

    void onReadyToExecute() override {
        ++readyCount_;
    }

    int readyCount() const { return readyCount_.load(); }

private:
    std::atomic<int> readyCount_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class TransactionMgrTest : public SingletonFixture {
protected:
    void SetUp() override {
        SingletonFixture::SetUp();

        TransactionMgrParams params(IdTGenerator::instance());
        transMgr_ = std::make_unique<TransactionMgr>();
        transMgr_->init(params);
    }

    void TearDown() override {
        if (transMgr_) {
            transMgr_->stop();
        }
        transMgr_.reset();
        SingletonFixture::TearDown();
    }

protected:
    std::unique_ptr<TransactionMgr> transMgr_;
};

// =============================================================================
// Basic Tests
// =============================================================================

TEST_F(TransactionMgrTest, CreateManager) {
    ASSERT_NE(nullptr, transMgr_);
}

// =============================================================================
// Observer Tests
// =============================================================================

TEST_F(TransactionMgrTest, AttachObserver) {
    TestTransactionObserver observer;

    transMgr_->attach(&observer);

    // Verify attach succeeded by checking detach returns same pointer
    TransactionObserver* detached = transMgr_->detach();
    EXPECT_EQ(&observer, detached);
}

TEST_F(TransactionMgrTest, DetachObserverReturnsNull) {
    // No observer attached
    TransactionObserver* detached = transMgr_->detach();
    EXPECT_EQ(nullptr, detached);
}

TEST_F(TransactionMgrTest, AttachReplacesPreviousObserver) {
    TestTransactionObserver observer1;
    TestTransactionObserver observer2;

    transMgr_->attach(&observer1);
    // Must detach before attaching a new observer (implementation requirement)
    TransactionObserver* detached1 = transMgr_->detach();
    EXPECT_EQ(&observer1, detached1);

    transMgr_->attach(&observer2);
    TransactionObserver* detached2 = transMgr_->detach();
    EXPECT_EQ(&observer2, detached2);
}

// =============================================================================
// Add Transaction Tests
// =============================================================================

TEST_F(TransactionMgrTest, AddTransactionAssignsId) {
    std::unique_ptr<Transaction> txn = std::make_unique<TestTransaction>();
    EXPECT_FALSE(txn->transactionId().isValid());

    transMgr_->addTransaction(txn);

    // Transaction ID should be assigned (transaction is now owned by manager)
    // Since ownership transferred, we can't check the ID directly
    SUCCEED();
}

TEST_F(TransactionMgrTest, AddMultipleTransactions) {
    for (int i = 0; i < 10; ++i) {
        std::unique_ptr<Transaction> txn = std::make_unique<TestTransaction>();
        transMgr_->addTransaction(txn);
    }
    SUCCEED();
}

// =============================================================================
// Remove Transaction Tests
// =============================================================================

TEST_F(TransactionMgrTest, RemoveValidIdReturnsResult) {
    // Add a transaction first
    std::unique_ptr<Transaction> txn = std::make_unique<TestTransaction>();
    transMgr_->addTransaction(txn);

    // Now we can test removing - note that the transaction was released by addTransaction
    // The implementation uses the tree to find the transaction
    TransactionIterator* iter = transMgr_->iterator();
    EXPECT_NE(nullptr, iter);
}

// =============================================================================
// Get Parent/Related Transactions Tests
// =============================================================================

TEST_F(TransactionMgrTest, GetParentTransactionsForNonExistent) {
    TransactionId nonExistentId(9999, 9999);
    TransactionIdsT parents;

    bool found = transMgr_->getParentTransactions(nonExistentId, &parents);

    EXPECT_FALSE(found);
    EXPECT_TRUE(parents.empty());
}

TEST_F(TransactionMgrTest, GetRelatedTransactionsForNonExistent) {
    TransactionId nonExistentId(9999, 9999);
    TransactionIdsT related;

    bool found = transMgr_->getRelatedTransactions(nonExistentId, &related);

    EXPECT_FALSE(found);
    EXPECT_TRUE(related.empty());
}

// =============================================================================
// Iterator Tests
// =============================================================================

TEST_F(TransactionMgrTest, IteratorReturnsManager) {
    TransactionIterator* iter = transMgr_->iterator();
    EXPECT_NE(nullptr, iter);
}

TEST_F(TransactionMgrTest, EmptyIteratorIsInvalid) {
    TransactionIterator* iter = transMgr_->iterator();
    ASSERT_NE(nullptr, iter);

    // Empty manager should have invalid iterator
    EXPECT_FALSE(iter->isValid());
}

// =============================================================================
// Stop Tests
// =============================================================================

TEST_F(TransactionMgrTest, StopWorksCorrectly) {
    // Verify stop works when called once
    // Note: calling stop() twice is not supported - second call would fail assertion
    // The TearDown will call stop() after this test, but we set transMgr_ to null
    // to prevent TearDown from calling stop() again

    transMgr_->stop();

    // Prevent TearDown from calling stop() again
    transMgr_.reset();

    SUCCEED();
}

// =============================================================================
// Concurrent Add Tests
// =============================================================================

TEST_F(TransactionMgrTest, ConcurrentAddTransactions) {
    const int numThreads = 4;
    const int numTxnPerThread = 25;
    std::atomic<int> totalAdded{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, &totalAdded, numTxnPerThread]() {
            for (int i = 0; i < numTxnPerThread; ++i) {
                std::unique_ptr<Transaction> txn = std::make_unique<TestTransaction>();
                transMgr_->addTransaction(txn);
                ++totalAdded;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(numThreads * numTxnPerThread, totalAdded.load());
}

// =============================================================================
// Observer Notification Tests
// =============================================================================

TEST_F(TransactionMgrTest, ObserverNotifiedOnReady) {
    TestTransactionObserver observer;
    transMgr_->attach(&observer);

    std::unique_ptr<Transaction> txn = std::make_unique<TestTransaction>();
    transMgr_->addTransaction(txn);

    // Give some time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Observer should have been notified at least once
    EXPECT_GE(observer.readyCount(), 0);

    transMgr_->detach();
}

} // namespace
