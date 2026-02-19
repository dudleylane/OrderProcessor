/**
 Concurrent Order Processor library - Google Mock Definitions

 Authors: dudleylane, Claude
 Test Infrastructure: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gmock/gmock.h>
#include "QueuesDef.h"

namespace test {

using COP::u32;

/**
 * Mock for InQueues interface - handles incoming event push operations
 */
class MockInQueues : public COP::Queues::InQueues {
public:
    MOCK_METHOD(void, push, (const std::string &source, const COP::Queues::OrderEvent &evnt), (override));
    MOCK_METHOD(void, push, (const std::string &source, const COP::Queues::OrderCancelEvent &evnt), (override));
    MOCK_METHOD(void, push, (const std::string &source, const COP::Queues::OrderReplaceEvent &evnt), (override));
    MOCK_METHOD(void, push, (const std::string &source, const COP::Queues::OrderChangeStateEvent &evnt), (override));
    MOCK_METHOD(void, push, (const std::string &source, const COP::Queues::ProcessEvent &evnt), (override));
    MOCK_METHOD(void, push, (const std::string &source, const COP::Queues::TimerEvent &evnt), (override));
};

/**
 * Mock for InQueueProcessor interface - processes events from incoming queues
 */
class MockInQueueProcessor : public COP::Queues::InQueueProcessor {
public:
    MOCK_METHOD(bool, process, (), (override));
    MOCK_METHOD(void, onEvent, (const std::string &source, const COP::Queues::OrderEvent &evnt), (override));
    MOCK_METHOD(void, onEvent, (const std::string &source, const COP::Queues::OrderCancelEvent &evnt), (override));
    MOCK_METHOD(void, onEvent, (const std::string &source, const COP::Queues::OrderReplaceEvent &evnt), (override));
    MOCK_METHOD(void, onEvent, (const std::string &source, const COP::Queues::OrderChangeStateEvent &evnt), (override));
    MOCK_METHOD(void, onEvent, (const std::string &source, const COP::Queues::ProcessEvent &evnt), (override));
    MOCK_METHOD(void, onEvent, (const std::string &source, const COP::Queues::TimerEvent &evnt), (override));
};

/**
 * Mock for InQueuesObserver interface - receives notifications when new events arrive
 */
class MockInQueuesObserver : public COP::Queues::InQueuesObserver {
public:
    MOCK_METHOD(void, onNewEvent, (), (override));
};

/**
 * Mock for InQueuesPublisher interface - manages processor attachment
 */
class MockInQueuesPublisher : public COP::Queues::InQueuesPublisher {
public:
    MOCK_METHOD(COP::Queues::InQueueProcessor*, attach, (COP::Queues::InQueueProcessor *obs), (override));
    MOCK_METHOD(COP::Queues::InQueueProcessor*, detachProcessor, (), (override));
};

/**
 * Mock for InQueuesContainer interface - manages queue pop operations
 */
class MockInQueuesContainer : public COP::Queues::InQueuesContainer {
public:
    MOCK_METHOD(u32, size, (), (const, override));
    MOCK_METHOD(bool, top, (COP::Queues::InQueueProcessor *obs), (override));
    MOCK_METHOD(bool, pop, (), (override));
    MOCK_METHOD(bool, pop, (COP::Queues::InQueueProcessor *obs), (override));
    MOCK_METHOD(COP::Queues::InQueuesObserver*, attach, (COP::Queues::InQueuesObserver *obs), (override));
    MOCK_METHOD(COP::Queues::InQueuesObserver*, detach, (), (override));
};

/**
 * Mock for OutQueues interface - handles outgoing event push operations
 */
class MockOutQueues : public COP::Queues::OutQueues {
public:
    MOCK_METHOD(void, push, (const COP::Queues::ExecReportEvent &evnt, const std::string &target), (override));
    MOCK_METHOD(void, push, (const COP::Queues::CancelRejectEvent &evnt, const std::string &target), (override));
    MOCK_METHOD(void, push, (const COP::Queues::BusinessRejectEvent &evnt, const std::string &target), (override));
};

} // namespace test
