/**
 Concurrent Order Processor library - Google Mock Definitions

 Author: Sergey Mikhailik
 Test Infrastructure: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gmock/gmock.h>
#include "DataModelDef.h"

namespace test {

using COP::SourceIdT;
using COP::IdT;
using COP::OrderBook;

/**
 * Mock for OrderFunctor interface - provides order matching logic
 * Note: OrderFunctor has a protected side_ member, so we need to handle that
 */
class MockOrderFunctor : public COP::OrderFunctor {
public:
    MockOrderFunctor() { side_ = COP::INVALID_SIDE; }
    explicit MockOrderFunctor(COP::Side side) { side_ = side; }

    void setSide(COP::Side side) { side_ = side; }

    MOCK_METHOD(SourceIdT, instrument, (), (const, override));
    MOCK_METHOD(bool, match, (const IdT &order, bool *stop), (const, override));
};

/**
 * Mock for OrderBook interface - manages order storage and matching
 */
class MockOrderBook : public COP::OrderBook {
public:
    MOCK_METHOD(void, add, (const COP::OrderEntry& order), (override));
    MOCK_METHOD(void, remove, (const COP::OrderEntry& order), (override));
    MOCK_METHOD(IdT, find, (const COP::OrderFunctor &functor), (const, override));
    MOCK_METHOD(void, findAll, (const COP::OrderFunctor &functor, OrdersT *result), (const, override));
    MOCK_METHOD(IdT, getTop, (const SourceIdT &instrument, const COP::Side &side), (const, override));
    MOCK_METHOD(void, restore, (const COP::OrderEntry& order), (override));
};

/**
 * Mock for OrderSaver interface - handles order persistence
 */
class MockOrderSaver : public COP::OrderSaver {
public:
    MOCK_METHOD(void, save, (const COP::OrderEntry& order), (override));
};

/**
 * Configurable test functor for order book testing - not a mock but a test double
 * This provides a concrete implementation for tests that need predictable behavior
 */
class TestOrderFunctor : public COP::OrderFunctor {
public:
    TestOrderFunctor(const SourceIdT &instr, COP::Side side, bool matchResult)
        : instr_(instr), matchResult_(matchResult) {
        side_ = side;
    }

    SourceIdT instrument() const override { return instr_; }
    bool match(const IdT &, bool *) const override { return matchResult_; }

    void setInstrument(const SourceIdT &instr) { instr_ = instr; }
    void setMatchResult(bool result) { matchResult_ = result; }
    void setSide(COP::Side side) { side_ = side; }

private:
    SourceIdT instr_;
    bool matchResult_;
};

} // namespace test
