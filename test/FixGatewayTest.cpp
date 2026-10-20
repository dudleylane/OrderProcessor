/**
 Concurrent Order Processor library - FIX Gateway Test Suite

 Tests for enum conversion, inbound FIX message translation, outbound
 ExecutionReport construction, and MultiOutQueues fan-out.
*/

#ifdef BUILD_FIX

// QuickFIX headers MUST come before any <flat_map> include (see FixGateway.h)
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/NewOrderMultileg.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/fix44/OrderCancelReplaceRequest.h>
#include <quickfix/fix44/News.h>
#include <quickfix/fix44/Logon.h>
#include <quickfix/fix44/Heartbeat.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include <quickfix/FieldConvertors.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/TimeRange.h>
#include "FixGateway.h"
#include "FixServer.h"
#include "MultiOutQueues.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"
#include "MockStorage.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace COP;
using namespace COP::App;
using namespace COP::Store;
using namespace COP::Queues;
using namespace test;
using ::testing::_;
using ::testing::Invoke;

namespace
{

const FIX::SessionID TEST_SID("FIX.4.4", "CLIENT_A", "ORDER_PROCESSOR", "");
/// A second client, whose orders TEST_SID must not be able to name (#71)
const FIX::SessionID OTHER_SID("FIX.4.4", "CLIENT_B", "ORDER_PROCESSOR", "");

// =============================================================================
// Enum Conversion Tests
// =============================================================================

class FixEnumTest : public ::testing::Test
{
};

TEST_F(FixEnumTest, SideConversion)
{
    EXPECT_EQ(BUY_SIDE, FixGateway::toSide(FIX::Side_BUY));
    EXPECT_EQ(SELL_SIDE, FixGateway::toSide(FIX::Side_SELL));
    EXPECT_EQ(SELL_SHORT_SIDE, FixGateway::toSide(FIX::Side_SELL_SHORT));
    EXPECT_EQ(INVALID_SIDE, FixGateway::toSide('Z'));
}

TEST_F(FixEnumTest, SideRoundTrip)
{
    EXPECT_EQ(FIX::Side_BUY, FixGateway::fromSide(BUY_SIDE));
    EXPECT_EQ(FIX::Side_SELL, FixGateway::fromSide(SELL_SIDE));
    EXPECT_EQ(FIX::Side_SELL_SHORT, FixGateway::fromSide(SELL_SHORT_SIDE));
    EXPECT_EQ(FIX::Side_CROSS, FixGateway::fromSide(CROSS_SIDE));
}

TEST_F(FixEnumTest, OrdTypeConversion)
{
    EXPECT_EQ(MARKET_ORDERTYPE, FixGateway::toOrdType(FIX::OrdType_MARKET));
    EXPECT_EQ(LIMIT_ORDERTYPE, FixGateway::toOrdType(FIX::OrdType_LIMIT));
    EXPECT_EQ(STOP_ORDERTYPE, FixGateway::toOrdType(FIX::OrdType_STOP));
    EXPECT_EQ(STOPLIMIT_ORDERTYPE, FixGateway::toOrdType(FIX::OrdType_STOP_LIMIT));
    EXPECT_EQ(FXSWAP_ORDERTYPE, FixGateway::toOrdType(FIX::OrdType_FOREX_SWAP));
    EXPECT_EQ(INVALID_ORDERTYPE, FixGateway::toOrdType('Z'));
}

TEST_F(FixEnumTest, TifConversion)
{
    EXPECT_EQ(DAY_TIF, FixGateway::toTif(FIX::TimeInForce_DAY));
    EXPECT_EQ(GTC_TIF, FixGateway::toTif(FIX::TimeInForce_GOOD_TILL_CANCEL));
    EXPECT_EQ(IOC_TIF, FixGateway::toTif(FIX::TimeInForce_IMMEDIATE_OR_CANCEL));
    EXPECT_EQ(FOK_TIF, FixGateway::toTif(FIX::TimeInForce_FILL_OR_KILL));
    EXPECT_EQ(GTD_TIF, FixGateway::toTif(FIX::TimeInForce_GOOD_TILL_DATE));
    EXPECT_EQ(OPG_TIF, FixGateway::toTif(FIX::TimeInForce_AT_THE_OPENING));
    EXPECT_EQ(ATCLOSE_TIF, FixGateway::toTif(FIX::TimeInForce_AT_THE_CLOSE));
    EXPECT_EQ(INVALID_TIF, FixGateway::toTif('Z'));
}

TEST_F(FixEnumTest, CurrencyConversion)
{
    EXPECT_EQ(USD_CURRENCY, FixGateway::toCurrency("USD"));
    EXPECT_EQ(EUR_CURRENCY, FixGateway::toCurrency("EUR"));
    EXPECT_EQ(GBP_CURRENCY, FixGateway::toCurrency("GBP"));
    EXPECT_EQ(JPY_CURRENCY, FixGateway::toCurrency("JPY"));
    EXPECT_EQ(CHF_CURRENCY, FixGateway::toCurrency("CHF"));
    EXPECT_EQ(AUD_CURRENCY, FixGateway::toCurrency("AUD"));
    EXPECT_EQ(CAD_CURRENCY, FixGateway::toCurrency("CAD"));
    EXPECT_EQ(NZD_CURRENCY, FixGateway::toCurrency("NZD"));
    EXPECT_EQ(INVALID_CURRENCY, FixGateway::toCurrency("XXX"));
}

TEST_F(FixEnumTest, OrdStatusConversion)
{
    EXPECT_EQ(FIX::OrdStatus_NEW, FixGateway::fromOrdStatus(NEW_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_PARTIALLY_FILLED, FixGateway::fromOrdStatus(PARTFILL_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_FILLED, FixGateway::fromOrdStatus(FILLED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_CANCELED, FixGateway::fromOrdStatus(CANCELED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_REJECTED, FixGateway::fromOrdStatus(REJECTED_ORDSTATUS));
    // FIX 4.4 has no Replaced status (39=5 is not in its dictionary): a replaced order is closed (#74)
    EXPECT_EQ(FIX::OrdStatus_CANCELED, FixGateway::fromOrdStatus(REPLACED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_EXPIRED, FixGateway::fromOrdStatus(EXPIRED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_SUSPENDED, FixGateway::fromOrdStatus(SUSPENDED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_DONE_FOR_DAY, FixGateway::fromOrdStatus(DFD_ORDSTATUS));
}

TEST_F(FixEnumTest, ExecTypeConversion)
{
    EXPECT_EQ(FIX::ExecType_NEW, FixGateway::fromExecType(NEW_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_TRADE, FixGateway::fromExecType(TRADE_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_CANCELED, FixGateway::fromExecType(CANCEL_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_REJECTED, FixGateway::fromExecType(REJECT_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_REPLACED, FixGateway::fromExecType(REPLACE_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_EXPIRED, FixGateway::fromExecType(EXPIRED_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_PENDING_CANCEL, FixGateway::fromExecType(PEND_CANCEL_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_TRADE_CORRECT, FixGateway::fromExecType(CORRECT_EXECTYPE));
    // A stop's release (#236): not NEW (0), which an execution type the gateway didn't know became
    EXPECT_EQ(FIX::ExecType_TRIGGERED_OR_ACTIVATED_BY_SYSTEM, FixGateway::fromExecType(TRIGGERED_EXECTYPE));
}

// =============================================================================
// FixGateway Inbound Tests
// =============================================================================

class FixGatewayInboundTest : public ProcessorFixture
{
protected:
    void SetUp() override
    {
        ProcessorFixture::SetUp();
        mockInQueues_ = std::make_unique<MockInQueues>();
        // The server's defaults, as main.cpp resolves them (#34); without them every order would be refused
        defaultAccountId_ = test::addAccount("TRADING-1");
        otherAccountId_ = test::addAccount("CLIENT-A");
        defaultClearingId_ = test::addClearing("HOUSE-CLEARING");
        gateway_ = std::make_unique<FixGateway>(mockInQueues_.get(), WideDataStorage::instance(),
                                                OrderStorage::instance(), defaultClearingId_, defaultAccountId_);
    }

    void TearDown() override
    {
        gateway_.reset();
        mockInQueues_.reset();
        ProcessorFixture::TearDown();
    }

    FIX44::NewOrderSingle makeNewOrderSingle(const std::string &clOrdId, const std::string &symbol, char side,
                                             char ordType, double price, double qty, char tif = FIX::TimeInForce_DAY)
    {
        FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
        FIX44::NewOrderSingle msg;
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(side));
        msg.set(FIX::TransactTime(now));
        msg.set(FIX::OrdType(ordType));
        msg.set(FIX::Symbol(symbol));
        msg.set(FIX::OrderQty(qty));
        msg.set(FIX::TimeInForce(tif));
        if (ordType != FIX::OrdType_MARKET)
        {
            msg.set(FIX::Price(price));
        }

        return msg;
    }

    /// An FX swap built as NewOrderMultileg_FxSwap_PushesToQueue builds it; it names no account. Its legs settle on these
    /// LocalMktDates (#198).
    FIX44::NewOrderMultileg makeFxSwap(const std::string &clOrdId, const std::string &nearDate = "20261015",
                                       const std::string &farDate = "20261115")
    {
        FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
        FIX44::NewOrderMultileg msg;
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(FIX::Side_BUY));
        msg.set(FIX::TransactTime(now));
        msg.set(FIX::OrdType(FIX::OrdType_FOREX_SWAP));
        msg.set(FIX::Symbol("aaa"));
        msg.set(FIX::OrderQty(1000000));
        msg.set(FIX::Currency("USD"));

        FIX44::NewOrderMultileg::NoLegs nearLeg;
        nearLeg.set(FIX::LegSymbol("aaa"));
        nearLeg.set(FIX::LegSide(FIX::Side_BUY));
        nearLeg.set(FIX::LegPrice(1.2650));
        nearLeg.set(FIX::LegSettlDate(nearDate));
        msg.addGroup(nearLeg);

        FIX44::NewOrderMultileg::NoLegs farLeg;
        farLeg.set(FIX::LegSymbol("aaa"));
        farLeg.set(FIX::LegSide(FIX::Side_SELL));
        farLeg.set(FIX::LegPrice(1.2680));
        farLeg.set(FIX::LegSettlDate(farDate));
        msg.addGroup(farLeg);

        return msg;
    }

    /// Stores an order from source under this ClOrdID key, as it stands
    OrderEntry *saveOrder(const std::string &source, const std::string &key)
    {
        auto order = createCorrectOrder(instrumentId1_);
        order->source_ = WideDataStorage::instance()->add(new StringT(source));
        order->clOrderId_ = WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, key.c_str(), static_cast<u32>(key.size())));
        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

    /// Stores an order as sid placed it through the gateway: its ClOrdID under the session's key, and the session as
    /// its source (#71)
    OrderEntry *saveOrderOf(const FIX::SessionID &sid, const std::string &clOrdId)
    {
        const std::string source = FixGateway::makeSourceString(sid);
        return saveOrder(source, FixGateway::sessionClOrdId(source, clOrdId));
    }

    FIX44::OrderCancelRequest makeCancelRequest(const std::string &origClOrdId, const std::string &clOrdId)
    {
        FIX44::OrderCancelRequest msg;
        msg.set(FIX::OrigClOrdID(origClOrdId));
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(FIX::Side_BUY));
        msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        msg.set(FIX::Symbol("aaa"));
        return msg;
    }

protected:
    std::unique_ptr<MockInQueues> mockInQueues_;
    std::unique_ptr<FixGateway> gateway_;
    SourceIdT defaultAccountId_;
    SourceIdT otherAccountId_;
    SourceIdT defaultClearingId_;
};

TEST_F(FixGatewayInboundTest, NewOrderSingle_PushesToQueue)
{
    // Expect one OrderEvent push
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &source, const OrderEvent &evt)
            {
                EXPECT_TRUE(source.find("FIX:") != std::string::npos);
                capturedOrder = evt.order_;
            }));

    auto msg = makeNewOrderSingle("ORD001", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(BUY_SIDE, capturedOrder->side_);
    EXPECT_EQ(LIMIT_ORDERTYPE, capturedOrder->ordType_);
    EXPECT_DOUBLE_EQ(10.25, capturedOrder->price_);
    EXPECT_EQ(100u, capturedOrder->orderQty_);
    EXPECT_EQ(100u, capturedOrder->leavesQty_);
    EXPECT_EQ(DAY_TIF, capturedOrder->tif_);
    EXPECT_EQ(RECEIVEDNEW_ORDSTATUS, capturedOrder->status_);

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_MarketOrder)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    auto msg = makeNewOrderSingle("ORD002", "aaa", FIX::Side_SELL, FIX::OrdType_MARKET, 0.0, 50);
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(SELL_SIDE, capturedOrder->side_);
    EXPECT_EQ(MARKET_ORDERTYPE, capturedOrder->ordType_);
    EXPECT_EQ(50u, capturedOrder->orderQty_);

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_UnknownSymbol_NoPush)
{
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);

    auto msg = makeNewOrderSingle("ORD003", "NONEXISTENT", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100);
    gateway_->onMessage(msg, TEST_SID);
}

// =============================================================================
// Server defaults (#34)
// =============================================================================

TEST_F(FixGatewayInboundTest, NewOrderSingle_WithoutAccount_GetsTheServerDefaults)
{
    // The bug this covers: FIX orders carried no clearing firm, and no account when they named none, so the engine
    // rejected every one (#34).
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    auto msg = makeNewOrderSingle("ORD101", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(defaultAccountId_, capturedOrder->account_.getId());
    EXPECT_EQ(defaultClearingId_, capturedOrder->clearing_.getId());
    std::string invalid;
    EXPECT_TRUE(capturedOrder->isValid(&invalid)) << invalid;

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_NamingAnAccount_KeepsIt)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    auto msg = makeNewOrderSingle("ORD102", "aaa", FIX::Side_SELL, FIX::OrdType_LIMIT, 10.50, 50);
    msg.set(FIX::Account("CLIENT-A"));
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(otherAccountId_, capturedOrder->account_.getId());
    EXPECT_EQ(defaultClearingId_, capturedOrder->clearing_.getId());

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_RefusedWhenTheServerHasNoClearingFirm)
{
    // Refused at the gateway, with a BusinessMessageReject to the sender, instead of being rejected by the engine.
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    FixGateway gateway(mockInQueues_.get(), WideDataStorage::instance(), OrderStorage::instance(), SourceIdT(),
                       defaultAccountId_);

    auto msg = makeNewOrderSingle("ORD103", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    msg.set(FIX::Account("CLIENT-A"));
    gateway.onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_WithoutAccount_RefusedWhenTheServerHasNoDefaultAccount)
{
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    FixGateway gateway(mockInQueues_.get(), WideDataStorage::instance(), OrderStorage::instance(), defaultClearingId_,
                       SourceIdT());

    auto msg = makeNewOrderSingle("ORD104", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    gateway.onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_FxSwap_WithoutAccount_GetsTheServerDefaults)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    auto msg = makeFxSwap("SWAP-101");
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(defaultAccountId_, capturedOrder->account_.getId());
    EXPECT_EQ(defaultClearingId_, capturedOrder->clearing_.getId());
    std::string invalid;
    EXPECT_TRUE(capturedOrder->isValid(&invalid)) << invalid;

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_FxSwap_RefusedWhenTheServerHasNoClearingFirm)
{
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    FixGateway gateway(mockInQueues_.get(), WideDataStorage::instance(), OrderStorage::instance(), SourceIdT(),
                       defaultAccountId_);

    auto msg = makeFxSwap("SWAP-102");
    gateway.onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, CancelRequest_PushesToQueue)
{
    // First create an order of this session's, so we can cancel it
    OrderEntry *saved = saveOrderOf(TEST_SID, "ORD-C1");
    ASSERT_NE(nullptr, saved);
    const std::string clOrdStr = "ORD-C1";

    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &source, const OrderCancelEvent &evt)
            {
                EXPECT_TRUE(source.find("FIX:") != std::string::npos);
                EXPECT_EQ(saved->orderId_, evt.id_);
                // the request's own ClOrdID, which the engine hands back on the cancel's answers (#98)
                EXPECT_EQ("CANCEL001", evt.requestClOrdId_);
            }));

    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX44::OrderCancelRequest cancelMsg;
    cancelMsg.set(FIX::OrigClOrdID(clOrdStr));
    cancelMsg.set(FIX::ClOrdID("CANCEL001"));
    cancelMsg.set(FIX::Side(FIX::Side_BUY));
    cancelMsg.set(FIX::TransactTime(now));
    cancelMsg.set(FIX::Symbol("aaa"));

    gateway_->onMessage(cancelMsg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_PushesToQueue)
{
    // The replacement is the original with the request's changes, a fresh id and the request's ClOrdID, naming the
    // original (#74). Before, it kept the original's id and ClOrdID, so the engine refused it as a duplicate ClOrdID.
    OrderEntry *saved = saveOrderOf(TEST_SID, "ORD-R1");
    ASSERT_NE(nullptr, saved);
    saved->status_ = NEW_ORDSTATUS;
    const std::string clOrdStr = "ORD-R1";
    const std::string source = FixGateway::makeSourceString(TEST_SID);

    OrderEntry *capturedReplacement = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &source, const OrderReplaceEvent &evt)
            {
                EXPECT_TRUE(source.find("FIX:") != std::string::npos);
                EXPECT_EQ(saved->orderId_, evt.id_);
                capturedReplacement = evt.replacementOrder_;
            }));

    FIX::UtcTimeStamp now2 = FIX::UtcTimeStamp::now();
    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID(clOrdStr));
    replaceMsg.set(FIX::ClOrdID("REPLACE001"));
    replaceMsg.set(FIX::Side(FIX::Side_BUY));
    replaceMsg.set(FIX::TransactTime(now2));
    replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    replaceMsg.set(FIX::Symbol("aaa"));
    replaceMsg.set(FIX::Price(15.50));
    replaceMsg.set(FIX::OrderQty(200));

    gateway_->onMessage(replaceMsg, TEST_SID);

    ASSERT_NE(nullptr, capturedReplacement);
    EXPECT_FALSE(capturedReplacement->orderId_.isValid());
    // both under the session's key (#71)
    const RawDataEntry &newClOrd = capturedReplacement->clOrderId_.get();
    EXPECT_EQ(FixGateway::sessionClOrdId(source, "REPLACE001"), std::string(newClOrd.data_, newClOrd.length_));
    const RawDataEntry &origClOrd = capturedReplacement->origClOrderId_.get();
    EXPECT_EQ(FixGateway::sessionClOrdId(source, clOrdStr), std::string(origClOrd.data_, origClOrd.length_));
    EXPECT_EQ(RECEIVEDNEW_ORDSTATUS, capturedReplacement->status_);
    EXPECT_DOUBLE_EQ(15.50, capturedReplacement->price_);
    EXPECT_EQ(200u, capturedReplacement->orderQty_);
    EXPECT_EQ(BUY_SIDE, capturedReplacement->side_);
    EXPECT_EQ(instrumentId1_, capturedReplacement->instrument_.getId());

    delete capturedReplacement; // the mock queue owns nothing
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_CarriesTheRequestsSideAndSymbol)
{
    // FIX requires a replace's Side and Symbol to match the order. They are carried over, so that the engine refuses a
    // replace that changes either, rather than keeping the original's without a word (#74).
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-R2"));

    OrderEntry *capturedReplacement = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                capturedReplacement = evt.replacementOrder_;
            }));

    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID("ORD-R2"));
    replaceMsg.set(FIX::ClOrdID("REPLACE002"));
    replaceMsg.set(FIX::Side(FIX::Side_SELL));
    replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    replaceMsg.set(FIX::Symbol("bbb"));
    replaceMsg.set(FIX::Price(15.50));
    replaceMsg.set(FIX::OrderQty(200));
    gateway_->onMessage(replaceMsg, TEST_SID);

    ASSERT_NE(nullptr, capturedReplacement);
    EXPECT_EQ(SELL_SIDE, capturedReplacement->side_);
    EXPECT_EQ(instrumentId2_, capturedReplacement->instrument_.getId());
    delete capturedReplacement;
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_UnknownSymbol_IsNotQueued)
{
    // The gateway refuses it itself (102=99), since the engine's refusal can only describe an instrument that exists;
    // there is no session here to send that to, so this checks that nothing reaches the engine (#74)
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-R4"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);

    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID("ORD-R4"));
    replaceMsg.set(FIX::ClOrdID("REPLACE004"));
    replaceMsg.set(FIX::Side(FIX::Side_BUY));
    replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    replaceMsg.set(FIX::Symbol("NO-SUCH-SYMBOL"));
    replaceMsg.set(FIX::Price(15.50));
    replaceMsg.set(FIX::OrderQty(200));
    gateway_->onMessage(replaceMsg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_UnknownOrigClOrdId_IsNotQueued)
{
    // The gateway answers it itself, with buildUnknownReplaceReject(); before #74 it got no answer at all. There is no
    // session here to send that to, so this checks that nothing reaches the engine.
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);

    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID("NO-SUCH-ORDER"));
    replaceMsg.set(FIX::ClOrdID("REPLACE003"));
    replaceMsg.set(FIX::Side(FIX::Side_BUY));
    replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    replaceMsg.set(FIX::Symbol("aaa"));
    replaceMsg.set(FIX::Price(15.50));
    replaceMsg.set(FIX::OrderQty(200));
    gateway_->onMessage(replaceMsg, TEST_SID);
}

// =============================================================================
// ClOrdIDs belong to the session that sent them (#71)
// =============================================================================

TEST(FixClOrdIdKeyTest, TheClientsClOrdIdComesBackFromItsSessionsKey)
{
    const std::string source = FixGateway::makeSourceString(TEST_SID);
    EXPECT_EQ("FIX:CLIENT_A->ORDER_PROCESSOR|ORD-1", FixGateway::sessionClOrdId(source, "ORD-1"));
    EXPECT_EQ("ORD-1", FixGateway::clientClOrdId(FixGateway::sessionClOrdId(source, "ORD-1"), source));
    // A ClOrdID may itself contain '|'
    EXPECT_EQ("A|B", FixGateway::clientClOrdId(FixGateway::sessionClOrdId(source, "A|B"), source));
    // A key without this session's prefix comes back whole: an order's from before #71, a ClOrdID the server made up,
    // or another session's
    EXPECT_EQ("ORD-1", FixGateway::clientClOrdId("ORD-1", source));
    EXPECT_EQ("WS-1-1", FixGateway::clientClOrdId("WS-1-1", source));
    const std::string others = FixGateway::sessionClOrdId(FixGateway::makeSourceString(OTHER_SID), "ORD-1");
    EXPECT_EQ(others, FixGateway::clientClOrdId(others, source));
    EXPECT_EQ(source, FixGateway::clientClOrdId(source, source));
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_StoresTheClOrdIdUnderItsSession)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));
    gateway_->onMessage(makeNewOrderSingle("ORD-S1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100), TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    const RawDataEntry &clOrd = capturedOrder->clOrderId_.get();
    EXPECT_EQ("FIX:CLIENT_A->ORDER_PROCESSOR|ORD-S1", std::string(clOrd.data_, clOrd.length_));
    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_StoresTheClOrdIdUnderItsSession)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));
    gateway_->onMessage(makeFxSwap("SWAP-S1"), TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    const RawDataEntry &clOrd = capturedOrder->clOrderId_.get();
    EXPECT_EQ("FIX:CLIENT_A->ORDER_PROCESSOR|SWAP-S1", std::string(clOrd.data_, clOrd.length_));
    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, CancelRequest_ForAnotherSessionsOrder_IsNotQueued)
{
    // Before #71 a session could cancel any order whose ClOrdID it knew or guessed. The gateway answers this one as an
    // unknown order itself; there is no session here to send that to, so this checks that nothing reaches the engine.
    ASSERT_NE(nullptr, saveOrderOf(OTHER_SID, "SHARED-1"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>())).Times(0);
    gateway_->onMessage(makeCancelRequest("SHARED-1", "CANCEL-X1"), TEST_SID);
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_ForAnotherSessionsOrder_IsNotQueued)
{
    ASSERT_NE(nullptr, saveOrderOf(OTHER_SID, "SHARED-2"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);

    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID("SHARED-2"));
    replaceMsg.set(FIX::ClOrdID("REPLACE-X2"));
    replaceMsg.set(FIX::Side(FIX::Side_BUY));
    replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    replaceMsg.set(FIX::Symbol("aaa"));
    replaceMsg.set(FIX::Price(15.50));
    replaceMsg.set(FIX::OrderQty(200));
    gateway_->onMessage(replaceMsg, TEST_SID);
}

// --- Who may log on, and what each counterparty may trade (#147) ---

/// The acceptor's own view of its session with CLIENT_A, as QuickFIX passes it: TargetCompID is the counterparty's
const FIX::SessionID ACCEPTOR_SID("FIX.4.4", "ORDER_PROCESSOR", "CLIENT_A");

FIX44::Logon makeLogon(const std::string &username, const std::string &password)
{
    FIX44::Logon logon(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER), FIX::HeartBtInt(30));
    if (!username.empty())
    {
        logon.set(FIX::Username(username));
    }
    if (!password.empty())
    {
        logon.set(FIX::Password(password));
    }
    return logon;
}

/// CLIENT_A may trade CLIENT-A only, with the password "secret" (few iterations, to stay fast under the sanitizers)
FixCredentials clientACredentials(const std::string &accounts = "CLIENT-A")
{
    return FixCredentials::parse("CLIENT_A client_a " + FixCredentials::hashPassword("secret", 1000) + " " + accounts,
                                 "creds");
}

TEST_F(FixGatewayInboundTest, ALogonNeedsItsCounterpartysUsernameAndPassword)
{
    const FixCredentials credentials = clientACredentials();
    gateway_->setCredentials(&credentials);
    EXPECT_NO_THROW(gateway_->fromAdmin(makeLogon("client_a", "secret"), ACCEPTOR_SID));
    EXPECT_THROW(gateway_->fromAdmin(makeLogon("client_a", "Secret"), ACCEPTOR_SID), FIX::RejectLogon);
    EXPECT_THROW(gateway_->fromAdmin(makeLogon("client_b", "secret"), ACCEPTOR_SID), FIX::RejectLogon);
    EXPECT_THROW(gateway_->fromAdmin(makeLogon("client_a", ""), ACCEPTOR_SID), FIX::RejectLogon);
    EXPECT_THROW(gateway_->fromAdmin(makeLogon("", ""), ACCEPTOR_SID), FIX::RejectLogon);
    // A session the credentials don't name, with another counterparty's username and password
    EXPECT_THROW(
        gateway_->fromAdmin(makeLogon("client_a", "secret"), FIX::SessionID("FIX.4.4", "ORDER_PROCESSOR", "CLIENT_Z")),
        FIX::RejectLogon);
    // Only a Logon is checked: the session's other admin messages carry no credentials
    EXPECT_NO_THROW(gateway_->fromAdmin(FIX44::Heartbeat(), ACCEPTOR_SID));
    // The client learns only that it was refused, whatever the reason
    try
    {
        gateway_->fromAdmin(makeLogon("client_b", "secret"), ACCEPTOR_SID);
    }
    catch (const FIX::RejectLogon &e)
    {
        EXPECT_EQ("Logon refused: wrong username or password", e.detail);
    }
}

TEST_F(FixGatewayInboundTest, WithoutCredentialsEveryLogonIsAccepted)
{
    // As tests run the gateway; the server refuses --fix-cfg without --fix-credentials
    EXPECT_NO_THROW(gateway_->fromAdmin(makeLogon("", ""), ACCEPTOR_SID));
}

TEST_F(FixGatewayInboundTest, ASessionTradesOnlyItsCounterpartysAccounts)
{
    const FixCredentials credentials = clientACredentials();
    gateway_->setCredentials(&credentials);
    std::vector<SourceIdT> queued;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                queued.push_back(evt.order_->account_.getId());
                delete evt.order_;
            }));

    // Its own account
    auto own = makeNewOrderSingle("ACCT-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    own.set(FIX::Account("CLIENT-A"));
    gateway_->onMessage(own, ACCEPTOR_SID);
    // No account: its first, not the server's default
    gateway_->onMessage(makeNewOrderSingle("ACCT-2", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100),
                        ACCEPTOR_SID);
    // The server's default account, which isn't its: refused, before the engine sees it
    auto other = makeNewOrderSingle("ACCT-3", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    other.set(FIX::Account("TRADING-1"));
    gateway_->onMessage(other, ACCEPTOR_SID);
    // An FX swap, with the same rules
    auto swapOther = makeFxSwap("ACCT-4");
    swapOther.set(FIX::Account("TRADING-1"));
    gateway_->onMessage(swapOther, ACCEPTOR_SID);
    gateway_->onMessage(makeFxSwap("ACCT-5"), ACCEPTOR_SID);

    EXPECT_EQ((std::vector<SourceIdT>{ otherAccountId_, otherAccountId_, otherAccountId_ }), queued);
}

TEST_F(FixGatewayInboundTest, CredentialsGivingAnAccountTheServerDoesntHaveAreRefused)
{
    const FixCredentials credentials = clientACredentials("CLIENT-A,NO-SUCH-ACCOUNT");
    EXPECT_THROW(gateway_->setCredentials(&credentials), std::runtime_error);
    // And it checks nothing then: the server doesn't start
    EXPECT_NO_THROW(gateway_->fromAdmin(makeLogon("", ""), ACCEPTOR_SID));
}

TEST_F(FixGatewayInboundTest, CancelRequest_ForItsOwnOrderStoredUnderTheClOrdIdAlone_IsQueued)
{
    // An order stored before #71 has its ClOrdID alone; the session that placed it can still cancel it
    OrderEntry *saved = saveOrder(FixGateway::makeSourceString(TEST_SID), "OLD-1");
    ASSERT_NE(nullptr, saved);
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderCancelEvent &evt)
            {
                EXPECT_EQ(saved->orderId_, evt.id_);
            }));
    gateway_->onMessage(makeCancelRequest("OLD-1", "CANCEL-O1"), TEST_SID);
}

TEST_F(FixGatewayInboundTest, CancelRequest_ForAnotherSourcesOrderStoredUnderTheClOrdIdAlone_IsNotQueued)
{
    // A WebSocket order's ClOrdID, or another session's from before #71: not this session's to name
    ASSERT_NE(nullptr, saveOrder("WebSocket", "WS-1-1"));
    ASSERT_NE(nullptr, saveOrder(FixGateway::makeSourceString(OTHER_SID), "OLD-2"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>())).Times(0);
    gateway_->onMessage(makeCancelRequest("WS-1-1", "CANCEL-W1"), TEST_SID);
    gateway_->onMessage(makeCancelRequest("OLD-2", "CANCEL-O2"), TEST_SID);
}

// =============================================================================
// NewOrderMultileg — FX Swap via standard FIX
// =============================================================================

TEST_F(FixGatewayInboundTest, NewOrderMultileg_FxSwap_PushesToQueue)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &source, const OrderEvent &evt)
            {
                EXPECT_TRUE(source.find("FIX:") != std::string::npos);
                capturedOrder = evt.order_;
            }));

    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX44::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("SWAP-001"));
    msg.set(FIX::Side(FIX::Side_BUY)); // near-leg side
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(FIX::OrdType_FOREX_SWAP));
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(1000000));
    msg.set(FIX::Currency("USD"));

    // Near leg: BUY at 1.2650, settl T+2
    FIX44::NewOrderMultileg::NoLegs nearLeg;
    nearLeg.set(FIX::LegSymbol("aaa"));
    nearLeg.set(FIX::LegSide(FIX::Side_BUY));
    nearLeg.set(FIX::LegPrice(1.2650));
    nearLeg.set(FIX::LegSettlDate("20261015"));
    msg.addGroup(nearLeg);

    // Far leg: SELL at 1.2680, settl T+92
    FIX44::NewOrderMultileg::NoLegs farLeg;
    farLeg.set(FIX::LegSymbol("aaa"));
    farLeg.set(FIX::LegSide(FIX::Side_SELL));
    farLeg.set(FIX::LegPrice(1.2680));
    farLeg.set(FIX::LegSettlDate("20261115"));
    msg.addGroup(farLeg);

    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(FXSWAP_ORDERTYPE, capturedOrder->ordType_);
    EXPECT_EQ(BUY_SIDE, capturedOrder->side_);
    EXPECT_DOUBLE_EQ(1.2650, capturedOrder->price_);           // near (matches root side)
    EXPECT_DOUBLE_EQ(1.2680, capturedOrder->farPrice_);        // far (opposite side)
    EXPECT_EQ(1792022400000ULL, capturedOrder->settlDate_);    // 2026-10-15, midnight UTC (#198)
    EXPECT_EQ(1794700800000ULL, capturedOrder->farSettlDate_); // 2026-11-15
    EXPECT_EQ(1000000u, capturedOrder->orderQty_);

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_NonSwapOrdType_Rejected)
{
    // NewOrderMultileg with non-FXSwap OrdType should be rejected (no push)
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);

    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX44::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("MLEG-BAD"));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(FIX::OrdType_LIMIT)); // not FX Swap
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(100));

    gateway_->onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_TooFewLegs_Rejected)
{
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);

    FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
    FIX44::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("MLEG-1"));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(FIX::OrdType_FOREX_SWAP));
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(100));

    // Only one leg — should be rejected
    FIX44::NewOrderMultileg::NoLegs oneLeg;
    oneLeg.set(FIX::LegSymbol("aaa"));
    oneLeg.set(FIX::LegSide(FIX::Side_BUY));
    oneLeg.set(FIX::LegPrice(1.2650));
    msg.addGroup(oneLeg);

    gateway_->onMessage(msg, TEST_SID);
}

// =============================================================================
// Quantities and prices the engine can't take are refused (#145)
// =============================================================================

/// OrderQty values the engine's QuantityT can't hold. Before #145 the gateway narrowed them without a check: -5 became
/// an order for 4294967291, and 4294967296 and 0.5 orders for 0.
const double BAD_QUANTITIES[] = { -5.0, 4294967296.0, 0.5, 0.0 };
/// A 310-digit price. QuickFIX's format check for a PRICE field accepts it, and converts it to infinity.
const std::string HUGE_PRICE = "1" + std::string(309, '0');
/// Prices no order trades at: beyond double's range either way, zero, and negative
const std::string BAD_PRICES[] = { HUGE_PRICE, "-" + HUGE_PRICE, "0", "-1.25" };

TEST_F(FixGatewayInboundTest, NewOrderSingle_OrderQtyTheEngineCannotHold_IsNotQueued)
{
    // Refused to the sender instead; there is no session here to send that to, so this checks that nothing reaches
    // the engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    int n = 0;
    for (double qty : BAD_QUANTITIES)
    {
        auto msg =
            makeNewOrderSingle("QTY-" + std::to_string(++n), "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, qty);
        gateway_->onMessage(msg, TEST_SID);
    }
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_LargestOrderQtyTheEngineHolds_IsQueued)
{
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    auto msg = makeNewOrderSingle("QTY-MAX", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 4294967295.0);
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(4294967295u, capturedOrder->orderQty_);
    EXPECT_EQ(4294967295u, capturedOrder->leavesQty_);
    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_LimitPriceNoOrderTradesAt_IsNotQueued)
{
    // Before #145 a 310-digit price reached the engine as inf, and a limit price of 0 or less as it was
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    int n = 0;
    for (const std::string &price : BAD_PRICES)
    {
        auto msg =
            makeNewOrderSingle("PX-" + std::to_string(++n), "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
        msg.setField(FIX::FIELD::Price, price);
        gateway_->onMessage(msg, TEST_SID);
    }
}

/// A saver that outlives the test's teardown, where the singletons go
class FixGatewayStringsTest : public FixGatewayInboundTest
{
protected:
    testing::NiceMock<test::MockDataSaver> saver_;
};

TEST_F(FixGatewayStringsTest, NewOrdersAndAReplaceWriteNoStringRecords)
{
    // Each FIX order's ClOrdID, source and destination, and a replace's new ClOrdID, were written to storage as they came
    // in, a commit each; the order's record carries them now (#192)
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-STR"));
    EXPECT_CALL(saver_, save(testing::An<const RawDataEntry &>())).Times(0);
    EXPECT_CALL(saver_, save(testing::_, testing::An<const StringT &>())).Times(0);
    WideDataStorage::instance()->bindStorage(&saver_);

    std::vector<OrderEntry *> orders;
    OrderEntry *replacement = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .Times(2)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                orders.push_back(evt.order_);
            }));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replacement = evt.replacementOrder_;
            }));

    gateway_->onMessage(makeNewOrderSingle("STR-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100), TEST_SID);
    gateway_->onMessage(makeFxSwap("STR-SWAP"), TEST_SID);
    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID("ORD-STR"));
    replaceMsg.set(FIX::ClOrdID("STR-REPLACE"));
    replaceMsg.set(FIX::Side(FIX::Side_BUY));
    replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    replaceMsg.set(FIX::Symbol("aaa"));
    replaceMsg.set(FIX::Price(15.50));
    replaceMsg.set(FIX::OrderQty(200));
    gateway_->onMessage(replaceMsg, TEST_SID);

    EXPECT_EQ(2u, orders.size());
    EXPECT_NE(nullptr, replacement);
    for (OrderEntry *order : orders)
    {
        delete order;
    }
    delete replacement;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_StopOrdersCarryTheirStopPx)
{
    // A stop order's StopPx (99) is its stop price (#223), for a STOP and for a STOP_LIMIT, whose Price (44) is its limit
    std::vector<OrderEntry *> captured;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .Times(2)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                captured.push_back(evt.order_);
            }));
    auto stop = makeNewOrderSingle("STP-1", "aaa", FIX::Side_SELL, FIX::OrdType_STOP, 0.0, 100);
    stop.removeField(FIX::FIELD::Price);
    stop.set(FIX::StopPx(9.5));
    gateway_->onMessage(stop, TEST_SID);
    auto stopLimit = makeNewOrderSingle("STL-1", "aaa", FIX::Side_BUY, FIX::OrdType_STOP_LIMIT, 10.5, 100);
    stopLimit.set(FIX::StopPx(10.0));
    gateway_->onMessage(stopLimit, TEST_SID);

    ASSERT_EQ(2u, captured.size());
    EXPECT_EQ(STOP_ORDERTYPE, captured[0]->ordType_);
    EXPECT_DOUBLE_EQ(9.5, captured[0]->stopPx_);
    EXPECT_EQ(STOPLIMIT_ORDERTYPE, captured[1]->ordType_);
    EXPECT_DOUBLE_EQ(10.0, captured[1]->stopPx_);
    EXPECT_DOUBLE_EQ(10.5, captured[1]->price_);
    for (OrderEntry *order : captured)
    {
        delete order;
    }
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_StopOrderWithoutAStopPxNoOrderTradesAt_IsNotQueued)
{
    // The gateway never read StopPx before #223's pick, so a FIX stop order reached the engine without a stop price, which
    // it now refuses. The gateway refuses it first, to its sender, as it does a Price (44) no order trades at (#145).
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    auto none = makeNewOrderSingle("STP-NONE", "aaa", FIX::Side_SELL, FIX::OrdType_STOP, 0.0, 100);
    none.removeField(FIX::FIELD::Price);
    gateway_->onMessage(none, TEST_SID);
    int n = 0;
    for (const std::string &stopPx : BAD_PRICES)
    {
        auto msg =
            makeNewOrderSingle("STP-" + std::to_string(++n), "aaa", FIX::Side_SELL, FIX::OrdType_STOP_LIMIT, 9.0, 100);
        msg.setField(FIX::FIELD::StopPx, stopPx);
        gateway_->onMessage(msg, TEST_SID);
    }
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_MarketOrderWithPriceZero_IsQueued)
{
    // A market order trades at no price of its own, so a Price (44) of 0 on one is still taken
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    auto msg = makeNewOrderSingle("MKT-PX0", "aaa", FIX::Side_SELL, FIX::OrdType_MARKET, 0.0, 50);
    msg.set(FIX::Price(0.0));
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(MARKET_ORDERTYPE, capturedOrder->ordType_);
    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_MarketOrderWithAPriceBeyondDoubleRange_IsNotQueued)
{
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    auto msg = makeNewOrderSingle("MKT-HUGE", "aaa", FIX::Side_SELL, FIX::OrdType_MARKET, 0.0, 50);
    msg.setField(FIX::FIELD::Price, HUGE_PRICE);
    gateway_->onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_FxSwapFarPriceNoOrderTradesAt_IsNotQueued)
{
    // The far price (6500) went through std::stod, which also takes "inf" and "nan"
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    int n = 0;
    for (const char *farPrice : { "inf", "nan", "0", "-1.0872" })
    {
        auto msg = makeNewOrderSingle("SWAP-FAR-" + std::to_string(++n), "aaa", FIX::Side_BUY, FIX::OrdType_FOREX_SWAP,
                                      1.0850, 1000000);
        msg.setField(6500, farPrice);
        msg.setField(6501, "20261115");
        msg.setField(FIX::FIELD::SettlDate, "20261015");
        gateway_->onMessage(msg, TEST_SID);
    }
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_OrderQtyTheEngineCannotHold_IsNotQueued)
{
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    int n = 0;
    for (double qty : BAD_QUANTITIES)
    {
        FIX44::NewOrderMultileg msg = makeFxSwap("SWAP-QTY-" + std::to_string(++n));
        msg.set(FIX::OrderQty(qty));
        gateway_->onMessage(msg, TEST_SID);
    }
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_LegPriceNoOrderTradesAt_IsNotQueued)
{
    // Either leg's LegPrice (566)
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    int n = 0;
    for (unsigned leg = 1; leg <= 2; ++leg)
    {
        for (const std::string &price : BAD_PRICES)
        {
            FIX44::NewOrderMultileg msg = makeFxSwap("SWAP-PX-" + std::to_string(++n));
            FIX44::NewOrderMultileg::NoLegs group;
            msg.getGroup(leg, group);
            group.setField(FIX::FIELD::LegPrice, price);
            msg.replaceGroup(leg, group);
            gateway_->onMessage(msg, TEST_SID);
        }
    }
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_OrderQtyTheEngineCannotHold_IsNotQueued)
{
    // Refused to the sender with an OrderCancelReject, as an unknown symbol is
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-R5"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    int n = 0;
    for (double qty : BAD_QUANTITIES)
    {
        FIX44::OrderCancelReplaceRequest replaceMsg;
        replaceMsg.set(FIX::OrigClOrdID("ORD-R5"));
        replaceMsg.set(FIX::ClOrdID("REPLACE-QTY-" + std::to_string(++n)));
        replaceMsg.set(FIX::Side(FIX::Side_BUY));
        replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
        replaceMsg.set(FIX::Symbol("aaa"));
        replaceMsg.set(FIX::Price(15.50));
        replaceMsg.set(FIX::OrderQty(qty));
        gateway_->onMessage(replaceMsg, TEST_SID);
    }
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_PriceNoOrderTradesAt_IsNotQueued)
{
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-R6"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    int n = 0;
    for (const std::string &price : BAD_PRICES)
    {
        FIX44::OrderCancelReplaceRequest replaceMsg;
        replaceMsg.set(FIX::OrigClOrdID("ORD-R6"));
        replaceMsg.set(FIX::ClOrdID("REPLACE-PX-" + std::to_string(++n)));
        replaceMsg.set(FIX::Side(FIX::Side_BUY));
        replaceMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        replaceMsg.set(FIX::OrdType(FIX::OrdType_LIMIT));
        replaceMsg.set(FIX::Symbol("aaa"));
        replaceMsg.setField(FIX::FIELD::Price, price);
        replaceMsg.set(FIX::OrderQty(200));
        gateway_->onMessage(replaceMsg, TEST_SID);
    }
}

/// A replace request for this session's order origClOrdId, of a buy of "aaa", as a stop limit
FIX44::OrderCancelReplaceRequest makeStopReplace(const std::string &origClOrdId, const std::string &clOrdId)
{
    FIX44::OrderCancelReplaceRequest msg;
    msg.set(FIX::OrigClOrdID(origClOrdId));
    msg.set(FIX::ClOrdID(clOrdId));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    msg.set(FIX::OrdType(FIX::OrdType_STOP_LIMIT));
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(100));
    return msg;
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_StopPxMovesAStopOrdersStop)
{
    // A replace's StopPx (99) moves a stop order's stop (#238), and one without it keeps the stop. The gateway didn't
    // read it, so the replacement kept the original's stop, and the client was told nothing.
    OrderEntry *saved = saveOrderOf(TEST_SID, "ORD-STP");
    ASSERT_NE(nullptr, saved);
    saved->ordType_ = STOPLIMIT_ORDERTYPE;
    saved->price_ = 10.5;
    saved->stopPx_ = 10.0;
    std::vector<OrderEntry *> captured;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .Times(2)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                captured.push_back(evt.replacementOrder_);
            }));

    auto moved = makeStopReplace("ORD-STP", "REPLACE-STP-1");
    moved.set(FIX::Price(10.0));
    moved.set(FIX::StopPx(9.5));
    gateway_->onMessage(moved, TEST_SID);
    auto kept = makeStopReplace("ORD-STP", "REPLACE-STP-2");
    kept.set(FIX::Price(10.25));
    gateway_->onMessage(kept, TEST_SID);

    ASSERT_EQ(2u, captured.size());
    EXPECT_EQ(STOPLIMIT_ORDERTYPE, captured[0]->ordType_);
    EXPECT_DOUBLE_EQ(9.5, captured[0]->stopPx_);
    EXPECT_DOUBLE_EQ(10.0, captured[0]->price_);
    EXPECT_DOUBLE_EQ(10.0, captured[1]->stopPx_);
    EXPECT_DOUBLE_EQ(10.25, captured[1]->price_);
    for (OrderEntry *order : captured)
    {
        delete order; // the mock queue owns nothing
    }
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_StopPxForAnOrderThatIsntAStopOrder_IsNotQueued)
{
    // A replace keeps the original's type, and a limit order has no stop to move (#238). Refused by the gateway
    // (102=99), as WebSocket's replace_order refuses a stopPx for one.
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-LMT"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    auto msg = makeStopReplace("ORD-LMT", "REPLACE-LMT-1");
    msg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.set(FIX::Price(15.50));
    msg.set(FIX::StopPx(15.0));
    gateway_->onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_StopPxNoOrderTradesAt_IsNotQueued)
{
    // Checked as a new stop order's StopPx is (#223), and refused to the sender with an OrderCancelReject (#238)
    OrderEntry *saved = saveOrderOf(TEST_SID, "ORD-STP-PX");
    ASSERT_NE(nullptr, saved);
    saved->ordType_ = STOPLIMIT_ORDERTYPE;
    saved->stopPx_ = 10.0;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    int n = 0;
    for (const std::string &stopPx : BAD_PRICES)
    {
        auto msg = makeStopReplace("ORD-STP-PX", "REPLACE-STP-PX-" + std::to_string(++n));
        msg.setField(FIX::FIELD::StopPx, stopPx);
        gateway_->onMessage(msg, TEST_SID);
    }
}

/// An ExpireTime (126) an hour ahead, to the second, in milliseconds since the epoch
DateTimeT anHourAhead()
{
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    return static_cast<DateTimeT>(now.count() + 60 * 60) * 1000;
}

/// A replace request for this session's limit order origClOrdId, a buy of "aaa"
FIX44::OrderCancelReplaceRequest makeLimitReplace(const std::string &origClOrdId, const std::string &clOrdId)
{
    FIX44::OrderCancelReplaceRequest msg;
    msg.set(FIX::OrigClOrdID(origClOrdId));
    msg.set(FIX::ClOrdID(clOrdId));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    msg.set(FIX::OrdType(FIX::OrdType_LIMIT));
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::Price(15.50));
    msg.set(FIX::OrderQty(200));
    return msg;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_AGtdOrderCarriesItsExpireTime)
{
    // TimeInForce GTD (59=6) expires at ExpireTime (126) (#240). The gateway never read it, and the engine refuses a GTD
    // order without an expiry, so every FIX GTD order was rejected.
    OrderEntry *captured = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                captured = evt.order_;
            }));
    const DateTimeT expiry = anHourAhead();
    auto msg = makeNewOrderSingle("GTD-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100,
                                  FIX::TimeInForce_GOOD_TILL_DATE);
    msg.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(expiry)));
    gateway_->onMessage(msg, TEST_SID);
    ASSERT_NE(nullptr, captured);
    EXPECT_EQ(GTD_TIF, captured->tif_);
    EXPECT_EQ(expiry, captured->expireTime_);
    delete captured;
}

TEST_F(FixGatewayInboundTest, NewOrderSingle_AnExpireTimeTheRulesRefuse_IsNotQueued)
{
    // As on WebSocket (#240): a GTD order without ExpireTime, with ExpireDate (432) alone, or with one that has passed, and
    // an ExpireTime on an order that isn't GTD. Each is refused to its sender before anything is stored.
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    const char gtd = FIX::TimeInForce_GOOD_TILL_DATE;
    gateway_->onMessage(makeNewOrderSingle("GTD-NONE", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100, gtd),
                        TEST_SID);
    auto date = makeNewOrderSingle("GTD-DATE", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100, gtd);
    date.set(FIX::ExpireDate("20991231"));
    gateway_->onMessage(date, TEST_SID);
    auto passed = makeNewOrderSingle("GTD-PAST", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100, gtd);
    passed.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(anHourAhead() - 2ULL * 60 * 60 * 1000)));
    gateway_->onMessage(passed, TEST_SID);
    auto day = makeNewOrderSingle("DAY-EXPIRY", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    day.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(anHourAhead())));
    gateway_->onMessage(day, TEST_SID);
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_AGtdSwapCarriesItsExpireTime)
{
    OrderEntry *captured = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                captured = evt.order_;
            }));
    const DateTimeT expiry = anHourAhead();
    auto msg = makeFxSwap("SWAP-GTD");
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_DATE));
    msg.setField(FIX::ExpireTime(FixGateway::toUtcTimeStamp(expiry)));
    gateway_->onMessage(msg, TEST_SID);
    ASSERT_NE(nullptr, captured);
    EXPECT_EQ(GTD_TIF, captured->tif_);
    EXPECT_EQ(expiry, captured->expireTime_);
    delete captured;
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_ExpireTimeFollowsReplaceOrdersRules)
{
    // A GTD replacement expires at the request's ExpireTime (126), or else at the original's, and a replace to another
    // time in force drops it (#240), as replace_order does
    OrderEntry *saved = saveOrderOf(TEST_SID, "ORD-GTD");
    ASSERT_NE(nullptr, saved);
    const DateTimeT kept = anHourAhead();
    saved->tif_ = GTD_TIF;
    saved->expireTime_ = kept;
    std::vector<OrderEntry *> captured;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .Times(3)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                captured.push_back(evt.replacementOrder_);
            }));

    gateway_->onMessage(makeLimitReplace("ORD-GTD", "RPL-KEEP"), TEST_SID);
    auto moved = makeLimitReplace("ORD-GTD", "RPL-MOVE");
    moved.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(kept + 60ULL * 60 * 1000)));
    gateway_->onMessage(moved, TEST_SID);
    auto gtc = makeLimitReplace("ORD-GTD", "RPL-GTC");
    gtc.set(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    gateway_->onMessage(gtc, TEST_SID);

    ASSERT_EQ(3u, captured.size());
    EXPECT_EQ(kept, captured[0]->expireTime_) << "not the original's";
    EXPECT_EQ(kept + 60ULL * 60 * 1000, captured[1]->expireTime_);
    EXPECT_EQ(GTC_TIF, captured[2]->tif_);
    EXPECT_EQ(0u, captured[2]->expireTime_) << "a GTC replacement kept an expiry";
    for (OrderEntry *order : captured)
    {
        delete order; // the mock queue owns nothing
    }
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_AnExpireTimeTheRulesRefuse_IsNotQueued)
{
    // Refused to the sender with an OrderCancelReject (102=99): a replace of a DAY order to GTD without ExpireTime, one
    // whose ExpireTime has passed, and an ExpireTime for an order that stays DAY (#240)
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-DAY"));
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    auto toGtd = makeLimitReplace("ORD-DAY", "RPL-1");
    toGtd.set(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_DATE));
    gateway_->onMessage(toGtd, TEST_SID);
    auto passed = makeLimitReplace("ORD-DAY", "RPL-2");
    passed.set(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_DATE));
    passed.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(anHourAhead() - 2ULL * 60 * 60 * 1000)));
    gateway_->onMessage(passed, TEST_SID);
    auto day = makeLimitReplace("ORD-DAY", "RPL-3");
    day.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(anHourAhead())));
    gateway_->onMessage(day, TEST_SID);
}

TEST(FixSessionTimeTest, ASessionsDayEndsAt1700NewYork)
{
    // config/fix.cfg's sessions roll where the engine's business day ends, 17:00 New York (#240, ruled 2026-10-16), and
    // rolled at 00:00 UTC (#160). QuickFIX reads StartTime and EndTime in local time (UseLocalTime), which the server keeps
    // New York's; the start must come after the end, as a session whose times are equal rolls at local midnight. The
    // range is the one QuickFIX's SessionFactory makes from them.
    // UTC first, whatever the machine's zone is (this one's is New York's): the server's function must set the zone
    ::setenv("TZ", "UTC", 1);
    ::tzset();
    App::useBusinessDayLocalTime();
    const std::filesystem::path cfg = test::sourceRoot() / "config" / "fix.cfg";
    ASSERT_TRUE(std::filesystem::exists(cfg)) << cfg << " not found";
    const FIX::SessionSettings settings(cfg.string());
    const FIX::Dictionary &defaults = settings.get();
    ASSERT_TRUE(defaults.has("UseLocalTime") && defaults.getBool("UseLocalTime"));
    const FIX::UtcTimeOnly start = FIX::UtcTimeOnlyConvertor::convert(defaults.getString("StartTime"));
    const FIX::UtcTimeOnly end = FIX::UtcTimeOnlyConvertor::convert(defaults.getString("EndTime"));
    FIX::TimeRange range(FIX::LocalTimeOnly(start.getHour(), start.getMinute(), start.getSecond()),
                         FIX::LocalTimeOnly(end.getHour(), end.getMinute(), end.getSecond()));
    // Asked as QuickFIX's Session asks it, the later time first: now, then when the session began. Daylight time: 17:00
    // EDT is 21:00 UTC, and New York's midnight is no boundary. (QuickFIX's arithmetic puts the session's last second,
    // 16:59:59, in the next one, so the checks keep off it.)
    EXPECT_FALSE(
        range.isInSameRange(FIX::UtcTimeStamp(21, 0, 1, 15, 10, 2026), FIX::UtcTimeStamp(20, 59, 58, 15, 10, 2026)));
    EXPECT_TRUE(
        range.isInSameRange(FIX::UtcTimeStamp(20, 59, 0, 16, 10, 2026), FIX::UtcTimeStamp(21, 0, 1, 15, 10, 2026)));
    EXPECT_TRUE(
        range.isInSameRange(FIX::UtcTimeStamp(4, 1, 0, 16, 10, 2026), FIX::UtcTimeStamp(3, 59, 0, 16, 10, 2026)));
    // Standard time: 17:00 EST is 22:00 UTC, and 00:00 UTC is no boundary
    EXPECT_FALSE(
        range.isInSameRange(FIX::UtcTimeStamp(22, 0, 1, 15, 12, 2026), FIX::UtcTimeStamp(21, 59, 58, 15, 12, 2026)));
    EXPECT_TRUE(
        range.isInSameRange(FIX::UtcTimeStamp(0, 30, 0, 16, 12, 2026), FIX::UtcTimeStamp(23, 30, 0, 15, 12, 2026)));
}

TEST(FixQuantityTest, OrderQtyIsAWholeNumberTheEngineCanHold)
{
    EXPECT_EQ(std::optional<QuantityT>(1u), FixGateway::toQuantity(1.0));
    EXPECT_EQ(std::optional<QuantityT>(100u), FixGateway::toQuantity(100.0));
    EXPECT_EQ(std::optional<QuantityT>(4294967295u), FixGateway::toQuantity(4294967295.0));
    for (double qty : BAD_QUANTITIES)
    {
        EXPECT_FALSE(FixGateway::toQuantity(qty).has_value()) << qty;
    }
    const double inf = std::numeric_limits<double>::infinity();
    for (double qty : { -0.0, 4294967295.5, 1e300, inf, -inf, std::numeric_limits<double>::quiet_NaN() })
    {
        EXPECT_FALSE(FixGateway::toQuantity(qty).has_value()) << qty;
    }
}

TEST(FixPriceTest, APriceIsFiniteAndAboveZeroUnlessTheOrderIsAMarketOrder)
{
    // As QuickFIX hands a 310-digit PRICE field over
    const double huge = FIX::DoubleConvertor::convert(HUGE_PRICE);
    EXPECT_TRUE(std::isinf(huge));

    EXPECT_TRUE(FixGateway::isTradablePrice(1.2650, LIMIT_ORDERTYPE));
    EXPECT_TRUE(FixGateway::isTradablePrice(1.2650, FXSWAP_ORDERTYPE));
    for (double price : { huge, -huge, 0.0, -1.25, std::numeric_limits<double>::quiet_NaN() })
    {
        EXPECT_FALSE(FixGateway::isTradablePrice(price, LIMIT_ORDERTYPE)) << price;
        EXPECT_FALSE(FixGateway::isTradablePrice(price, FXSWAP_ORDERTYPE)) << price;
    }
    // A market order trades at no price of its own, so any finite one will do
    EXPECT_TRUE(FixGateway::isTradablePrice(0.0, MARKET_ORDERTYPE));
    EXPECT_FALSE(FixGateway::isTradablePrice(huge, MARKET_ORDERTYPE));
    EXPECT_FALSE(FixGateway::isTradablePrice(-huge, MARKET_ORDERTYPE));
}

// =============================================================================
// Session Routing Tests
// =============================================================================

TEST_F(FixGatewayInboundTest, SessionMapPopulatedOnLogon)
{
    std::string source = FixGateway::makeSourceString(TEST_SID);

    EXPECT_FALSE(gateway_->hasFixSession(source));
    gateway_->onLogon(TEST_SID);
    EXPECT_TRUE(gateway_->hasFixSession(source));
}

TEST_F(FixGatewayInboundTest, ASessionStaysKnownWhileItsClientIsAway)
{
    // QuickFIX creates each session it is configured with at start, before any logon, and keeps what it is sent while
    // the client is logged out, for a ResendRequest. A report for the session's order must reach it then too: the
    // gateway used to forget a session at logout, and dropped the report (#160).
    std::string source = FixGateway::makeSourceString(TEST_SID);
    gateway_->onCreate(TEST_SID);
    EXPECT_TRUE(gateway_->hasFixSession(source)) << "a session isn't known until its client logs on";
    gateway_->onLogon(TEST_SID);
    gateway_->onLogout(TEST_SID);
    EXPECT_TRUE(gateway_->hasFixSession(source)) << "a session was forgotten when its client logged out";
}

TEST_F(FixGatewayInboundTest, MakeSourceString_Format)
{
    std::string source = FixGateway::makeSourceString(TEST_SID);
    EXPECT_EQ("FIX:CLIENT_A->ORDER_PROCESSOR", source);
}

// =============================================================================
// Inbound Dispatch Tests
// =============================================================================

TEST_F(FixGatewayInboundTest, FromApp_RoutesAGenericNewOrderSingle)
{
    // The session hands fromApp() a plain FIX::Message, as here. FIX44::MessageCracker cast it to
    // NewOrderSingle through a temporary FIX44::Message, which UBSan reports as a member call on an object
    // of the wrong type; dispatch() builds the typed message instead.
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));

    const FIX::Message generic(makeNewOrderSingle("ORD-GEN", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100));
    gateway_->fromApp(generic, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(BUY_SIDE, capturedOrder->side_);
    EXPECT_DOUBLE_EQ(10.25, capturedOrder->price_);
    EXPECT_EQ(100u, capturedOrder->orderQty_);

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, FromApp_RoutesAGenericCancelRequest)
{
    OrderEntry *saved = saveOrderOf(TEST_SID, "ORD-G1");
    ASSERT_NE(nullptr, saved);

    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderCancelEvent &evt)
            {
                EXPECT_EQ(saved->orderId_, evt.id_);
            }));

    gateway_->fromApp(FIX::Message(makeCancelRequest("ORD-G1", "CANCEL-GEN")), TEST_SID);
}

TEST_F(FixGatewayInboundTest, FromApp_UnsupportedTypeStillReachesQuickFix)
{
    // QuickFIX turns UnsupportedMessageType into the right reject, so a type the gateway does not handle
    // must still throw it.
    FIX44::News news;
    news.set(FIX::Headline("not handled"));
    EXPECT_THROW(gateway_->fromApp(news, TEST_SID), FIX::UnsupportedMessageType);
}

// =============================================================================
// Exception Containment Tests (#36)
// =============================================================================

TEST_F(FixGatewayInboundTest, FromApp_HandlerExceptionIsContained)
{
    // The bug this covers: an exception from a handler left fromApp() into QuickFIX's session thread,
    // which handles only FIX::Exception, and terminated the server (#36). The incoming queue failing stands in for any
    // such exception; storage failing on the order's first write did, which an order no longer makes (#192). No session
    // is logged on here, so the reject cannot be sent; that is logged, not thrown.
    OrderEntry *refused = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                refused = evt.order_; // the queue never took it, so the test frees it
                throw std::runtime_error("injected queue failure");
            }));

    const FIX::Message generic(makeNewOrderSingle("ORD-EXC", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100));
    EXPECT_NO_THROW(gateway_->fromApp(generic, TEST_SID));
    ASSERT_NE(nullptr, refused) << "the handler never reached the queue";
    delete refused;
}

// =============================================================================
// Outbound ExecutionReport content (#56)
// =============================================================================

/// A report as OrderDataStorage keeps it since #56: a full copy, with an exec id. Value-initialized, so the fields
/// a test does not set (a correction's quantities, say) are zero rather than indeterminate.
template <typename T> T makeReport(ExecType type, OrderStatus status)
{
    T report{};
    report.type_ = type;
    report.orderStatus_ = status;
    report.orderId_ = IdT(7, 1);
    report.execId_ = IdT(8, 1);
    report.transactTime_ = 12345;
    report.market_ = INTERNAL_EXECUTION;
    return report;
}

TEST_F(FixGatewayInboundTest, ExecutionReport_RejectCarriesOrdRejReasonAndText)
{
    // A rejected order's report says why: 150=8, 39=8, OrdRejReason 103=0 (broker / exchange option, since the
    // engine's reasons are free text) and the reason itself in Text (58)
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto reject = makeReport<RejectExecEntry>(REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    reject.rejectReason_ = "There is no market for this instrument!";

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&reject, *order);

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_REJECTED, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_REJECTED, ordStatus.getValue());
    ASSERT_TRUE(report.isSetField(FIX::FIELD::OrdRejReason));
    FIX::OrdRejReason ordRejReason;
    report.get(ordRejReason);
    EXPECT_EQ(0, ordRejReason.getValue());
    ASSERT_TRUE(report.isSetField(FIX::FIELD::Text));
    FIX::Text text;
    report.get(text);
    EXPECT_EQ("There is no market for this instrument!", text.getValue());
}

TEST_F(FixGatewayInboundTest, ExecutionReport_AcknowledgementCarriesNoRejectFields)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_NEW, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_NEW, ordStatus.getValue());
    EXPECT_FALSE(report.isSetField(FIX::FIELD::OrdRejReason));
    EXPECT_FALSE(report.isSetField(FIX::FIELD::Text));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_AStopsReleaseIsTriggeredAndNew)
{
    // A held stop released by a trade at its stop price (#223) is reported with FIX 4.4's ExecType L, triggered or
    // activated by system, and the order NEW (#236). The gateway sent ExecType NEW (0) for an execution type it
    // didn't know
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto release = makeReport<ExecutionEntry>(TRIGGERED_EXECTYPE, NEW_ORDSTATUS);

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&release, *order);

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_TRIGGERED_OR_ACTIVATED_BY_SYSTEM, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_NEW, ordStatus.getValue());
    EXPECT_TRUE(report.isSetField(FIX::FIELD::ClOrdID));
    EXPECT_FALSE(report.isSetField(FIX::FIELD::OrdRejReason));
    EXPECT_FALSE(report.isSetField(FIX::FIELD::LastQty));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_AGtdOrdersReportCarriesItsExpireTime)
{
    // ExpireTime (126), to the millisecond, on a GTD order's reports, and on no other's (#240)
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->tif_ = GTD_TIF;
    order->expireTime_ = 1792098000250ULL; // 2026-10-15 21:00:00.250 UTC
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);
    ASSERT_TRUE(report.isSetField(FIX::FIELD::ExpireTime));
    EXPECT_EQ("20261015-21:00:00.250", report.getField(FIX::FIELD::ExpireTime));
    order->tif_ = DAY_TIF;
    EXPECT_FALSE(FixGateway::buildExecutionReport(&ack, *order).isSetField(FIX::FIELD::ExpireTime));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_ARestatementIsExecTypeDForAGtRenewal)
{
    // A business day's restatement of a GT order (#244): 150=D and 378=1, GT renewal / restatement, with the order's
    // status. No other report carries a restatement reason.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->tif_ = GTC_TIF;
    auto restated = makeReport<ExecutionEntry>(RESTATED_EXECTYPE, NEW_ORDSTATUS);
    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&restated, *order);
    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_RESTATED, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_NEW, ordStatus.getValue());
    FIX::ExecRestatementReason reason;
    ASSERT_TRUE(report.isSet(reason));
    report.get(reason);
    EXPECT_EQ(FIX::ExecRestatementReason_GT_RENEWAL, reason.getValue());
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    EXPECT_FALSE(FixGateway::buildExecutionReport(&ack, *order).isSetField(FIX::FIELD::ExecRestatementReason));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_AGtOrderCarriesItsDayFiguresOnceItTradedOnAnEarlierDay)
{
    // DayOrderQty (424), DayCumQty (425) and DayAvgPx (426), which FIX 4.4 gives a GT order on the days after its first
    // trade (#244): 30 of its 50 filled before today. One whose fills are all today's, or a DAY order, carries none.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->tif_ = GTC_TIF;
    order->orderQty_ = 100;
    order->cumQty_ = 50;
    order->leavesQty_ = 50;
    order->dayOrderQty_ = 70;
    order->dayCumQty_ = 20;
    order->dayAvgPx_ = 10.25;
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, PARTFILL_ORDSTATUS);
    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);
    FIX::DayOrderQty dayOrderQty;
    ASSERT_TRUE(report.isSet(dayOrderQty));
    report.get(dayOrderQty);
    EXPECT_DOUBLE_EQ(70.0, dayOrderQty.getValue());
    FIX::DayCumQty dayCumQty;
    report.get(dayCumQty);
    EXPECT_DOUBLE_EQ(20.0, dayCumQty.getValue());
    FIX::DayAvgPx dayAvgPx;
    report.get(dayAvgPx);
    EXPECT_DOUBLE_EQ(10.25, dayAvgPx.getValue());

    order->dayCumQty_ = 50;
    EXPECT_FALSE(FixGateway::buildExecutionReport(&ack, *order).isSetField(FIX::FIELD::DayOrderQty))
        << "every fill is today's";
    order->dayCumQty_ = 20;
    order->tif_ = DAY_TIF;
    EXPECT_FALSE(FixGateway::buildExecutionReport(&ack, *order).isSetField(FIX::FIELD::DayOrderQty)) << "a DAY order";
}

TEST_F(FixGatewayInboundTest, ExecutionReport_CancelAckNamesTheCancelRequest)
{
    // FIX 4.4 answers a cancel request with the request's own ClOrdID in 11, and the cancelled order's in 41 (#98)
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    const auto &clOrd = order->clOrderId_.get();
    const std::string orderClOrdId(clOrd.data_, clOrd.length_);
    auto ack = makeReport<ExecutionEntry>(CANCEL_EXECTYPE, CANCELED_ORDSTATUS);

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order, "CXL-7");
    FIX::ClOrdID clOrdId;
    report.get(clOrdId);
    EXPECT_EQ("CXL-7", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    report.get(origClOrdId);
    EXPECT_EQ(orderClOrdId, origClOrdId.getValue());

    // A cancel no request asked for, as of a market order the engine can't fill, has no request ClOrdID: the order's
    // own stays in 11
    FIX44::ExecutionReport internal = FixGateway::buildExecutionReport(&ack, *order);
    internal.get(clOrdId);
    EXPECT_EQ(orderClOrdId, clOrdId.getValue());
    EXPECT_FALSE(internal.isSetField(FIX::FIELD::OrigClOrdID));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_TradeCarriesLastQtyAndLastPx)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    trade.lastQty_ = 40;
    trade.lastPx_ = 10.5;

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_TRADE, execType.getValue());
    FIX::LastQty lastQty;
    report.get(lastQty);
    EXPECT_DOUBLE_EQ(40, lastQty.getValue());
    FIX::LastPx lastPx;
    report.get(lastPx);
    EXPECT_DOUBLE_EQ(10.5, lastPx.getValue());
    EXPECT_FALSE(report.isSetField(FIX::FIELD::Text));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_TradeCarriesItsTradeDate)
{
    // TradeDate (75), a LocalMktDate: the business day the trade counts in, YYYYMMDD, as the engine keeps it (#258). The
    // gateway sent none, and the engine's own was 1 for every trade.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    trade.lastQty_ = 40;
    trade.lastPx_ = 10.5;
    trade.tradeDate_ = 20261016;

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);
    ASSERT_TRUE(report.isSetField(FIX::FIELD::TradeDate));
    FIX::TradeDate tradeDate;
    report.get(tradeDate);
    EXPECT_EQ("20261016", tradeDate.getString());

    trade.tradeDate_ = 0;
    EXPECT_FALSE(FixGateway::buildExecutionReport(&trade, *order).isSetField(FIX::FIELD::TradeDate))
        << "a trade in no day has a date";
}

TEST_F(FixGatewayInboundTest, ExecutionReport_CorrectionIsTradeCorrect)
{
    // Corrections used to fall through fromExecType's default and go out as ExecType NEW
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto correct = makeReport<ExecCorrectExecEntry>(CORRECT_EXECTYPE, PARTFILL_ORDSTATUS);

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&correct, *order);

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_TRADE_CORRECT, execType.getValue());
}

TEST_F(FixGatewayInboundTest, ExecutionReport_ReplaceNamesTheReplacedOrder)
{
    // FIX acknowledges a replace with the replacement's report: 150=5, its own OrderID, ClOrdID and status, and the
    // ClOrdID of the order it replaced in OrigClOrdID (41) (#74). Other reports carry no 41.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->orderId_ = IdT(9, 1);
    order->origClOrderId_ = WideDataStorage::instance()->add(new RawDataEntry(STRING_RAWDATATYPE, "ORD-1", 5));
    auto replace = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, PARTFILL_ORDSTATUS);
    replace.orderId_ = order->orderId_;
    replace.origOrderId_ = IdT(7, 1);

    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&replace, *order);

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_REPLACED, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_PARTIALLY_FILLED, ordStatus.getValue());
    FIX::OrderID orderId;
    report.get(orderId);
    EXPECT_EQ("9", orderId.getValue());
    const RawDataEntry &clOrd = order->clOrderId_.get();
    FIX::ClOrdID clOrdId;
    report.get(clOrdId);
    EXPECT_EQ(std::string(clOrd.data_, clOrd.length_), clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    report.get(origClOrdId);
    EXPECT_EQ("ORD-1", origClOrdId.getValue());

    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    EXPECT_FALSE(FixGateway::buildExecutionReport(&ack, *order).isSetField(FIX::FIELD::OrigClOrdID));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_CarriesTheClientsOwnClOrdIds)
{
    // Stored under the session's key, the ClOrdIDs go out as the client sent them (#71): 11 on every report, 41 on the
    // replacement's REPLACE report and on a cancel's acknowledgement
    const std::string source = FixGateway::makeSourceString(TEST_SID);
    auto order = createCorrectOrder(instrumentId1_);
    order->source_ = WideDataStorage::instance()->add(new StringT(source));
    const std::string key = FixGateway::sessionClOrdId(source, "ORD-2");
    order->clOrderId_ = WideDataStorage::instance()->add(
        new RawDataEntry(STRING_RAWDATATYPE, key.c_str(), static_cast<u32>(key.size())));
    const std::string origKey = FixGateway::sessionClOrdId(source, "ORD-1");
    order->origClOrderId_ = WideDataStorage::instance()->add(
        new RawDataEntry(STRING_RAWDATATYPE, origKey.c_str(), static_cast<u32>(origKey.size())));

    FIX::ClOrdID clOrdId;
    FIX::OrigClOrdID origClOrdId;
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    FixGateway::buildExecutionReport(&ack, *order).get(clOrdId);
    EXPECT_EQ("ORD-2", clOrdId.getValue());

    auto replace = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, NEW_ORDSTATUS);
    FIX44::ExecutionReport replaced = FixGateway::buildExecutionReport(&replace, *order);
    replaced.get(clOrdId);
    EXPECT_EQ("ORD-2", clOrdId.getValue());
    replaced.get(origClOrdId);
    EXPECT_EQ("ORD-1", origClOrdId.getValue());

    auto cancel = makeReport<ExecutionEntry>(CANCEL_EXECTYPE, CANCELED_ORDSTATUS);
    FixGateway::buildExecutionReport(&cancel, *order, "CXL-9").get(origClOrdId);
    EXPECT_EQ("ORD-2", origClOrdId.getValue());
}

TEST_F(FixGatewayInboundTest, ExecutionReport_OnlyTheReplacementsReplaceReportGoesOverFix)
{
    // The engine reports a replace for both orders. FIX acknowledges it once, with the replacement's report: the
    // replaced order's has no FIX 4.4 form, since 39=5 left the protocol (#74).
    auto replaced = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, REPLACED_ORDSTATUS);
    auto replacement = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, NEW_ORDSTATUS);
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, FILLED_ORDSTATUS);
    EXPECT_FALSE(FixGateway::reportsOverFix(&replaced));
    EXPECT_TRUE(FixGateway::reportsOverFix(&replacement));
    EXPECT_TRUE(FixGateway::reportsOverFix(&trade));
}

// =============================================================================
// Orders refused without being stored (#67)
// =============================================================================

/// An order the engine refused because its ClOrdID is already in use
Queues::OrderRejectEvent makeOrderReject(bool replacement)
{
    Queues::OrderRejectEvent reject;
    reject.clOrderId_ = "ORD-7";
    reject.origClOrderId_ = replacement ? "ORD-1" : "";
    reject.symbol_ = "aaa";
    reject.side_ = SELL_SIDE;
    reject.orderQty_ = 40;
    reject.replacement_ = replacement;
    reject.duplicateClOrderId_ = true;
    reject.reason_ = "Order refused: ClOrdID ORD-7 is already in use";
    return reject;
}

TEST(FixOrderRejectTest, DuplicateNewOrderGetsAnExecutionReportReject)
{
    // The FIX convention for a duplicate order: 150=8, 39=8, OrdRejReason 6, and the reason in Text (58). The order
    // was never stored, so it has no OrderID.
    FIX44::ExecutionReport report = FixGateway::buildOrderReject(makeOrderReject(false), 41);

    FIX::OrderID orderId;
    report.get(orderId);
    EXPECT_EQ("NONE", orderId.getValue());
    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_REJECTED, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_REJECTED, ordStatus.getValue());
    FIX::OrdRejReason ordRejReason;
    report.get(ordRejReason);
    EXPECT_EQ(FIX::OrdRejReason_DUPLICATE_ORDER, ordRejReason.getValue());
    FIX::Text text;
    report.get(text);
    EXPECT_EQ("Order refused: ClOrdID ORD-7 is already in use", text.getValue());
    FIX::ClOrdID clOrdId;
    report.get(clOrdId);
    EXPECT_EQ("ORD-7", clOrdId.getValue());
    FIX::Side side;
    report.get(side);
    EXPECT_EQ(FIX::Side_SELL, side.getValue());
    FIX::OrderQty orderQty;
    report.get(orderQty);
    EXPECT_DOUBLE_EQ(40, orderQty.getValue());
    FIX::LeavesQty leavesQty;
    report.get(leavesQty);
    EXPECT_DOUBLE_EQ(0, leavesQty.getValue());
    FIX::Symbol symbol;
    report.get(symbol);
    EXPECT_EQ("aaa", symbol.getValue());
}

TEST(FixOrderRejectTest, TheSenderSeesItsOwnClOrdIdsInAnOrderReject)
{
    // The engine names a ClOrdID in use as it is stored, with the session's key, both in the event and in its reason;
    // the client gets its own (#71)
    const std::string source = FixGateway::makeSourceString(TEST_SID);
    Queues::OrderRejectEvent evnt = makeOrderReject(true);
    evnt.clOrderId_ = FixGateway::sessionClOrdId(source, "ORD-7");
    evnt.origClOrderId_ = FixGateway::sessionClOrdId(source, "ORD-1");
    evnt.reason_ = "Replace refused: ClOrdID " + evnt.clOrderId_ + " is already in use";

    const Queues::OrderRejectEvent shown = FixGateway::clientView(evnt, source);
    EXPECT_EQ("ORD-7", shown.clOrderId_);
    EXPECT_EQ("ORD-1", shown.origClOrderId_);
    EXPECT_EQ("Replace refused: ClOrdID ORD-7 is already in use", shown.reason_);

    FIX44::OrderCancelReject reject = FixGateway::buildReplaceReject(shown, "7", FIX::OrdStatus_NEW);
    FIX::ClOrdID clOrdId;
    reject.get(clOrdId);
    EXPECT_EQ("ORD-7", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    reject.get(origClOrdId);
    EXPECT_EQ("ORD-1", origClOrdId.getValue());
    FIX::Text text;
    reject.get(text);
    EXPECT_EQ("Replace refused: ClOrdID ORD-7 is already in use", text.getValue());

    // A ClOrdID the gateway quotes as the client sent it stays as it is
    EXPECT_EQ("ORD-7", FixGateway::clientView(makeOrderReject(true), source).clOrderId_);
}

TEST(FixOrderRejectTest, AnOrderRejectsExecIdIsTheIdItIsGiven)
{
    // sendOrderReject() gives each reject an id from the shared generator, whose saved limit keeps it from repeating
    // after a restart (#81). Before, the gateway numbered rejects itself, from R1 in every run.
    FIX::ExecID first, second;
    FixGateway::buildOrderReject(makeOrderReject(false), 10007).get(first);
    FixGateway::buildOrderReject(makeOrderReject(false), 10008).get(second);
    EXPECT_EQ("R10007", first.getValue());
    EXPECT_EQ("R10008", second.getValue());
}

TEST(FixOrderRejectTest, OrderRefusedForAnotherReasonUsesTheBrokerOptionCode)
{
    Queues::OrderRejectEvent evnt = makeOrderReject(false);
    evnt.duplicateClOrderId_ = false;
    FIX::OrdRejReason ordRejReason;
    FixGateway::buildOrderReject(evnt, 42).get(ordRejReason);
    EXPECT_EQ(0, ordRejReason.getValue());
}

TEST(FixOrderRejectTest, AnOrderTheGatewayRefusesGetsAnExecutionReportRejectQuotingItsQuantityAsSent)
{
    // Refused before the engine sees it (#145), so it has no OrderID; OrderQty is the client's own, which QuantityT
    // can't hold, and OrdRejReason 13 is FIX's code for an incorrect quantity
    FIX44::NewOrderSingle order;
    order.set(FIX::ClOrdID("QTY-1"));
    order.set(FIX::Side(FIX::Side_SELL));
    order.set(FIX::Symbol("aaa"));
    order.set(FIX::OrderQty(-5));
    const std::string why = "Order refused: OrderQty (38) must be a whole number from 1 to 4294967295";
    FIX44::ExecutionReport report = FixGateway::buildOrderRefusal(order, FIX::OrdRejReason_INCORRECT_QUANTITY, why, 43);

    FIX::OrderID orderId;
    report.get(orderId);
    EXPECT_EQ("NONE", orderId.getValue());
    FIX::ExecID execId;
    report.get(execId);
    EXPECT_EQ("R43", execId.getValue());
    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(FIX::ExecType_REJECTED, execType.getValue());
    FIX::OrdStatus ordStatus;
    report.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_REJECTED, ordStatus.getValue());
    FIX::ClOrdID clOrdId;
    report.get(clOrdId);
    EXPECT_EQ("QTY-1", clOrdId.getValue());
    FIX::Side side;
    report.get(side);
    EXPECT_EQ(FIX::Side_SELL, side.getValue());
    FIX::Symbol symbol;
    report.get(symbol);
    EXPECT_EQ("aaa", symbol.getValue());
    EXPECT_EQ("-5", report.getField(FIX::FIELD::OrderQty));
    FIX::OrdRejReason ordRejReason;
    report.get(ordRejReason);
    EXPECT_EQ(FIX::OrdRejReason_INCORRECT_QUANTITY, ordRejReason.getValue());
    FIX::Text text;
    report.get(text);
    EXPECT_EQ(why, text.getValue());
}

TEST(FixOrderRejectTest, DuplicateReplacementGetsAnOrderCancelReject)
{
    // A cancel/replace request is refused with an OrderCancelReject (35=9) that answers it (434=2), says why
    // (CxlRejReason 6, duplicate ClOrdID; Text 58), and describes the order it was to replace
    FIX44::OrderCancelReject reject =
        FixGateway::buildReplaceReject(makeOrderReject(true), "12", FIX::OrdStatus_PARTIALLY_FILLED);

    EXPECT_EQ("9", reject.getHeader().getField(FIX::FIELD::MsgType));
    FIX::OrderID orderId;
    reject.get(orderId);
    EXPECT_EQ("12", orderId.getValue());
    FIX::ClOrdID clOrdId;
    reject.get(clOrdId);
    EXPECT_EQ("ORD-7", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    reject.get(origClOrdId);
    EXPECT_EQ("ORD-1", origClOrdId.getValue());
    FIX::OrdStatus ordStatus;
    reject.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_PARTIALLY_FILLED, ordStatus.getValue());
    FIX::CxlRejResponseTo responseTo;
    reject.get(responseTo);
    EXPECT_EQ(FIX::CxlRejResponseTo_ORDER_CANCEL_REPLACE_REQUEST, responseTo.getValue());
    FIX::CxlRejReason cxlRejReason;
    reject.get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_DUPLICATE_CL_ORD_ID, cxlRejReason.getValue());
    FIX::Text text;
    reject.get(text);
    EXPECT_EQ("Order refused: ClOrdID ORD-7 is already in use", text.getValue());
}

// =============================================================================
// Cancels the engine refused (#73)
// =============================================================================

TEST(FixOrderRejectTest, RefusedReplaceSaysWhyAsARefusedCancelDoes)
{
    // The engine's reason maps to CxlRejReason as for a refused cancel, and a ClOrdID already in use still says 6 (#74)
    Queues::OrderRejectEvent evnt = makeOrderReject(true);
    evnt.duplicateClOrderId_ = false;
    FIX::CxlRejReason cxlRejReason;
    const std::pair<Queues::CancelRejectEvent::Reason, int> cases[] = {
        { Queues::CancelRejectEvent::TOO_LATE, FIX::CxlRejReason_TOO_LATE_TO_CANCEL },
        { Queues::CancelRejectEvent::UNKNOWN_ORDER, FIX::CxlRejReason_UNKNOWN_ORDER },
        { Queues::CancelRejectEvent::PENDING, FIX::CxlRejReason_ORDER_ALREADY_IN_PENDING_STATUS },
        { Queues::CancelRejectEvent::OTHER, FIX::CxlRejReason_OTHER },
    };
    for (const auto &[refusal, code] : cases)
    {
        evnt.refusal_ = refusal;
        FixGateway::buildReplaceReject(evnt, "12", FIX::OrdStatus_NEW).get(cxlRejReason);
        EXPECT_EQ(code, cxlRejReason.getValue()) << "refusal " << refusal;
    }
    evnt.duplicateClOrderId_ = true;
    FixGateway::buildReplaceReject(evnt, "12", FIX::OrdStatus_NEW).get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_DUPLICATE_CL_ORD_ID, cxlRejReason.getValue());
}

TEST(FixOrderRejectTest, ReplaceOfAnUnknownOrderIsRejectedAsUnknown)
{
    // The gateway's own answer to an OrigClOrdID that names no order (#74): 102=1, OrderID NONE, OrdStatus 8, and the
    // request's ClOrdIDs. Before, the request got no answer at all.
    FIX44::OrderCancelReject reject = FixGateway::buildUnknownReplaceReject("RPL-2", "ORD-9");

    EXPECT_EQ("9", reject.getHeader().getField(FIX::FIELD::MsgType));
    FIX::OrderID orderId;
    reject.get(orderId);
    EXPECT_EQ("NONE", orderId.getValue());
    FIX::ClOrdID clOrdId;
    reject.get(clOrdId);
    EXPECT_EQ("RPL-2", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    reject.get(origClOrdId);
    EXPECT_EQ("ORD-9", origClOrdId.getValue());
    FIX::OrdStatus ordStatus;
    reject.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_REJECTED, ordStatus.getValue());
    FIX::CxlRejResponseTo responseTo;
    reject.get(responseTo);
    EXPECT_EQ(FIX::CxlRejResponseTo_ORDER_CANCEL_REPLACE_REQUEST, responseTo.getValue());
    FIX::CxlRejReason cxlRejReason;
    reject.get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_UNKNOWN_ORDER, cxlRejReason.getValue());
    FIX::Text text;
    reject.get(text);
    EXPECT_EQ("Replace refused: unknown order", text.getValue());
}

TEST(FixCancelRejectTest, CancelOfAFilledOrderIsTooLate)
{
    // An OrderCancelReject (35=9) answering the cancel request (434=1): too late to cancel (102=0), with the order's
    // status (39=2) and why in Text (58)
    Queues::CancelRejectEvent evnt;
    evnt.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    evnt.ordStatus_ = FILLED_ORDSTATUS;
    FIX44::OrderCancelReject reject = FixGateway::buildCancelReject(evnt, "7", "ORD-1", "ORD-1");

    EXPECT_EQ("9", reject.getHeader().getField(FIX::FIELD::MsgType));
    FIX::OrderID orderId;
    reject.get(orderId);
    EXPECT_EQ("7", orderId.getValue());
    FIX::ClOrdID clOrdId;
    reject.get(clOrdId);
    EXPECT_EQ("ORD-1", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    reject.get(origClOrdId);
    EXPECT_EQ("ORD-1", origClOrdId.getValue());
    FIX::OrdStatus ordStatus;
    reject.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_FILLED, ordStatus.getValue());
    FIX::CxlRejResponseTo responseTo;
    reject.get(responseTo);
    EXPECT_EQ(FIX::CxlRejResponseTo_ORDER_CANCEL_REQUEST, responseTo.getValue());
    FIX::CxlRejReason cxlRejReason;
    reject.get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_TOO_LATE_TO_CANCEL, cxlRejReason.getValue());
    FIX::Text text;
    reject.get(text);
    EXPECT_EQ("Cancel rejected: too late, the order is FILLED", text.getValue());
}

TEST(FixCancelRejectTest, AnEngineRejectNamesTheCancelRequestAndTheOrder)
{
    // The engine carries the request's ClOrdID back since #98: 11 is the request's, 41 the order's
    Queues::CancelRejectEvent evnt;
    evnt.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    evnt.ordStatus_ = CANCELED_ORDSTATUS;
    evnt.requestClOrdId_ = "CXL-8";
    FIX44::OrderCancelReject reject = FixGateway::buildEngineCancelReject(evnt, "7", "ORD-1");
    FIX::ClOrdID clOrdId;
    reject.get(clOrdId);
    EXPECT_EQ("CXL-8", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    reject.get(origClOrdId);
    EXPECT_EQ("ORD-1", origClOrdId.getValue());

    // An event without it falls back to the order's in 11, as before #98
    evnt.requestClOrdId_.clear();
    FixGateway::buildEngineCancelReject(evnt, "7", "ORD-1").get(clOrdId);
    EXPECT_EQ("ORD-1", clOrdId.getValue());
}

TEST(FixCancelRejectTest, CancelOfAnUnknownOrderIsRejectedAsUnknown)
{
    // The gateway's own answer to an unknown OrigClOrdID: 102=1, and OrdStatus 8, since there is no order to report
    Queues::CancelRejectEvent evnt;
    FIX44::OrderCancelReject reject = FixGateway::buildCancelReject(evnt, "NONE", "CXL-2", "ORD-9");

    FIX::OrderID orderId;
    reject.get(orderId);
    EXPECT_EQ("NONE", orderId.getValue());
    FIX::ClOrdID clOrdId;
    reject.get(clOrdId);
    EXPECT_EQ("CXL-2", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    reject.get(origClOrdId);
    EXPECT_EQ("ORD-9", origClOrdId.getValue());
    FIX::OrdStatus ordStatus;
    reject.get(ordStatus);
    EXPECT_EQ(FIX::OrdStatus_REJECTED, ordStatus.getValue());
    FIX::CxlRejReason cxlRejReason;
    reject.get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_UNKNOWN_ORDER, cxlRejReason.getValue());
    FIX::Text text;
    reject.get(text);
    EXPECT_EQ("Cancel rejected: unknown order", text.getValue());
}

TEST(FixCancelRejectTest, PendingAndOtherReasonsMapToTheirCodes)
{
    Queues::CancelRejectEvent evnt;
    evnt.ordStatus_ = NEW_ORDSTATUS;
    FIX::CxlRejReason cxlRejReason;

    evnt.reason_ = Queues::CancelRejectEvent::PENDING;
    FixGateway::buildCancelReject(evnt, "7", "ORD-1", "ORD-1").get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_ORDER_ALREADY_IN_PENDING_STATUS, cxlRejReason.getValue());

    evnt.reason_ = Queues::CancelRejectEvent::OTHER;
    FixGateway::buildCancelReject(evnt, "7", "ORD-1", "ORD-1").get(cxlRejReason);
    EXPECT_EQ(FIX::CxlRejReason_OTHER, cxlRejReason.getValue());
}

// =============================================================================
// MultiOutQueues Tests
// =============================================================================

class MultiOutQueuesTest : public ::testing::Test
{
};

TEST_F(MultiOutQueuesTest, FansOutToAllDelegates)
{
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    ExecReportEvent evt(nullptr);
    EXPECT_CALL(mock1, push(testing::An<const ExecReportEvent &>(), _)).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const ExecReportEvent &>(), _)).Times(1);
    multi.push(evt, "target");
}

TEST_F(MultiOutQueuesTest, FansOutCancelReject)
{
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    CancelRejectEvent evt;
    EXPECT_CALL(mock1, push(testing::An<const CancelRejectEvent &>(), _)).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const CancelRejectEvent &>(), _)).Times(1);
    multi.push(evt, "target");
}

TEST_F(MultiOutQueuesTest, FansOutOrderReject)
{
    // Without its own push, MultiOutQueues would take OutQueues' empty default and FIX clients would hear nothing (#67)
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    OrderRejectEvent evt;
    EXPECT_CALL(mock1, push(testing::An<const OrderRejectEvent &>(), "FIX:session")).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const OrderRejectEvent &>(), "FIX:session")).Times(1);
    OutQueues &queues = multi; // the engine pushes through the interface
    queues.push(evt, "FIX:session");
}

TEST_F(MultiOutQueuesTest, FansOutBusinessReject)
{
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    BusinessRejectEvent evt;
    EXPECT_CALL(mock1, push(testing::An<const BusinessRejectEvent &>(), _)).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const BusinessRejectEvent &>(), _)).Times(1);
    multi.push(evt, "target");
}

TEST_F(MultiOutQueuesTest, EmptyDelegatesNoCrash)
{
    MultiOutQueues multi;
    ExecReportEvent evt(nullptr);
    multi.push(evt, "target");
    SUCCEED();
}

TEST_F(FixGatewayInboundTest, ExecutionReport_ASwapsLegsCarryTheirSideDateAndPrice)
{
    // A swap's near leg trades on the order's side and settles on its near date; its far leg trades the other way and
    // settles on its far date. Each report carries its leg in a NoLegs group and a TradeLinkID (820) (#153).
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->side_ = BUY_SIDE;
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->settlDate_ = 1792022400000ULL;    // 2026-10-15, midnight UTC
    order->farSettlDate_ = 1794700800000ULL; // 2026-11-15
    order->farPrice_ = 1.0872;
    for (const ExecLegType leg : { NEAR_LEG, FAR_LEG })
    {
        auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
        trade.execLegType_ = leg;
        trade.tradeLink_ = 4242;
        // A millisecond apart: the link, not the time, pairs them (#199)
        trade.transactTime_ = (NEAR_LEG == leg) ? 1792022400999ULL : 1792022401000ULL;
        trade.lastQty_ = 100;
        trade.lastPx_ = (NEAR_LEG == leg) ? 1.0850 : 1.0872;

        FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);

        ASSERT_EQ(1u, report.groupCount(FIX::FIELD::NoLegs)) << "leg " << leg;
        FIX44::ExecutionReport::NoLegs group;
        report.getGroup(1, group);
        FIX::LegSide side;
        group.get(side);
        EXPECT_EQ((NEAR_LEG == leg) ? FIX::Side_BUY : FIX::Side_SELL, side.getValue()) << "leg " << leg;
        // Which date each leg gets, as a LocalMktDate: it used to be the engine's milliseconds (#198)
        FIX::LegSettlDate date;
        group.get(date);
        EXPECT_EQ((NEAR_LEG == leg) ? "20261015" : "20261115", date.getString()) << "leg " << leg;
        FIX::LegLastPx px;
        group.get(px);
        EXPECT_DOUBLE_EQ(trade.lastPx_, px.getValue()) << "leg " << leg;
        // The same on both legs: the swap fill's link (#199)
        ASSERT_TRUE(report.isSetField(820)) << "leg " << leg;
        EXPECT_EQ("4242", report.getField(820)) << "leg " << leg;
    }

    // A plain fill has no legs
    auto plain = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    FIX44::ExecutionReport report = FixGateway::buildExecutionReport(&plain, *order);
    EXPECT_EQ(0u, report.groupCount(FIX::FIELD::NoLegs));
    EXPECT_FALSE(report.isSetField(820));
}

// --- Settlement dates, as FIX's LocalMktDate (#198, #190) ---

/// 2026-10-15 and 2026-11-15 at midnight UTC, in the engine's milliseconds, as the frontend sends them
const DateTimeT OCT_15 = 1792022400000ULL;
const DateTimeT NOV_15 = 1794700800000ULL;

TEST(FixLocalMktDateTest, ADateIsMidnightUtcOfThatDay)
{
    EXPECT_EQ(std::optional<DateTimeT>(OCT_15), FixGateway::fromLocalMktDate("20261015"));
    EXPECT_EQ(std::optional<DateTimeT>(0u), FixGateway::fromLocalMktDate("19700101"));
    EXPECT_EQ(std::optional<DateTimeT>(1709164800000ULL), FixGateway::fromLocalMktDate("20240229")); // a leap day
    EXPECT_EQ("20261015", FixGateway::toLocalMktDate(OCT_15));
    EXPECT_EQ("20261015", FixGateway::toLocalMktDate(OCT_15 + 86399999)); // its last millisecond
    EXPECT_EQ("20261016", FixGateway::toLocalMktDate(OCT_15 + 86400000));
    EXPECT_EQ("19700101", FixGateway::toLocalMktDate(0));
    EXPECT_EQ("99991231", FixGateway::toLocalMktDate(std::numeric_limits<DateTimeT>::max()));
}

TEST(FixLocalMktDateTest, AnythingElseIsNotADate)
{
    // std::stoull took the first two, as 18446744073709551611 and 1000 ms into 1970, and threw on the rest (#190)
    for (const char *text :
         { "-5", "1000", "2026101", "202610150", "abcdefgh", "", " 2026101", "+2026101", "123456789012345678901",
           "2026-10-", "20230229", "20261301", "20261000", "20261032", "19691231" })
    {
        EXPECT_FALSE(FixGateway::fromLocalMktDate(text).has_value()) << "'" << text << "'";
    }
}

TEST_F(FixGatewayInboundTest, ASwapsSettlementDatesAreReadAsDates)
{
    // NewOrderSingle's 64 and 6501 here; NewOrderMultileg_FxSwap_PushesToQueue checks each leg's 588
    OrderEntry *capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                capturedOrder = evt.order_;
            }));
    auto msg = makeNewOrderSingle("SWAP-DATE-1", "aaa", FIX::Side_BUY, FIX::OrdType_FOREX_SWAP, 1.0850, 1000000);
    msg.setField(6500, "1.0872");
    msg.setField(FIX::FIELD::SettlDate, "20261015");
    msg.setField(6501, "20261115");
    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(OCT_15, capturedOrder->settlDate_);
    EXPECT_EQ(NOV_15, capturedOrder->farSettlDate_);
    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, ASwapWithADateThatIsntOneIsNotQueued)
{
    // Refused to the sender before anything is stored (#190); there is no session here to send that to
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    int n = 0;
    for (const char *bad : { "-5", "abc", "123456789012345678901", "20261301" })
    {
        for (const int field : { static_cast<int>(FIX::FIELD::SettlDate), 6501 })
        {
            auto msg = makeNewOrderSingle("SWAP-BAD-" + std::to_string(++n), "aaa", FIX::Side_BUY,
                                          FIX::OrdType_FOREX_SWAP, 1.0850, 1000000);
            msg.setField(6500, "1.0872");
            msg.setField(FIX::FIELD::SettlDate, "20261015");
            msg.setField(6501, "20261115");
            msg.setField(field, bad);
            gateway_->onMessage(msg, TEST_SID);
        }
        gateway_->onMessage(makeFxSwap("SWAP-BAD-" + std::to_string(++n), bad, "20261115"), TEST_SID);
        gateway_->onMessage(makeFxSwap("SWAP-BAD-" + std::to_string(++n), "20261015", bad), TEST_SID);
    }
}

} // anonymous namespace

#endif // BUILD_FIX
