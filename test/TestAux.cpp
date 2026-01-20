/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include <sstream>
#include <atomic>

using namespace std;
using namespace test;
using namespace COP;
using namespace COP::ACID;
using namespace COP::Store;

// =============================================================================
// Legacy Assertion Helper
// =============================================================================

void test::check(bool rez, const std::string &error)
{
    if (!rez)
        throw std::logic_error(error);
}

// =============================================================================
// TestTransactionContext Implementation
// =============================================================================

TestTransactionContext::~TestTransactionContext()
{
    clear();
}

void TestTransactionContext::clear()
{
    for (size_t pos = 0; pos < op_.size(); ++pos) {
        delete op_[pos];
    }
    op_.clear();
}

bool TestTransactionContext::isOperationEnqueued(COP::ACID::OperationType type) const
{
    for (size_t pos = 0; pos < op_.size(); ++pos) {
        if (type == op_[pos]->type())
            return true;
    }
    return false;
}

COP::ACID::Operation* TestTransactionContext::getOperation(size_t index) const
{
    if (index < op_.size())
        return op_[index];
    return nullptr;
}

void TestTransactionContext::addOperation(std::unique_ptr<Operation> &op)
{
    op_.push_back(op.release());
}

void TestTransactionContext::removeLastOperation()
{
    if (!op_.empty()) {
        delete op_.back();
        op_.pop_back();
    }
}

size_t TestTransactionContext::startNewStage()
{
    return 0;
}

void TestTransactionContext::removeStage(const size_t &)
{
}

const std::string &TestTransactionContext::invalidReason() const
{
    return reason_;
}

void TestTransactionContext::setInvalidReason(const std::string &reason)
{
    reason_ = reason;
}

bool TestTransactionContext::executeTransaction(const Context &)
{
    return false;
}

// =============================================================================
// Data Factory Functions
// =============================================================================

namespace {
    // Static counters for generating unique IDs
    static std::atomic<int> s_clOrderIdCounter{0};
    static std::atomic<int> s_orderIdCounter{0};
}

COP::SourceIdT test::addInstrument(const std::string &symbol,
                                    const std::string &securityId,
                                    const std::string &securityIdSource)
{
    std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
    instr->symbol_ = symbol;
    instr->securityId_ = securityId;
    instr->securityIdSource_ = securityIdSource;
    return WideDataStorage::instance()->add(instr.release());
}

COP::SourceIdT test::addAccount(const std::string &accountName,
                                 const std::string &firmName,
                                 COP::AccountType type)
{
    std::unique_ptr<AccountEntry> account(new AccountEntry());
    account->type_ = type;
    account->firm_ = firmName;
    account->account_ = accountName;
    account->id_ = IdT();
    return WideDataStorage::instance()->add(account.release());
}

COP::SourceIdT test::addClearing(const std::string &firmName)
{
    std::unique_ptr<ClearingEntry> clearing(new ClearingEntry());
    clearing->firm_ = firmName;
    return WideDataStorage::instance()->add(clearing.release());
}

std::unique_ptr<OrderEntry> test::createCorrectOrder(COP::SourceIdT instrumentId)
{
    SourceIdT srcId, destId, accountId, clearingId, clOrdId, origClOrderID, execList;

    srcId = WideDataStorage::instance()->add(new StringT("CLNT"));
    destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));

    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
    clOrdId = WideDataStorage::instance()->add(clOrd.release());

    accountId = addAccount("ACT", "ACTFirm", PRINCIPAL_ACCOUNTTYPE);
    clearingId = addClearing("CLRFirm");

    std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
    execList = WideDataStorage::instance()->add(execLst.release());

    std::unique_ptr<OrderEntry> ptr(
        new OrderEntry(srcId, destId, clOrdId, origClOrderID, instrumentId, accountId, clearingId, execList));

    ptr->creationTime_ = 100;
    ptr->lastUpdateTime_ = 115;
    ptr->expireTime_ = 175;
    ptr->settlDate_ = 225;
    ptr->price_ = 1.46;

    ptr->status_ = RECEIVEDNEW_ORDSTATUS;
    ptr->side_ = BUY_SIDE;
    ptr->ordType_ = LIMIT_ORDERTYPE;
    ptr->tif_ = DAY_TIF;
    ptr->settlType_ = _3_SETTLTYPE;
    ptr->capacity_ = PRINCIPAL_CAPACITY;
    ptr->currency_ = USD_CURRENCY;
    ptr->orderQty_ = 77;

    return ptr;
}

std::unique_ptr<OrderEntry> test::createCorrectOrder()
{
    SourceIdT instrumentId = addInstrument("AAASymbl", "AAA", "AAASrc");
    return createCorrectOrder(instrumentId);
}

std::unique_ptr<OrderEntry> test::createReplOrder()
{
    SourceIdT srcId, destId, accountId, clearingId, instrument, clOrdId, origClOrderID, execList;

    srcId = WideDataStorage::instance()->add(new StringT("CLNT1"));
    destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));

    std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestReplClOrderId", 17));
    clOrdId = WideDataStorage::instance()->add(clOrd.release());

    accountId = addAccount("ACT1", "ACT1Firm", PRINCIPAL_ACCOUNTTYPE);
    clearingId = addClearing("CLR1Firm");
    instrument = addInstrument("BBBSymbl", "BBB", "BBBSrc");

    std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
    execList = WideDataStorage::instance()->add(execLst.release());

    std::unique_ptr<OrderEntry> ptr(
        new OrderEntry(srcId, destId, clOrdId, origClOrderID, instrument, accountId, clearingId, execList));

    ptr->creationTime_ = 200;
    ptr->lastUpdateTime_ = 215;
    ptr->expireTime_ = 275;
    ptr->settlDate_ = 325;
    ptr->price_ = 2.46;

    ptr->status_ = RECEIVEDNEW_ORDSTATUS;
    ptr->side_ = SELL_SIDE;
    ptr->ordType_ = LIMIT_ORDERTYPE;
    ptr->tif_ = DAY_TIF;
    ptr->settlType_ = _3_SETTLTYPE;
    ptr->capacity_ = PRINCIPAL_CAPACITY;
    ptr->currency_ = USD_CURRENCY;
    ptr->orderQty_ = 150;

    return ptr;
}

std::unique_ptr<TradeExecEntry> test::createTradeExec(
    const OrderEntry &order,
    const COP::IdT &execId,
    QuantityT lastQty,
    PriceT lastPx)
{
    std::unique_ptr<TradeExecEntry> ptr(new TradeExecEntry);

    ptr->execId_ = execId;
    ptr->orderId_ = order.orderId_;

    ptr->type_ = TRADE_EXECTYPE;
    ptr->orderStatus_ = PARTFILL_ORDSTATUS;
    ptr->transactTime_ = 1;

    ptr->lastQty_ = lastQty;
    ptr->lastPx_ = lastPx;
    ptr->currency_ = order.currency_;
    ptr->tradeDate_ = 1;

    OrderStorage::instance()->save(ptr.get());
    return ptr;
}

std::unique_ptr<ExecCorrectExecEntry> test::createCorrectExec(
    const OrderEntry &order,
    const COP::IdT &execId)
{
    std::unique_ptr<ExecCorrectExecEntry> ptr(new ExecCorrectExecEntry);

    ptr->execRefId_ = IdT();
    ptr->execId_ = execId;
    ptr->orderId_ = order.orderId_;

    ptr->type_ = TRADE_EXECTYPE;
    ptr->orderStatus_ = PARTFILL_ORDSTATUS;
    ptr->transactTime_ = 1;

    ptr->cumQty_ = 30;
    ptr->leavesQty_ = 20;
    ptr->lastQty_ = 10;
    ptr->lastPx_ = 10.34;
    ptr->currency_ = order.currency_;
    ptr->tradeDate_ = 1;

    OrderStorage::instance()->save(ptr.get());
    return ptr;
}

void test::assignClOrderId(OrderEntry *order)
{
    int id = ++s_clOrderIdCounter;
    std::ostringstream oss;
    oss << "TestClOrderId_" << id;
    std::string val = oss.str();

    std::unique_ptr<RawDataEntry> clOrd(
        new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
    order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
}

void test::assignOrderId(OrderEntry *order)
{
    int id = ++s_orderIdCounter;
    order->orderId_ = IdT(1, static_cast<u64>(id));
}
