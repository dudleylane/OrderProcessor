/**
 * Concurrent Order Processor library - Transaction Scope Tests
 *
 * Authors: dudleylane, Claude
 * Test Implementation: 2026
 *
 * Copyright (C) 2026 dudleylane
 *
 * Distributed under the GNU General Public License (GPL).
 *
 * Tests for TransactionScope rollback behavior, ACID compliance,
 * and arena allocator boundary conditions.
 */

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <stdexcept>

#include "TransactionScope.h"
#include "TransactionDef.h"

using namespace COP;
using namespace COP::ACID;

namespace {

// =============================================================================
// Test Operation Classes
// =============================================================================

/**
 * Operation that tracks execution and rollback calls
 */
class TrackingOperation : public Operation {
public:
    TrackingOperation(int id, std::vector<int>* execOrder, std::vector<int>* rollbackOrder)
        : Operation(MATCH_ORDER_TROPERATION, IdT(id, 1))
        , id_(id)
        , execOrder_(execOrder)
        , rollbackOrder_(rollbackOrder)
        , executed_(false)
        , rolledBack_(false)
    {}

    void execute(const Context&) override {
        executed_ = true;
        if (execOrder_) {
            execOrder_->push_back(id_);
        }
    }

    void rollback(const Context&) override {
        rolledBack_ = true;
        if (rollbackOrder_) {
            rollbackOrder_->push_back(id_);
        }
    }

    bool wasExecuted() const { return executed_; }
    bool wasRolledBack() const { return rolledBack_; }
    int id() const { return id_; }

private:
    int id_;
    std::vector<int>* execOrder_;
    std::vector<int>* rollbackOrder_;
    bool executed_;
    bool rolledBack_;
};

/**
 * Operation that throws an exception during execute
 */
class FailingOperation : public Operation {
public:
    FailingOperation(int id, std::vector<int>* rollbackOrder)
        : Operation(MATCH_ORDER_TROPERATION, IdT(id, 1))
        , id_(id)
        , rollbackOrder_(rollbackOrder)
        , rolledBack_(false)
    {}

    void execute(const Context&) override {
        throw std::runtime_error("Intentional failure for testing");
    }

    void rollback(const Context&) override {
        rolledBack_ = true;
        if (rollbackOrder_) {
            rollbackOrder_->push_back(id_);
        }
    }

    bool wasRolledBack() const { return rolledBack_; }

private:
    int id_;
    std::vector<int>* rollbackOrder_;
    bool rolledBack_;
};

/**
 * Operation that throws during rollback
 */
class FailingRollbackOperation : public Operation {
public:
    FailingRollbackOperation(int id, std::vector<int>* rollbackOrder)
        : Operation(MATCH_ORDER_TROPERATION, IdT(id, 1))
        , id_(id)
        , rollbackOrder_(rollbackOrder)
    {}

    void execute(const Context&) override {
        // Success
    }

    void rollback(const Context&) override {
        if (rollbackOrder_) {
            rollbackOrder_->push_back(id_);
        }
        throw std::runtime_error("Rollback failure for testing");
    }

private:
    int id_;
    std::vector<int>* rollbackOrder_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class TransactionScopeTest : public ::testing::Test {
protected:
    void SetUp() override {
        execOrder_.clear();
        rollbackOrder_.clear();
    }

    Context createContext() {
        return Context();
    }

    std::vector<int> execOrder_;
    std::vector<int> rollbackOrder_;
};

// =============================================================================
// Basic Execution Tests
// =============================================================================

TEST_F(TransactionScopeTest, ExecuteEmptyTransaction_ReturnsTrue) {
    TransactionScope scope;
    Context ctx = createContext();

    bool result = scope.executeTransaction(ctx);

    EXPECT_TRUE(result);
}

TEST_F(TransactionScopeTest, ExecuteSingleOperation_CallsExecute) {
    TransactionScope scope;
    auto op = std::make_unique<TrackingOperation>(1, &execOrder_, &rollbackOrder_);
    TrackingOperation* opPtr = op.get();

    std::unique_ptr<Operation> baseOp(op.release());
    scope.addOperation(baseOp);

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_TRUE(result);
    EXPECT_TRUE(opPtr->wasExecuted());
    EXPECT_FALSE(opPtr->wasRolledBack());
    EXPECT_EQ(1u, execOrder_.size());
    EXPECT_EQ(1, execOrder_[0]);
}

TEST_F(TransactionScopeTest, ExecuteMultipleOperations_CallsAllInOrder) {
    TransactionScope scope;

    for (int i = 1; i <= 5; ++i) {
        auto op = std::make_unique<TrackingOperation>(i, &execOrder_, &rollbackOrder_);
        std::unique_ptr<Operation> baseOp(op.release());
        scope.addOperation(baseOp);
    }

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_TRUE(result);
    ASSERT_EQ(5u, execOrder_.size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(i + 1, execOrder_[i]);
    }
    EXPECT_TRUE(rollbackOrder_.empty());
}

// =============================================================================
// Rollback Tests
// =============================================================================

TEST_F(TransactionScopeTest, ExecuteReturnsFalse_WhenOperationThrows) {
    TransactionScope scope;

    auto op = std::make_unique<FailingOperation>(1, &rollbackOrder_);
    std::unique_ptr<Operation> baseOp(op.release());
    scope.addOperation(baseOp);

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_FALSE(result);
}

TEST_F(TransactionScopeTest, RollbackCalledOnFailure_ForExecutedOperations) {
    TransactionScope scope;

    // Add two successful operations, then one that fails
    auto op1 = std::make_unique<TrackingOperation>(1, &execOrder_, &rollbackOrder_);
    auto op2 = std::make_unique<TrackingOperation>(2, &execOrder_, &rollbackOrder_);
    auto op3 = std::make_unique<FailingOperation>(3, &rollbackOrder_);

    TrackingOperation* op1Ptr = op1.get();
    TrackingOperation* op2Ptr = op2.get();

    std::unique_ptr<Operation> baseOp1(op1.release());
    std::unique_ptr<Operation> baseOp2(op2.release());
    std::unique_ptr<Operation> baseOp3(op3.release());

    scope.addOperation(baseOp1);
    scope.addOperation(baseOp2);
    scope.addOperation(baseOp3);

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_FALSE(result);
    EXPECT_TRUE(op1Ptr->wasExecuted());
    EXPECT_TRUE(op2Ptr->wasExecuted());
    EXPECT_TRUE(op1Ptr->wasRolledBack());
    EXPECT_TRUE(op2Ptr->wasRolledBack());
}

TEST_F(TransactionScopeTest, RollbackOrder_IsReverseOfExecution) {
    TransactionScope scope;

    // Add three successful operations, then one that fails
    for (int i = 1; i <= 3; ++i) {
        auto op = std::make_unique<TrackingOperation>(i, &execOrder_, &rollbackOrder_);
        std::unique_ptr<Operation> baseOp(op.release());
        scope.addOperation(baseOp);
    }

    auto failOp = std::make_unique<FailingOperation>(4, &rollbackOrder_);
    std::unique_ptr<Operation> baseFailOp(failOp.release());
    scope.addOperation(baseFailOp);

    Context ctx = createContext();
    scope.executeTransaction(ctx);

    // Execution order: 1, 2, 3 (4 fails)
    ASSERT_EQ(3u, execOrder_.size());
    EXPECT_EQ(1, execOrder_[0]);
    EXPECT_EQ(2, execOrder_[1]);
    EXPECT_EQ(3, execOrder_[2]);

    // Rollback order: 4, 3, 2, 1 (reverse, including failed op)
    ASSERT_EQ(4u, rollbackOrder_.size());
    EXPECT_EQ(4, rollbackOrder_[0]);
    EXPECT_EQ(3, rollbackOrder_[1]);
    EXPECT_EQ(2, rollbackOrder_[2]);
    EXPECT_EQ(1, rollbackOrder_[3]);
}

TEST_F(TransactionScopeTest, RollbackNotCalled_ForUnexecutedOperations) {
    TransactionScope scope;

    // First operation fails immediately
    auto failOp = std::make_unique<FailingOperation>(1, &rollbackOrder_);
    auto op2 = std::make_unique<TrackingOperation>(2, &execOrder_, &rollbackOrder_);
    TrackingOperation* op2Ptr = op2.get();

    std::unique_ptr<Operation> baseFailOp(failOp.release());
    std::unique_ptr<Operation> baseOp2(op2.release());

    scope.addOperation(baseFailOp);
    scope.addOperation(baseOp2);

    Context ctx = createContext();
    scope.executeTransaction(ctx);

    // Op2 should not have been executed or rolled back
    EXPECT_FALSE(op2Ptr->wasExecuted());
    EXPECT_FALSE(op2Ptr->wasRolledBack());

    // Only op1's rollback should be called
    ASSERT_EQ(1u, rollbackOrder_.size());
    EXPECT_EQ(1, rollbackOrder_[0]);
}

TEST_F(TransactionScopeTest, RollbackStops_WhenRollbackThrows) {
    // Documents current behavior: rollback stops on first exception
    // This is a design trade-off; some systems continue rollback on exception
    TransactionScope scope;

    // Op1: normal, Op2: throws during rollback, Op3: normal, Op4: fails during execute
    auto op1 = std::make_unique<TrackingOperation>(1, &execOrder_, &rollbackOrder_);
    auto op2 = std::make_unique<FailingRollbackOperation>(2, &rollbackOrder_);
    auto op3 = std::make_unique<TrackingOperation>(3, &execOrder_, &rollbackOrder_);
    auto op4 = std::make_unique<FailingOperation>(4, &rollbackOrder_);

    TrackingOperation* op1Ptr = op1.get();
    TrackingOperation* op3Ptr = op3.get();

    std::unique_ptr<Operation> baseOp1(op1.release());
    std::unique_ptr<Operation> baseOp2(op2.release());
    std::unique_ptr<Operation> baseOp3(op3.release());
    std::unique_ptr<Operation> baseOp4(op4.release());

    scope.addOperation(baseOp1);
    scope.addOperation(baseOp2);
    scope.addOperation(baseOp3);
    scope.addOperation(baseOp4);

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_FALSE(result);

    // Rollback order is: 4, 3, 2 (throws), stops
    // Op1's rollback is NOT called because op2's rollback throws first
    EXPECT_FALSE(op1Ptr->wasRolledBack());
    // Op3's rollback IS called (before op2)
    EXPECT_TRUE(op3Ptr->wasRolledBack());
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(TransactionScopeTest, FirstOperationFails_OnlyFirstRolledBack) {
    TransactionScope scope;

    auto failOp = std::make_unique<FailingOperation>(1, &rollbackOrder_);
    std::unique_ptr<Operation> baseFailOp(failOp.release());
    scope.addOperation(baseFailOp);

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_FALSE(result);
    ASSERT_EQ(1u, rollbackOrder_.size());
    EXPECT_EQ(1, rollbackOrder_[0]);
}

TEST_F(TransactionScopeTest, LastOperationFails_AllRolledBack) {
    TransactionScope scope;

    for (int i = 1; i <= 4; ++i) {
        auto op = std::make_unique<TrackingOperation>(i, &execOrder_, &rollbackOrder_);
        std::unique_ptr<Operation> baseOp(op.release());
        scope.addOperation(baseOp);
    }

    auto failOp = std::make_unique<FailingOperation>(5, &rollbackOrder_);
    std::unique_ptr<Operation> baseFailOp(failOp.release());
    scope.addOperation(baseFailOp);

    Context ctx = createContext();
    bool result = scope.executeTransaction(ctx);

    EXPECT_FALSE(result);
    ASSERT_EQ(4u, execOrder_.size());
    ASSERT_EQ(5u, rollbackOrder_.size());

    // Verify reverse order
    EXPECT_EQ(5, rollbackOrder_[0]);
    EXPECT_EQ(4, rollbackOrder_[1]);
    EXPECT_EQ(3, rollbackOrder_[2]);
    EXPECT_EQ(2, rollbackOrder_[3]);
    EXPECT_EQ(1, rollbackOrder_[4]);
}

TEST_F(TransactionScopeTest, TransactionId_CanBeSetAndRetrieved) {
    TransactionScope scope;
    TransactionId id(123, 456);

    scope.setTransactionId(id);

    EXPECT_EQ(id, scope.transactionId());
}

TEST_F(TransactionScopeTest, RemoveLastOperation_RemovesCorrectly) {
    TransactionScope scope;

    auto op1 = std::make_unique<TrackingOperation>(1, &execOrder_, &rollbackOrder_);
    auto op2 = std::make_unique<TrackingOperation>(2, &execOrder_, &rollbackOrder_);

    std::unique_ptr<Operation> baseOp1(op1.release());
    std::unique_ptr<Operation> baseOp2(op2.release());

    scope.addOperation(baseOp1);
    scope.addOperation(baseOp2);
    scope.removeLastOperation();

    Context ctx = createContext();
    scope.executeTransaction(ctx);

    // Only op1 should have been executed
    ASSERT_EQ(1u, execOrder_.size());
    EXPECT_EQ(1, execOrder_[0]);
}

// =============================================================================
// Arena Allocator Tests
// =============================================================================

TEST_F(TransactionScopeTest, ArenaAllocateReturnsNonNull) {
    TransactionScope scope;

    void* ptr = scope.arenaAllocate(64, alignof(std::max_align_t));
    EXPECT_NE(nullptr, ptr);
}

TEST_F(TransactionScopeTest, ArenaAllocateRespectsSizeLimit) {
    TransactionScope scope;

    // ARENA_SIZE is 2048 — allocating exactly that should succeed
    void* ptr = scope.arenaAllocate(TransactionScope::ARENA_SIZE, 1);
    EXPECT_NE(nullptr, ptr);

    // Arena is now full — next allocation should return nullptr
    void* ptr2 = scope.arenaAllocate(1, 1);
    EXPECT_EQ(nullptr, ptr2);
}

TEST_F(TransactionScopeTest, ArenaAllocateOversizeReturnsNull) {
    TransactionScope scope;

    // Requesting more than ARENA_SIZE should return nullptr immediately
    void* ptr = scope.arenaAllocate(TransactionScope::ARENA_SIZE + 1, 1);
    EXPECT_EQ(nullptr, ptr);
}

TEST_F(TransactionScopeTest, ArenaAllocateMultipleSmallAllocations) {
    TransactionScope scope;

    // Allocate many small objects until arena is full
    int count = 0;
    while (true) {
        void* ptr = scope.arenaAllocate(64, alignof(std::max_align_t));
        if (!ptr) break;
        ++count;
    }

    // With ARENA_SIZE = 2048 and 64-byte allocations (potentially aligned),
    // we should get at least several allocations
    EXPECT_GE(count, 10);
}

TEST_F(TransactionScopeTest, IsFromArenaIdentifiesArenaPointers) {
    TransactionScope scope;

    void* arenaPtr = scope.arenaAllocate(64, alignof(std::max_align_t));
    ASSERT_NE(nullptr, arenaPtr);

    EXPECT_TRUE(scope.isFromArena(arenaPtr));
}

TEST_F(TransactionScopeTest, IsFromArenaRejectsHeapPointers) {
    TransactionScope scope;

    int heapVar = 42;
    EXPECT_FALSE(scope.isFromArena(&heapVar));

    auto heapAlloc = std::make_unique<char[]>(64);
    EXPECT_FALSE(scope.isFromArena(heapAlloc.get()));
}

TEST_F(TransactionScopeTest, IsFromArenaRejectsNull) {
    TransactionScope scope;
    EXPECT_FALSE(scope.isFromArena(nullptr));
}

TEST_F(TransactionScopeTest, ResetClearsArena) {
    TransactionScope scope;

    // Fill up most of the arena
    for (int i = 0; i < 20; ++i) {
        scope.arenaAllocate(64, alignof(std::max_align_t));
    }

    // Reset should reclaim arena space
    scope.reset();

    // Should be able to allocate again
    void* ptr = scope.arenaAllocate(64, alignof(std::max_align_t));
    EXPECT_NE(nullptr, ptr);
}

TEST_F(TransactionScopeTest, ActiveScopeIsThreadLocal) {
    TransactionScope scope1;
    TransactionScope scope2;

    TransactionScope::s_activeScope = &scope1;
    EXPECT_EQ(&scope1, TransactionScope::s_activeScope);

    // Verify it's thread-local: another thread sees nullptr
    TransactionScope* otherThreadScope = nullptr;
    std::thread t([&]() {
        otherThreadScope = TransactionScope::s_activeScope;
    });
    t.join();

    EXPECT_EQ(nullptr, otherThreadScope);
    EXPECT_EQ(&scope1, TransactionScope::s_activeScope);

    // Cleanup
    TransactionScope::s_activeScope = nullptr;
}

TEST_F(TransactionScopeTest, SwapExchangesContents) {
    TransactionScope scope1;
    TransactionScope scope2;

    TransactionId id1(1, 100);
    TransactionId id2(2, 200);

    scope1.setTransactionId(id1);
    scope2.setTransactionId(id2);

    scope1.swap(scope2);

    EXPECT_EQ(id2, scope1.transactionId());
    EXPECT_EQ(id1, scope2.transactionId());
}

TEST_F(TransactionScopeTest, ArenaArenaSizeConstant) {
    // Verify ARENA_SIZE is at least 1024 as documented
    EXPECT_GE(TransactionScope::ARENA_SIZE, 1024u);
}

} // namespace
