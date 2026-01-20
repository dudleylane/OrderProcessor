/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <deque>
#include <memory>

#include "DataModelDef.h"
#include "TransactionDef.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "IdTGenerator.h"

namespace test {

// =============================================================================
// Legacy Assertion Helper (for migration from legacy tests)
// =============================================================================

/**
 * Legacy assertion function - throws std::logic_error if condition is false.
 * Prefer EXPECT/ASSERT macros from Google Test in new tests.
 */
void check(bool rez, const std::string &error = "");

// =============================================================================
// Transaction Test Doubles
// =============================================================================

/**
 * Test double for COP::ACID::Scope - captures operations for verification
 */
class TestTransactionContext : public COP::ACID::Scope {
public:
    ~TestTransactionContext();

    void addOperation(std::unique_ptr<COP::ACID::Operation> &op) override;
    void removeLastOperation() override;
    size_t startNewStage() override;
    void removeStage(const size_t &id) override;

    // Extended interface for testing
    const std::string &invalidReason() const;
    void setInvalidReason(const std::string &reason);
    bool executeTransaction(const COP::ACID::Context &cnxt);

    void clear();
    bool isOperationEnqueued(COP::ACID::OperationType type) const;
    size_t operationCount() const { return op_.size(); }
    COP::ACID::Operation* getOperation(size_t index) const;

public:
    std::string reason_;
    std::deque<COP::ACID::Operation *> op_;
};

/**
 * No-op implementation of OrderSaver for tests that don't need persistence
 */
class DummyOrderSaver : public COP::OrderSaver {
public:
    DummyOrderSaver() = default;
    ~DummyOrderSaver() = default;

    void save(const COP::OrderEntry&) override {}
};

// =============================================================================
// Data Factory Functions (consolidated from multiple legacy test files)
// =============================================================================

/**
 * Creates and registers an instrument in WideDataStorage
 * @param symbol The instrument symbol
 * @param securityId Optional security ID (defaults to "AAA")
 * @param securityIdSource Optional security ID source (defaults to "AAASrc")
 * @return SourceIdT reference to the stored instrument
 */
COP::SourceIdT addInstrument(const std::string &symbol,
                              const std::string &securityId = "AAA",
                              const std::string &securityIdSource = "AAASrc");

/**
 * Creates and registers an account in WideDataStorage
 * @param accountName The account name
 * @param firmName The firm name
 * @param type Account type (defaults to PRINCIPAL_ACCOUNTTYPE)
 * @return SourceIdT reference to the stored account
 */
COP::SourceIdT addAccount(const std::string &accountName,
                           const std::string &firmName = "ACTFirm",
                           COP::AccountType type = COP::PRINCIPAL_ACCOUNTTYPE);

/**
 * Creates and registers a clearing entry in WideDataStorage
 * @param firmName The clearing firm name
 * @return SourceIdT reference to the stored clearing entry
 */
COP::SourceIdT addClearing(const std::string &firmName);

/**
 * Creates a fully-initialized OrderEntry for testing
 * @param instrumentId SourceIdT of the instrument (must be pre-registered)
 * @return unique_ptr to the created OrderEntry
 */
std::unique_ptr<COP::OrderEntry> createCorrectOrder(COP::SourceIdT instrumentId);

/**
 * Creates a fully-initialized OrderEntry with auto-created instrument
 * @return unique_ptr to the created OrderEntry
 */
std::unique_ptr<COP::OrderEntry> createCorrectOrder();

/**
 * Creates a replacement order based on an existing order
 * @return unique_ptr to the replacement OrderEntry
 */
std::unique_ptr<COP::OrderEntry> createReplOrder();

/**
 * Creates a trade execution entry for testing
 * @param order The order being filled
 * @param execId The execution ID
 * @param lastQty Fill quantity (defaults to 100)
 * @param lastPx Fill price (defaults to 10.25)
 * @return unique_ptr to the created TradeExecEntry
 */
std::unique_ptr<COP::TradeExecEntry> createTradeExec(
    const COP::OrderEntry &order,
    const COP::IdT &execId,
    COP::QuantityT lastQty = 100,
    COP::PriceT lastPx = 10.25);

/**
 * Creates an execution correction entry for testing
 * @param order The order being corrected
 * @param execId The execution ID
 * @return unique_ptr to the created ExecCorrectExecEntry
 */
std::unique_ptr<COP::ExecCorrectExecEntry> createCorrectExec(
    const COP::OrderEntry &order,
    const COP::IdT &execId);

/**
 * Assigns a unique client order ID to the order
 * @param order The order to modify
 */
void assignClOrderId(COP::OrderEntry *order);

/**
 * Assigns a unique order ID to the order
 * @param order The order to modify
 */
void assignOrderId(COP::OrderEntry *order);

// =============================================================================
// State Machine Test Event Helpers
// =============================================================================

/**
 * Creates a state machine test event with test flags enabled
 * @tparam T Event type
 * @param checkRez Expected result of the state check
 * @return Event configured for state machine testing
 */
template<typename T>
T createTestEvent(bool checkRez = true) {
    T evnt;
    evnt.testStateMachine_ = true;
    evnt.testStateMachineCheckResult_ = checkRez;
    return evnt;
}

/**
 * Creates a state machine test event with a parameter
 * @tparam T Event type
 * @tparam P Parameter type
 * @param param Constructor parameter for the event
 * @param checkRez Expected result of the state check
 * @return Event configured for state machine testing
 */
template<typename T, typename P>
T createTestEvent(const P &param, bool checkRez = true) {
    T evnt(param);
    evnt.testStateMachine_ = true;
    evnt.testStateMachineCheckResult_ = checkRez;
    return evnt;
}

} // namespace test