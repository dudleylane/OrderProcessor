/**
 Concurrent Order Processor library - Google Test Migration

 Original Author: Sergey Mikhailik
 Test Migration: 2026

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

#include "InterLockCache.h"

using namespace aux;

namespace {

class TestType {
public:
    TestType() : value_(0) {}
    int value_;
};

class InterlockCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        InterLockCache<TestType>::create("test_cache", 10);
    }

    void TearDown() override {
        InterLockCache<TestType>::destroy();
    }
};

TEST_F(InterlockCacheTest, PopAndPushBasic) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    // Pop an item from the cache
    TestType* item = cache->popFront();
    ASSERT_NE(nullptr, item);

    item->value_ = 42;

    // Push it back
    cache->pushBack(item);
}

TEST_F(InterlockCacheTest, MultiplePopPush) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    std::vector<TestType*> items;

    // Pop multiple items
    for (int i = 0; i < 5; ++i) {
        TestType* item = cache->popFront();
        ASSERT_NE(nullptr, item);
        item->value_ = i;
        items.push_back(item);
    }

    // Push them all back
    for (auto* item : items) {
        cache->pushBack(item);
    }
}

TEST_F(InterlockCacheTest, ExceedCacheSize) {
    auto* cache = InterLockCache<TestType>::instance();
    ASSERT_NE(nullptr, cache);

    std::vector<TestType*> items;

    // Pop more items than cache size (should allocate new ones)
    for (int i = 0; i < 15; ++i) {
        TestType* item = cache->popFront();
        ASSERT_NE(nullptr, item);
        items.push_back(item);
    }

    // Push them all back
    for (auto* item : items) {
        cache->pushBack(item);
    }
}

} // namespace
