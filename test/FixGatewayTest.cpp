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
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include "FixGateway.h"
#include "MultiOutQueues.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"
#include "MockStorage.h"

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
        FIX::UtcTimeStamp now;
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

    /// An FX swap built as NewOrderMultileg_FxSwap_PushesToQueue builds it; it names no account
    FIX44::NewOrderMultileg makeFxSwap(const std::string &clOrdId)
    {
        FIX::UtcTimeStamp now;
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
        nearLeg.set(FIX::LegSettlDate("1000"));
        msg.addGroup(nearLeg);

        FIX44::NewOrderMultileg::NoLegs farLeg;
        farLeg.set(FIX::LegSymbol("aaa"));
        farLeg.set(FIX::LegSide(FIX::Side_SELL));
        farLeg.set(FIX::LegPrice(1.2680));
        farLeg.set(FIX::LegSettlDate("2000"));
        msg.addGroup(farLeg);

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
    // First create an order so we can cancel it
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);

    // Get the clOrderId string
    const auto &clOrd = saved->clOrderId_.get();
    std::string clOrdStr(clOrd.data_, clOrd.length_);

    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &source, const OrderCancelEvent &evt)
            {
                EXPECT_TRUE(source.find("FIX:") != std::string::npos);
                EXPECT_EQ(saved->orderId_, evt.id_);
                // the request's own ClOrdID, which the engine hands back on the cancel's answers (#98)
                EXPECT_EQ("CANCEL001", evt.requestClOrdId_);
            }));

    FIX::UtcTimeStamp now;
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
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    saved->status_ = NEW_ORDSTATUS;

    const auto &clOrd = saved->clOrderId_.get();
    std::string clOrdStr(clOrd.data_, clOrd.length_);

    OrderEntry *capturedReplacement = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &source, const OrderReplaceEvent &evt)
            {
                EXPECT_TRUE(source.find("FIX:") != std::string::npos);
                EXPECT_EQ(saved->orderId_, evt.id_);
                capturedReplacement = evt.replacementOrder_;
            }));

    FIX::UtcTimeStamp now2;
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
    const RawDataEntry &newClOrd = capturedReplacement->clOrderId_.get();
    EXPECT_EQ("REPLACE001", std::string(newClOrd.data_, newClOrd.length_));
    const RawDataEntry &origClOrd = capturedReplacement->origClOrderId_.get();
    EXPECT_EQ(clOrdStr, std::string(origClOrd.data_, origClOrd.length_));
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
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    const auto &clOrd = saved->clOrderId_.get();

    OrderEntry *capturedReplacement = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                capturedReplacement = evt.replacementOrder_;
            }));

    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID(std::string(clOrd.data_, clOrd.length_)));
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
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    const auto &clOrd = saved->clOrderId_.get();
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);

    FIX44::OrderCancelReplaceRequest replaceMsg;
    replaceMsg.set(FIX::OrigClOrdID(std::string(clOrd.data_, clOrd.length_)));
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

    FIX::UtcTimeStamp now;
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
    nearLeg.set(FIX::LegSettlDate("1000"));
    msg.addGroup(nearLeg);

    // Far leg: SELL at 1.2680, settl T+92
    FIX44::NewOrderMultileg::NoLegs farLeg;
    farLeg.set(FIX::LegSymbol("aaa"));
    farLeg.set(FIX::LegSide(FIX::Side_SELL));
    farLeg.set(FIX::LegPrice(1.2680));
    farLeg.set(FIX::LegSettlDate("2000"));
    msg.addGroup(farLeg);

    gateway_->onMessage(msg, TEST_SID);

    ASSERT_NE(nullptr, capturedOrder);
    EXPECT_EQ(FXSWAP_ORDERTYPE, capturedOrder->ordType_);
    EXPECT_EQ(BUY_SIDE, capturedOrder->side_);
    EXPECT_DOUBLE_EQ(1.2650, capturedOrder->price_);    // near (matches root side)
    EXPECT_DOUBLE_EQ(1.2680, capturedOrder->farPrice_); // far (opposite side)
    EXPECT_EQ(1000u, capturedOrder->settlDate_);
    EXPECT_EQ(2000u, capturedOrder->farSettlDate_);
    EXPECT_EQ(1000000u, capturedOrder->orderQty_);

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_NonSwapOrdType_Rejected)
{
    // NewOrderMultileg with non-FXSwap OrdType should be rejected (no push)
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);

    FIX::UtcTimeStamp now;
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

    FIX::UtcTimeStamp now;
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
// Session Routing Tests
// =============================================================================

TEST_F(FixGatewayInboundTest, SessionMapPopulatedOnLogon)
{
    std::string source = FixGateway::makeSourceString(TEST_SID);

    EXPECT_FALSE(gateway_->hasFixSession(source));
    gateway_->onLogon(TEST_SID);
    EXPECT_TRUE(gateway_->hasFixSession(source));
    gateway_->onLogout(TEST_SID);
    EXPECT_FALSE(gateway_->hasFixSession(source));
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
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    const auto &clOrd = saved->clOrderId_.get();

    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderCancelEvent &evt)
            {
                EXPECT_EQ(saved->orderId_, evt.id_);
            }));

    FIX44::OrderCancelRequest cancelMsg;
    cancelMsg.set(FIX::OrigClOrdID(std::string(clOrd.data_, clOrd.length_)));
    cancelMsg.set(FIX::ClOrdID("CANCEL-GEN"));
    cancelMsg.set(FIX::Side(FIX::Side_BUY));
    cancelMsg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    cancelMsg.set(FIX::Symbol("aaa"));

    gateway_->fromApp(FIX::Message(cancelMsg), TEST_SID);
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
    // which handles only FIX::Exception, and terminated the server (#36). Storage failing on the order's
    // first write stands in for any such exception. No session is logged on here, so the reject cannot be
    // sent; that is logged, not thrown.
    testing::NiceMock<MockDataSaver> saver;
    ON_CALL(saver, save(testing::An<const RawDataEntry &>()))
        .WillByDefault(testing::Throw(std::runtime_error("injected storage failure")));
    WideDataStorage::instance()->bindStorage(&saver);
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);

    const FIX::Message generic(makeNewOrderSingle("ORD-EXC", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100));
    EXPECT_NO_THROW(gateway_->fromApp(generic, TEST_SID));
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

} // anonymous namespace

#endif // BUILD_FIX
