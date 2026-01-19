/**
 Concurrent Order Processor library - Google Test Migration

 Original Author: Sergey Mikhailik
 Test Migration: 2026

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
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

TEST_F(NLinkTreeTest, AddSingleNode) {
    int readyToExecute = 0;
    DependObjs deps;
    Transaction* tr = nullptr;

    bool result = tree_->add(1, tr, deps, &readyToExecute);
    EXPECT_TRUE(result);
}

TEST_F(NLinkTreeTest, ClearTree) {
    tree_->clear();
    // Should not crash
    SUCCEED();
}

} // namespace
