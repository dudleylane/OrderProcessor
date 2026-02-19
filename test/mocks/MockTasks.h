/**
 Concurrent Order Processor library - Google Mock Definitions

 Authors: dudleylane, Claude
 Test Infrastructure: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gmock/gmock.h>
#include "TasksDef.h"

namespace test {

/**
 * Mock for ExecTask interface - represents an executable task
 */
class MockExecTask : public COP::Tasks::ExecTask {
public:
    MOCK_METHOD(void, execute, (), (override));
};

/**
 * Mock for ExecTaskManager interface - manages task scheduling
 */
class MockExecTaskManager : public COP::Tasks::ExecTaskManager {
public:
    MOCK_METHOD(void, addTask, (const COP::ACID::TransactionId &id), (override));
};

} // namespace test
