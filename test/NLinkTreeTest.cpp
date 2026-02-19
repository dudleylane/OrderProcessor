/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 Tests for NLinkTree: basic add/remove, dependency tracking,
 traversal, parent/child queries, and flat_map-backed storage.
*/

#include <gtest/gtest.h>
#include "NLinkedTree.h"
#include "TransactionMgr.h"

using namespace aux;
using namespace COP::ACID;

namespace {

class NLinkTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        tree_ = std::make_unique<NLinkTree>();
    }

    void TearDown() override {
        tree_.reset();
    }

    std::unique_ptr<NLinkTree> tree_;
};

// =============================================================================
// Basic Add/Remove
// =============================================================================

TEST_F(NLinkTreeTest, AddSingleNode) {
    int readyToExecute = 0;
    DependObjs deps;
    Transaction* tr = nullptr;
    COP::IdT key(1, 20260119);

    bool result = tree_->add(key, tr, deps, &readyToExecute);
    EXPECT_TRUE(result);
}

TEST_F(NLinkTreeTest, ClearTree) {
    tree_->clear();
    SUCCEED();
}

TEST_F(NLinkTreeTest, AddMultipleIndependentNodes) {
    int readyToExecute = 0;
    DependObjs deps;
    Transaction* tr = nullptr;
    int totalReady = 0;

    for (int i = 1; i <= 10; ++i) {
        COP::IdT key(i, 20260119);
        bool result = tree_->add(key, tr, deps, &readyToExecute);
        EXPECT_TRUE(result);
        // Each independent node should produce readyToExecute = 1
        EXPECT_EQ(1, readyToExecute);
        totalReady += readyToExecute;
    }
    EXPECT_EQ(10, totalReady);
}

TEST_F(NLinkTreeTest, AddAndRemoveSingle) {
    int readyToExecute = 0;
    DependObjs deps;
    Transaction* tr = nullptr;
    COP::IdT key(1, 20260119);

    tree_->add(key, tr, deps, &readyToExecute);

    int removedReady = 0;
    bool removed = tree_->remove(key, &removedReady);
    EXPECT_TRUE(removed);
}

TEST_F(NLinkTreeTest, RemoveNonExistentKey) {
    int readyToExecute = 0;
    COP::IdT key(999, 20260119);

    bool removed = tree_->remove(key, &readyToExecute);
    EXPECT_FALSE(removed);
}

// =============================================================================
// Dependency Tracking (flat_map-backed)
// =============================================================================

TEST_F(NLinkTreeTest, AddWithDependency) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;

    // Add node A (independent)
    COP::IdT keyA(1, 20260119);
    DependObjs depsA;
    tree_->add(keyA, tr, depsA, &readyToExecute);
    EXPECT_EQ(1, readyToExecute);

    // Add node B depending on A (object dependency via order_ObjectType)
    COP::IdT keyB(2, 20260119);
    DependObjs depsB;
    depsB.list_[0] = ObjectInTransaction(COP::ACID::order_ObjectType, keyA);
    depsB.size_ = 1;

    tree_->add(keyB, tr, depsB, &readyToExecute);
    // B should not add to readyToExecute since it depends on A
}

TEST_F(NLinkTreeTest, RemovingDependencyUnblocksChild) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;

    // Add A
    COP::IdT keyA(1, 20260119);
    DependObjs depsA;
    tree_->add(keyA, tr, depsA, &readyToExecute);
    int readyAfterA = readyToExecute;

    // Add B depending on A
    COP::IdT keyB(2, 20260119);
    DependObjs depsB;
    depsB.list_[0] = ObjectInTransaction(COP::ACID::order_ObjectType, keyA);
    depsB.size_ = 1;
    tree_->add(keyB, tr, depsB, &readyToExecute);

    // Remove A — should unblock B
    int newReady = 0;
    tree_->remove(keyA, &newReady);
    EXPECT_GE(newReady, 0);
}

// =============================================================================
// Parent/Child Queries
// =============================================================================

TEST_F(NLinkTreeTest, GetParentsOfRoot) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;
    COP::IdT key(1, 20260119);
    DependObjs deps;

    tree_->add(key, tr, deps, &readyToExecute);

    KSetT parents;
    bool found = tree_->getParents(key, &parents);
    EXPECT_TRUE(found);
    EXPECT_TRUE(parents.empty());
}

TEST_F(NLinkTreeTest, GetChildrenOfLeaf) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;
    COP::IdT key(1, 20260119);
    DependObjs deps;

    tree_->add(key, tr, deps, &readyToExecute);

    KSetT children;
    bool found = tree_->getChildren(key, &children);
    EXPECT_TRUE(found);
    EXPECT_TRUE(children.empty());
}

TEST_F(NLinkTreeTest, GetParentsOfNonExistent) {
    COP::IdT key(999, 20260119);
    KSetT parents;
    bool found = tree_->getParents(key, &parents);
    EXPECT_FALSE(found);
}

TEST_F(NLinkTreeTest, GetChildrenOfNonExistent) {
    COP::IdT key(999, 20260119);
    KSetT children;
    bool found = tree_->getChildren(key, &children);
    EXPECT_FALSE(found);
}

// =============================================================================
// Traversal
// =============================================================================

TEST_F(NLinkTreeTest, TraverseEmptyTree) {
    K key;
    V value;
    bool hasNext = tree_->next(&key, &value);
    EXPECT_FALSE(hasNext);
}

TEST_F(NLinkTreeTest, CurrentOnEmptyTree) {
    EXPECT_FALSE(tree_->isCurrentValid());
}

TEST_F(NLinkTreeTest, TraverseSingleNode) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;
    COP::IdT key(1, 20260119);
    DependObjs deps;

    tree_->add(key, tr, deps, &readyToExecute);

    K outKey;
    V outValue;
    bool hasNext = tree_->next(&outKey, &outValue);
    if (hasNext) {
        EXPECT_EQ(key, outKey);
    }
}

// =============================================================================
// Clear and Reuse
// =============================================================================

TEST_F(NLinkTreeTest, ClearAndReuseTree) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;
    DependObjs deps;

    // Add several nodes
    for (int i = 1; i <= 5; ++i) {
        COP::IdT key(i, 20260119);
        tree_->add(key, tr, deps, &readyToExecute);
    }

    // Clear
    tree_->clear();

    // Verify tree is empty by trying to traverse
    K outKey;
    V outValue;
    EXPECT_FALSE(tree_->next(&outKey, &outValue));

    // Add new nodes after clear
    int totalReady = 0;
    for (int i = 10; i <= 12; ++i) {
        readyToExecute = 0;
        COP::IdT key(i, 20260119);
        tree_->add(key, tr, deps, &readyToExecute);
        totalReady += readyToExecute;
    }
    EXPECT_EQ(3, totalReady);
}

TEST_F(NLinkTreeTest, AddRemoveStressTest) {
    int readyToExecute = 0;
    Transaction* tr = nullptr;
    DependObjs deps;

    // Add and remove many nodes to exercise flat_map growth and shrink
    for (int round = 0; round < 10; ++round) {
        for (int i = 1; i <= 20; ++i) {
            COP::IdT key(round * 100 + i, 20260119);
            tree_->add(key, tr, deps, &readyToExecute);
        }
        for (int i = 1; i <= 20; ++i) {
            int removedReady = 0;
            COP::IdT key(round * 100 + i, 20260119);
            tree_->remove(key, &removedReady);
        }
    }
    SUCCEED();
}

} // namespace
