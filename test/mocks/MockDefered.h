/**
 Concurrent Order Processor library - Google Mock Definitions

 Author: Sergey Mikhailik
 Test Infrastructure: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gmock/gmock.h>
#include "DeferedEvents.h"

namespace test {

/**
 * Mock for DeferedEventFunctor interface - processes deferred events
 */
class MockDeferedEventFunctor : public COP::Proc::DeferedEventFunctor {
public:
    MOCK_METHOD(void, process, (COP::OrdState::onTradeExecution &evnt, COP::OrderEntry *order, const COP::ACID::Context &cnxt), (override));
    MOCK_METHOD(void, process, (COP::OrdState::onInternalCancel &evnt, COP::OrderEntry *order, const COP::ACID::Context &cnxt), (override));
};

/**
 * Mock for DeferedEventContainer interface - holds deferred events for later processing
 */
class MockDeferedEventContainer : public COP::Proc::DeferedEventContainer {
public:
    MOCK_METHOD(void, addDeferedEvent, (COP::Proc::DeferedEventBase *evnt), (override));
};

/**
 * Mock for DeferedEventBase interface - represents a deferred event
 */
class MockDeferedEventBase : public COP::Proc::DeferedEventBase {
public:
    MOCK_METHOD(void, execute, (COP::Proc::DeferedEventFunctor *func, const COP::ACID::Context &cnxt, COP::ACID::Scope *scope), (override));
};

} // namespace test
