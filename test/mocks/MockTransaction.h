/**
 Concurrent Order Processor library - Google Mock Definitions

 Author: Sergey Mikhailik
 Test Infrastructure: 2026

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).
*/

#pragma once

#include <gmock/gmock.h>
#include "TransactionDef.h"

namespace test {

using COP::IdT;

/**
 * Mock for TransactionObserver interface - receives notifications when transactions are ready
 */
class MockTransactionObserver : public COP::ACID::TransactionObserver {
public:
    MOCK_METHOD(void, onReadyToExecute, (), (override));
};

/**
 * Mock for TransactionProcessor interface - processes transaction execution
 */
class MockTransactionProcessor : public COP::ACID::TransactionProcessor {
public:
    MOCK_METHOD(void, process, (const COP::ACID::TransactionId &id, COP::ACID::Transaction *tr), (override));
};

/**
 * Mock for TransactionIterator interface - iterates over transactions
 */
class MockTransactionIterator : public COP::ACID::TransactionIterator {
public:
    MOCK_METHOD(bool, next, (COP::ACID::TransactionId *id, COP::ACID::Transaction **tr), (override));
    MOCK_METHOD(bool, get, (COP::ACID::TransactionId *id, COP::ACID::Transaction **tr), (const, override));
    MOCK_METHOD(bool, isValid, (), (const, override));
};

/**
 * Mock for TransactionManager interface - manages transaction lifecycle
 */
class MockTransactionManager : public COP::ACID::TransactionManager {
public:
    MOCK_METHOD(void, attach, (COP::ACID::TransactionObserver *obs), (override));
    MOCK_METHOD(COP::ACID::TransactionObserver*, detach, (), (override));
    MOCK_METHOD(void, addTransaction, (std::unique_ptr<COP::ACID::Transaction> &tr), (override));
    MOCK_METHOD(bool, removeTransaction, (const COP::ACID::TransactionId &id, COP::ACID::Transaction *t), (override));
    MOCK_METHOD(bool, getParentTransactions, (const COP::ACID::TransactionId &id, COP::ACID::TransactionIdsT *parent), (const, override));
    MOCK_METHOD(bool, getRelatedTransactions, (const COP::ACID::TransactionId &id, COP::ACID::TransactionIdsT *related), (const, override));
    MOCK_METHOD(COP::ACID::TransactionIterator*, iterator, (), (override));
};

/**
 * Mock for Scope interface - manages transaction operations
 */
class MockScope : public COP::ACID::Scope {
public:
    MOCK_METHOD(void, addOperation, (std::unique_ptr<COP::ACID::Operation> &op), (override));
    MOCK_METHOD(void, removeLastOperation, (), (override));
    MOCK_METHOD(size_t, startNewStage, (), (override));
    MOCK_METHOD(void, removeStage, (const size_t &id), (override));
};

/**
 * Mock for Transaction interface - represents a complete transaction
 */
class MockTransaction : public COP::ACID::Transaction {
public:
    MOCK_METHOD(const COP::ACID::TransactionId&, transactionId, (), (const, override));
    MOCK_METHOD(void, setTransactionId, (const COP::ACID::TransactionId &id), (override));
    MOCK_METHOD(void, getRelatedObjects, (COP::ACID::ObjectsInTransactionT *obj), (const, override));
    MOCK_METHOD(bool, executeTransaction, (const COP::ACID::Context &cnxt), (override));
};

/**
 * Mock for Operation abstract class - represents a single operation in a transaction
 * Note: Since Operation has a constructor with parameters, we need to provide default values
 */
class MockOperation : public COP::ACID::Operation {
public:
    MockOperation() : COP::ACID::Operation(COP::ACID::INVALID_TROPERATION, IdT()) {}
    MockOperation(COP::ACID::OperationType type, const IdT &id) : COP::ACID::Operation(type, id) {}
    MockOperation(COP::ACID::OperationType type, const IdT &id, const IdT &relId)
        : COP::ACID::Operation(type, id, relId) {}

    MOCK_METHOD(void, execute, (const COP::ACID::Context &cnxt), (override));
    MOCK_METHOD(void, rollback, (const COP::ACID::Context &cnxt), (override));
};

} // namespace test
