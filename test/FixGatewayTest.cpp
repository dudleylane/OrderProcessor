/**
 Concurrent Order Processor library - FIX Gateway Test Suite

 Tests for enum conversion, inbound FIX message translation, outbound
 ExecutionReport construction, and MultiOutQueues fan-out.
*/

#ifdef BUILD_FIX

// QuickFIX headers MUST come before any <flat_map> include (see FixGateway.h)
#include <quickfix/fix50sp2/NewOrderSingle.h>
#include <quickfix/fix50sp2/NewOrderMultileg.h>
#include <quickfix/fix50sp2/OrderCancelRequest.h>
#include <quickfix/fix50sp2/OrderCancelReplaceRequest.h>
#include <quickfix/fix50sp2/News.h>
#include <quickfix/fixt11/Logon.h>
#include <quickfix/fixt11/Heartbeat.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include <quickfix/FieldConvertors.h>
#include <quickfix/MessageStore.h>
#include <quickfix/SessionFactory.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/TimeRange.h>
#include <quickfix/Utility.h>
#include "FixGateway.h"
#include "FixServer.h"
#include "MultiOutQueues.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"
#include "MockStorage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace COP;
using namespace COP::App;
using namespace COP::Store;
using namespace COP::Queues;
using namespace test;
using ::testing::_;
using ::testing::Invoke;

namespace
{

const FIX::SessionID TEST_SID("FIXT.1.1", "CLIENT_A", "ORDER_PROCESSOR", "");
/// A second client, whose orders TEST_SID must not be able to name (#71)
const FIX::SessionID OTHER_SID("FIXT.1.1", "CLIENT_B", "ORDER_PROCESSOR", "");

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
    // 39=5, Replaced, has been out of use since FIX 4.4, though FIX 5.0 SP2 lists it: a replaced order is closed (#74)
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

    FIX50SP2::NewOrderSingle makeNewOrderSingle(const std::string &clOrdId, const std::string &symbol, char side,
                                                char ordType, double price, double qty, char tif = FIX::TimeInForce_DAY)
    {
        FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
        FIX50SP2::NewOrderSingle msg;
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
    FIX50SP2::NewOrderMultileg makeFxSwap(const std::string &clOrdId, const std::string &nearDate = "20261015",
                                          const std::string &farDate = "20261115")
    {
        FIX::UtcTimeStamp now = FIX::UtcTimeStamp::now();
        FIX50SP2::NewOrderMultileg msg;
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(FIX::Side_BUY));
        msg.set(FIX::TransactTime(now));
        msg.set(FIX::OrdType(FIX::OrdType_FOREX_SWAP));
        msg.set(FIX::Symbol("aaa"));
        msg.set(FIX::OrderQty(1000000));
        msg.set(FIX::Currency("USD"));

        FIX50SP2::NewOrderMultileg::NoLegs nearLeg;
        nearLeg.set(FIX::LegSymbol("aaa"));
        nearLeg.set(FIX::LegSide(FIX::Side_BUY));
        nearLeg.set(FIX::LegPrice(1.2650));
        nearLeg.set(FIX::LegSettlDate(nearDate));
        msg.addGroup(nearLeg);

        FIX50SP2::NewOrderMultileg::NoLegs farLeg;
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

    FIX50SP2::OrderCancelRequest makeCancelRequest(const std::string &origClOrdId, const std::string &clOrdId)
    {
        FIX50SP2::OrderCancelRequest msg;
        msg.set(FIX::OrigClOrdID(origClOrdId));
        msg.set(FIX::ClOrdID(clOrdId));
        msg.set(FIX::Side(FIX::Side_BUY));
        msg.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
        msg.set(FIX::Symbol("aaa"));
        return msg;
    }

    /// Hands the gateway the engine's refusal of sid's replacement clOrdId of the order origClOrdId, or of its new order
    /// clOrdId when origClOrdId is empty, as the engine names them, under the session's key (#398)
    void engineRefuses(const FIX::SessionID &sid, const std::string &clOrdId, const std::string &origClOrdId = "")
    {
        const std::string source = FixGateway::makeSourceString(sid);
        Queues::OrderRejectEvent refusal;
        refusal.replacement_ = !origClOrdId.empty();
        refusal.clOrderId_ = FixGateway::sessionClOrdId(source, clOrdId);
        if (refusal.replacement_)
        {
            refusal.origClOrderId_ = FixGateway::sessionClOrdId(source, origClOrdId);
        }
        refusal.symbol_ = "aaa";
        refusal.side_ = BUY_SIDE;
        refusal.orderQty_ = 70;
        refusal.refusal_ = CancelRejectEvent::OTHER;
        refusal.reason_ = refusal.replacement_ ? "Replace refused" : "Order refused";
        gateway_->sendOrderReject(refusal, source);
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
    FIX50SP2::OrderCancelRequest cancelMsg;
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
    FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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

    FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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

    FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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

    FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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

    FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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
const FIX::SessionID ACCEPTOR_SID("FIXT.1.1", "ORDER_PROCESSOR", "CLIENT_A");

FIXT11::Logon makeLogon(const std::string &username, const std::string &password)
{
    FIXT11::Logon logon(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER), FIX::HeartBtInt(30),
                        FIX::DefaultApplVerID(FIX::ApplVerID_FIX50_SP2));
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
        gateway_->fromAdmin(makeLogon("client_a", "secret"), FIX::SessionID("FIXT.1.1", "ORDER_PROCESSOR", "CLIENT_Z")),
        FIX::RejectLogon);
    // Only a Logon is checked: the session's other admin messages carry no credentials
    EXPECT_NO_THROW(gateway_->fromAdmin(FIXT11::Heartbeat(), ACCEPTOR_SID));
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
    FIX50SP2::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("SWAP-001"));
    msg.set(FIX::Side(FIX::Side_BUY)); // near-leg side
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(FIX::OrdType_FOREX_SWAP));
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(1000000));
    msg.set(FIX::Currency("USD"));

    // Near leg: BUY at 1.2650, settl T+2
    FIX50SP2::NewOrderMultileg::NoLegs nearLeg;
    nearLeg.set(FIX::LegSymbol("aaa"));
    nearLeg.set(FIX::LegSide(FIX::Side_BUY));
    nearLeg.set(FIX::LegPrice(1.2650));
    nearLeg.set(FIX::LegSettlDate("20261015"));
    msg.addGroup(nearLeg);

    // Far leg: SELL at 1.2680, settl T+92
    FIX50SP2::NewOrderMultileg::NoLegs farLeg;
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
    FIX50SP2::NewOrderMultileg msg;
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
    FIX50SP2::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("MLEG-1"));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(FIX::OrdType_FOREX_SWAP));
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(100));

    // Only one leg — should be rejected
    FIX50SP2::NewOrderMultileg::NoLegs oneLeg;
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
    FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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
        FIX50SP2::NewOrderMultileg msg = makeFxSwap("SWAP-QTY-" + std::to_string(++n));
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
            FIX50SP2::NewOrderMultileg msg = makeFxSwap("SWAP-PX-" + std::to_string(++n));
            FIX50SP2::NewOrderMultileg::NoLegs group;
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
        FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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
        FIX50SP2::OrderCancelReplaceRequest replaceMsg;
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
FIX50SP2::OrderCancelReplaceRequest makeStopReplace(const std::string &origClOrdId, const std::string &clOrdId)
{
    FIX50SP2::OrderCancelReplaceRequest msg;
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
FIX50SP2::OrderCancelReplaceRequest makeLimitReplace(const std::string &origClOrdId, const std::string &clOrdId)
{
    FIX50SP2::OrderCancelReplaceRequest msg;
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

TEST_F(FixGatewayInboundTest, ReplaceRequest_ASessionsNewestRefusedReplacementsAreRemembered)
{
    // A replace that names a refused replacement is decided against the order it was to replace (#398), for each
    // session's newest REFUSALS_KEPT refusals: one more forgets the session's oldest, which is then an unknown order, as
    // before #398. Another session's refusals forget none of this session's.
    OrderEntry *x = saveOrderOf(TEST_SID, "ORD-X");
    ASSERT_NE(nullptr, x);
    // Remembered, though there is no session to send the refusals to
    for (size_t i = 0; i <= FixGateway::REFUSALS_KEPT; ++i)
    {
        engineRefuses(TEST_SID, "RPL-" + std::to_string(i), "ORD-X");
    }
    for (size_t i = 0; i <= FixGateway::REFUSALS_KEPT; ++i)
    {
        engineRefuses(OTHER_SID, "RPL-" + std::to_string(i), "ORD-X");
    }
    std::vector<IdT> replaced;
    std::vector<std::unique_ptr<OrderEntry>> replacements; // never reach an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .Times(2)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replaced.push_back(evt.id_);
                replacements.emplace_back(evt.replacementOrder_);
            }));
    gateway_->onMessage(makeLimitReplace("RPL-0", "NEW-0"), TEST_SID); // forgotten
    gateway_->onMessage(makeLimitReplace("RPL-1", "NEW-1"), TEST_SID); // the oldest remembered
    gateway_->onMessage(makeLimitReplace("RPL-" + std::to_string(FixGateway::REFUSALS_KEPT), "NEW-2"), TEST_SID);
    ASSERT_EQ(2u, replaced.size());
    const std::string xKey = FixGateway::sessionClOrdId(FixGateway::makeSourceString(TEST_SID), "ORD-X");
    for (size_t i = 0; i < replaced.size(); ++i)
    {
        EXPECT_EQ(x->orderId_, replaced[i]);
        const RawDataEntry &origClOrd = replacements[i]->origClOrderId_.get();
        EXPECT_EQ(xKey, std::string(origClOrd.data_, origClOrd.length_));
    }
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_ARefusalCountsAmongTheNewestFromWhenItWasLastRefused)
{
    // A ClOrdID refused as a replacement, then for a new order, which forgets it, and then as a replacement again
    // counts among the session's newest REFUSALS_KEPT from that last refusal (#398): REFUSALS_KEPT - 1 refusals after
    // it, it is still remembered
    OrderEntry *x = saveOrderOf(TEST_SID, "ORD-X");
    ASSERT_NE(nullptr, x);
    engineRefuses(TEST_SID, "RPL-Y", "ORD-X");
    engineRefuses(TEST_SID, "RPL-Y");
    engineRefuses(TEST_SID, "RPL-Y", "ORD-X");
    for (size_t i = 1; i < FixGateway::REFUSALS_KEPT; ++i)
    {
        engineRefuses(TEST_SID, "RPL-" + std::to_string(i), "ORD-X");
    }
    std::vector<IdT> replaced;
    std::unique_ptr<OrderEntry> replacement; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replaced.push_back(evt.id_);
                replacement.reset(evt.replacementOrder_);
            }));
    gateway_->onMessage(makeLimitReplace("RPL-Y", "NEW-Y"), TEST_SID);
    ASSERT_EQ(1u, replaced.size());
    EXPECT_EQ(x->orderId_, replaced[0]);
}

TEST_F(FixGatewayInboundTest, ReplaceRequest_ARefusalForAClOrdIdInUseTakesNoPlaceAmongTheNewest)
{
    // The gateway's refusal of a replace whose ClOrdID is in use (102=6, #403) isn't remembered (#398): that ClOrdID
    // names another order. Remembered, it would take a place among the session's newest REFUSALS_KEPT, and the oldest
    // refusal would be forgotten one refusal early. A request that names the ClOrdID finds the order that has it first,
    // so the place is what shows it.
    OrderEntry *x = saveOrderOf(TEST_SID, "ORD-X");
    ASSERT_NE(nullptr, x);
    ASSERT_NE(nullptr, saveOrderOf(TEST_SID, "ORD-P"));
    std::vector<IdT> replaced;
    std::unique_ptr<OrderEntry> replacement; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replaced.push_back(evt.id_);
                replacement.reset(evt.replacementOrder_);
            }));
    engineRefuses(TEST_SID, "RPL-0", "ORD-X");
    gateway_->onMessage(makeLimitReplace("ORD-X", "ORD-P"), TEST_SID); // refused: ORD-P is in use
    for (size_t i = 1; i < FixGateway::REFUSALS_KEPT; ++i)
    {
        engineRefuses(TEST_SID, "RPL-" + std::to_string(i), "ORD-X");
    }
    gateway_->onMessage(makeLimitReplace("RPL-0", "NEW-0"), TEST_SID); // the oldest of REFUSALS_KEPT
    ASSERT_EQ(1u, replaced.size());
    EXPECT_EQ(x->orderId_, replaced[0]);
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
    // The session hands fromApp() a plain FIX::Message, as here. FIX50SP2::MessageCracker cast it to
    // NewOrderSingle through a temporary FIX50SP2::Message, which UBSan reports as a member call on an object
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

TEST_F(FixGatewayInboundTest, FromApp_UnsupportedTypeIsAnsweredByTheGateway)
{
    // A type the gateway does not handle is answered with its own BusinessMessageReject, not QuickFIX's, which a
    // counterparty that validates refuses on a FIXT session (FixtSessionTest); it used to throw UnsupportedMessageType
    // for QuickFIX to answer. No session is logged on here, so the reject cannot be sent; that is logged, not thrown.
    FIX50SP2::News news;
    news.set(FIX::Headline("not handled"));
    EXPECT_NO_THROW(gateway_->fromApp(news, TEST_SID));
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

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&reject, *order);

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

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);

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
    // A held stop released by a trade at its stop price (#223) is reported with ExecType L, triggered or activated by
    // system, and the order NEW (#236). The gateway sent ExecType NEW (0) for an execution type it didn't know. L is
    // FIX 5.0's: FIX 4.4's ExecType ends at I, which is why the gateway speaks FIX 5.0 SP2 (#377)
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    auto release = makeReport<ExecutionEntry>(TRIGGERED_EXECTYPE, NEW_ORDSTATUS);

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&release, *order);

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

/// WorkingIndicator (636) of the report the gateway builds for exec on order, or "(not set)"
std::string workingIndicatorOf(const ExecutionEntry &exec, const OrderEntry &order)
{
    const FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&exec, order);
    return report.isSetField(FIX::FIELD::WorkingIndicator) ? report.getField(FIX::FIELD::WorkingIndicator)
                                                           : std::string("(not set)");
}

TEST_F(FixGatewayInboundTest, ExecutionReport_WorkingIndicatorSaysWhetherANewOrderIsInTheBook)
{
    // EP284's E.1.f (#387): a New order held off the book, a stop or stop limit whose stop hasn't been reached, is
    // acknowledged with WorkingIndicator (636) N, and so restated at a business day's start; its release (150=L) says Y.
    // A New order booked at once says Y, which E.1.f's state diagram takes as it takes no 636. A report of any other
    // status has none.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    const auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    const auto restated = makeReport<ExecutionEntry>(RESTATED_EXECTYPE, NEW_ORDSTATUS);
    const auto release = makeReport<ExecutionEntry>(TRIGGERED_EXECTYPE, NEW_ORDSTATUS);
    order->ordType_ = LIMIT_ORDERTYPE;
    EXPECT_EQ("Y", workingIndicatorOf(ack, *order));
    order->ordType_ = MARKET_ORDERTYPE;
    EXPECT_EQ("Y", workingIndicatorOf(ack, *order));
    for (const OrderType held : { STOP_ORDERTYPE, STOPLIMIT_ORDERTYPE })
    {
        SCOPED_TRACE(held);
        order->ordType_ = held;
        order->stopTriggered_ = false;
        EXPECT_EQ("N", workingIndicatorOf(ack, *order));
        EXPECT_EQ("N", workingIndicatorOf(restated, *order));
        order->stopTriggered_ = true; // as its release leaves it
        EXPECT_EQ("Y", workingIndicatorOf(release, *order));
        EXPECT_EQ("Y", workingIndicatorOf(restated, *order));
    }
    order->stopTriggered_ = false;
    order->ordType_ = STOP_ORDERTYPE;
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    trade.lastQty_ = 40;
    trade.lastPx_ = 10.5;
    EXPECT_EQ("(not set)", workingIndicatorOf(trade, *order));
    auto reject = makeReport<RejectExecEntry>(REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    reject.rejectReason_ = "There is no market for this instrument!";
    EXPECT_EQ("(not set)", workingIndicatorOf(reject, *order));
    EXPECT_EQ("(not set)", workingIndicatorOf(makeReport<ExecutionEntry>(CANCEL_EXECTYPE, CANCELED_ORDSTATUS), *order));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_AReplacementsWorkingIndicatorFollowsWhatItIs)
{
    // A replace is acknowledged with the replacement's report (150=5), New if nothing has filled, which says whether the
    // replacement is in the book (#387). A held stop's replacement that is a stop order too is held (N); one that is a
    // limit order is booked at once (Y); a released stop's replacement that is a stop order stays released (Y), as
    // Processor::process(onReplace) carries stopTriggered_ over to a stop replacement alone (#223).
    auto replacement = createCorrectOrder(instrumentId1_);
    assignClOrderId(replacement.get());
    auto ack = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, NEW_ORDSTATUS);
    ack.origOrderId_ = IdT(6, 1);
    replacement->ordType_ = STOP_ORDERTYPE;
    replacement->stopTriggered_ = false;
    EXPECT_EQ("N", workingIndicatorOf(ack, *replacement));
    replacement->ordType_ = LIMIT_ORDERTYPE; // a held stop replaced by a limit order: never released, so not held
    EXPECT_EQ("Y", workingIndicatorOf(ack, *replacement));
    replacement->ordType_ = STOPLIMIT_ORDERTYPE;
    replacement->stopTriggered_ = true;
    EXPECT_EQ("Y", workingIndicatorOf(ack, *replacement));
    auto partFilled = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, PARTFILL_ORDSTATUS);
    EXPECT_EQ("(not set)", workingIndicatorOf(partFilled, *replacement));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_AGtdOrdersReportCarriesItsExpireTime)
{
    // ExpireTime (126), to the millisecond, on a GTD order's reports, and on no other's (#240)
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->tif_ = GTD_TIF;
    order->expireTime_ = 1792098000250ULL; // 2026-10-15 21:00:00.250 UTC
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);
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
    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&restated, *order);
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
    // DayOrderQty (424), DayCumQty (425) and DayAvgPx (426), which FIX gives a GT order on the days after its first
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
    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);
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
    // FIX answers a cancel request with the request's own ClOrdID in 11, and the cancelled order's in 41 (#98)
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    const auto &clOrd = order->clOrderId_.get();
    const std::string orderClOrdId(clOrd.data_, clOrd.length_);
    auto ack = makeReport<ExecutionEntry>(CANCEL_EXECTYPE, CANCELED_ORDSTATUS);

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order, "CXL-7");
    FIX::ClOrdID clOrdId;
    report.get(clOrdId);
    EXPECT_EQ("CXL-7", clOrdId.getValue());
    FIX::OrigClOrdID origClOrdId;
    report.get(origClOrdId);
    EXPECT_EQ(orderClOrdId, origClOrdId.getValue());

    // A cancel no request asked for, as of a market order the engine can't fill, has no request ClOrdID: the order's
    // own stays in 11
    FIX50SP2::ExecutionReport internal = FixGateway::buildExecutionReport(&ack, *order);
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

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);

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

TEST_F(FixGatewayInboundTest, ExecutionReport_CarriesTheOrdersAvgPx)
{
    // AvgPx (6): FIX 4.4's ExecutionReport required it, so its type did too, and FIX 5.0 SP2's doesn't. The gateway
    // still sends it (#377).
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->avgPx_ = 10.25;
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);

    ASSERT_TRUE(report.isSetField(FIX::FIELD::AvgPx));
    FIX::AvgPx avgPx;
    report.get(avgPx);
    EXPECT_DOUBLE_EQ(10.25, avgPx.getValue());
}

TEST_F(FixGatewayInboundTest, ExecutionReport_CarriesTheOrdersOrderQty)
{
    // OrderQty (38) on every report of a stored order (#384), as on every report of the Order State Changes. Not
    // required in FIX 5.0 SP2, it was sent only on the rejects of orders the gateway never stored. Without it, a
    // closed order's quantity can't be worked out from the report: here 40 of 100 traded, and the rest was cancelled.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->orderQty_ = 100;
    order->cumQty_ = 40;
    order->leavesQty_ = 0;
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    auto cancel = makeReport<ExecutionEntry>(CANCEL_EXECTYPE, CANCELED_ORDSTATUS);
    auto reject = makeReport<RejectExecEntry>(REJECT_EXECTYPE, REJECTED_ORDSTATUS);
    auto expiry = makeReport<ExecutionEntry>(EXPIRED_EXECTYPE, EXPIRED_ORDSTATUS);
    const std::vector<const ExecutionEntry *> reports = { &ack, &trade, &cancel, &reject, &expiry };
    for (const ExecutionEntry *exec : reports)
    {
        FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(exec, *order);
        FIX::OrderQty orderQty;
        ASSERT_TRUE(report.isSet(orderQty)) << "ExecType " << report.getField(FIX::FIELD::ExecType);
        report.get(orderQty);
        EXPECT_DOUBLE_EQ(100.0, orderQty.getValue()) << "ExecType " << report.getField(FIX::FIELD::ExecType);
    }

    // Written as a whole number, the same bytes QuickFIX writes for the double, up to the largest quantity
    for (const QuantityT qty : { QuantityT{ 1 }, QuantityT{ 77 }, std::numeric_limits<QuantityT>::max() })
    {
        order->orderQty_ = qty;
        FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&ack, *order);
        EXPECT_EQ(FIX::DoubleConvertor::convert(static_cast<double>(qty)), report.getField(FIX::FIELD::OrderQty));
    }

    // A replacement's report carries the replacement's quantity, which the replace may have changed (#74)
    auto replacement = createCorrectOrder(instrumentId1_);
    assignClOrderId(replacement.get());
    replacement->orderQty_ = 150;
    auto replace = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, PARTFILL_ORDSTATUS);
    FIX50SP2::ExecutionReport replaced = FixGateway::buildExecutionReport(&replace, *replacement);
    FIX::OrderQty replacedQty;
    ASSERT_TRUE(replaced.isSet(replacedQty));
    replaced.get(replacedQty);
    EXPECT_DOUBLE_EQ(150.0, replacedQty.getValue());
}

/// The bytes QuickFIX writes for a quantity as the double its QTY fields take (#396)
std::string qtyAsDouble(QuantityT qty)
{
    return FIX::DoubleConvertor::convert(static_cast<double>(qty));
}

TEST_F(FixGatewayInboundTest, ExecutionReport_EveryQuantityIsTheBytesQuickFixWritesForTheDouble)
{
    // Each quantity a report carries is written as the whole number it is (#396), not through the double QuickFIX's QTY
    // fields take, with the bytes that double would give. A GT order's trade on a later day carries six, and a swap
    // leg's trade LegLastQty (1418) as well: each a value of its own, so that none is written in another's place, and
    // among them 0 and the largest quantity, to which those below 1 wrap round. Day figures go out only while DayCumQty
    // is below CumQty.
    for (const QuantityT qty : { QuantityT{ 1 }, QuantityT{ 77 }, std::numeric_limits<QuantityT>::max() })
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        order->tif_ = GTC_TIF;
        order->cumQty_ = qty;
        order->dayCumQty_ = qty - 1;
        order->orderQty_ = qty - 2;
        order->leavesQty_ = qty - 3;
        order->dayOrderQty_ = qty - 4;
        auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
        trade.lastQty_ = qty - 5;
        const FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);
        const std::vector<std::pair<int, QuantityT>> expected = {
            { FIX::FIELD::CumQty, order->cumQty_ },           { FIX::FIELD::DayCumQty, order->dayCumQty_ },
            { FIX::FIELD::OrderQty, order->orderQty_ },       { FIX::FIELD::LeavesQty, order->leavesQty_ },
            { FIX::FIELD::DayOrderQty, order->dayOrderQty_ }, { FIX::FIELD::LastQty, trade.lastQty_ }
        };
        for (const auto &[tag, value] : expected)
        {
            EXPECT_EQ(qtyAsDouble(value), report.getField(tag)) << "tag " << tag << ", " << value;
        }

        trade.execLegType_ = NEAR_LEG;
        const FIX50SP2::ExecutionReport legReport = FixGateway::buildExecutionReport(&trade, *order);
        ASSERT_EQ(1u, legReport.groupCount(FIX::FIELD::NoLegs));
        FIX50SP2::ExecutionReport::NoLegs leg;
        legReport.getGroup(1, leg);
        EXPECT_EQ(qtyAsDouble(trade.lastQty_), leg.getField(FIX::FIELD::LegLastQty)) << trade.lastQty_;
    }
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

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);
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

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&correct, *order);

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

    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&replace, *order);

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
    FIX50SP2::ExecutionReport replaced = FixGateway::buildExecutionReport(&replace, *order);
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
    // replaced order's isn't sent, since 39=5 has been out of use since FIX 4.4 (#74).
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
    FIX50SP2::ExecutionReport report = FixGateway::buildOrderReject(makeOrderReject(false), 41);

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
    FIX::AvgPx avgPx; // which FIX 5.0 SP2 doesn't require, and FIX 4.4 did (#377)
    report.get(avgPx);
    EXPECT_DOUBLE_EQ(0, avgPx.getValue());
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

    FIX50SP2::OrderCancelReject reject = FixGateway::buildReplaceReject(shown, "7", FIX::OrdStatus_NEW);
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
    FIX50SP2::NewOrderSingle order;
    order.set(FIX::ClOrdID("QTY-1"));
    order.set(FIX::Side(FIX::Side_SELL));
    order.set(FIX::Symbol("aaa"));
    order.set(FIX::OrderQty(-5));
    const std::string why = "Order refused: OrderQty (38) must be a whole number from 1 to 4294967295";
    FIX50SP2::ExecutionReport report =
        FixGateway::buildOrderRefusal(order, FIX::OrdRejReason_INCORRECT_QUANTITY, why, 43);

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

TEST(FixOrderRejectTest, AnOrderRejectsQuantitiesAreTheBytesQuickFixWritesForTheDouble)
{
    // As every report's are (#396): OrderQty (38) as the engine refused it, and LeavesQty and CumQty of 0, written as
    // the whole numbers they are, with the bytes QuickFIX writes for the doubles its QTY fields take
    for (const QuantityT qty : { QuantityT{ 1 }, QuantityT{ 77 }, std::numeric_limits<QuantityT>::max() })
    {
        Queues::OrderRejectEvent refused = makeOrderReject(false);
        refused.orderQty_ = qty;
        const FIX50SP2::ExecutionReport reject = FixGateway::buildOrderReject(refused, 41);
        EXPECT_EQ(qtyAsDouble(qty), reject.getField(FIX::FIELD::OrderQty)) << qty;
        EXPECT_EQ(qtyAsDouble(0), reject.getField(FIX::FIELD::LeavesQty));
        EXPECT_EQ(qtyAsDouble(0), reject.getField(FIX::FIELD::CumQty));
    }
}

TEST(FixOrderRejectTest, DuplicateReplacementGetsAnOrderCancelReject)
{
    // A cancel/replace request is refused with an OrderCancelReject (35=9) that answers it (434=2), says why
    // (CxlRejReason 6, duplicate ClOrdID; Text 58), and describes the order it was to replace
    FIX50SP2::OrderCancelReject reject =
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
    FIX50SP2::OrderCancelReject reject = FixGateway::buildUnknownReplaceReject("RPL-2", "ORD-9");

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
    FIX50SP2::OrderCancelReject reject = FixGateway::buildCancelReject(evnt, "7", "ORD-1", "ORD-1");

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
    FIX50SP2::OrderCancelReject reject = FixGateway::buildEngineCancelReject(evnt, "7", "ORD-1");
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
    FIX50SP2::OrderCancelReject reject = FixGateway::buildCancelReject(evnt, "NONE", "CXL-2", "ORD-9");

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

        FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&trade, *order);

        ASSERT_EQ(1u, report.groupCount(FIX::FIELD::NoLegs)) << "leg " << leg;
        FIX50SP2::ExecutionReport::NoLegs group;
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
    FIX50SP2::ExecutionReport report = FixGateway::buildExecutionReport(&plain, *order);
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

// =============================================================================
// FIXT.1.1 sessions, with FIX 5.0 SP2's messages (#377)
// =============================================================================

/// A field's value, or "(not set)", so that a field that isn't there fails a comparison rather than throws
std::string fieldOf(const FIX::FieldMap &fields, int tag)
{
    return fields.isSetField(tag) ? fields.getField(tag) : "(not set)";
}

/// The tests above call the gateway as QuickFIX would. These hand CLIENT_A's messages to a QuickFIX session made from
/// config/fix.cfg, as the server's acceptor makes one, with the gateway as its application, and read what the session
/// sends as a counterparty reads it: parsed with the session's own dictionaries, this tree's config/spec, and validated
/// as QuickFIX validates by default. The session never ends, where the cfg's ends at 17:00 New York, so that no test
/// sees it roll.
class FixtSessionTest : public FixGatewayInboundTest, public FIX::Responder
{
protected:
    void SetUp() override
    {
        FixGatewayInboundTest::SetUp();
        const FIX::SessionSettings cfg((test::sourceRoot() / "config" / "fix.cfg").string());
        FIX::Dictionary settings;
        for (const auto &[key, value] : cfg.get())
        {
            // The session's times: a session that never ends has none
            if ((FIX::string_toUpper(FIX::START_TIME) != key) && (FIX::string_toUpper(FIX::END_TIME) != key))
            {
                settings.setString(key, value);
            }
        }
        settings.setBool(FIX::NON_STOP_SESSION, true);
        for (const char *dictionary : { FIX::TRANSPORT_DATA_DICTIONARY, FIX::APP_DATA_DICTIONARY })
        {
            ASSERT_TRUE(settings.has(dictionary)) << "config/fix.cfg names no " << dictionary;
            // The file of its name in this tree's config/spec, as fix_client.py's write_cfg() finds it
            const std::filesystem::path file = std::filesystem::path(settings.getString(dictionary)).filename();
            settings.setString(dictionary, (test::sourceRoot() / "config" / "spec" / file).string());
        }
        settings_ = settings;
        sid_ = FIX::SessionID(settings.getString(FIX::BEGINSTRING), settings.getString(FIX::SENDERCOMPID), "CLIENT_A");
        // A request that waits for its order is ended by the order's answer, then, never by the minute's timeout, unless
        // a test makes it short (#380)
        makeSession(std::chrono::seconds(60));
    }

    /// The gateway, whose requests wait up to answerWait for the order they name (#380), and CLIENT_A's session, of
    /// which it is the application, made in place of those made before
    void makeSession(std::chrono::milliseconds answerWait)
    {
        if (nullptr != session_)
        {
            factory_->destroy(session_);
            session_ = nullptr;
        }
        factory_.reset();
        gateway_ =
            std::make_unique<FixGateway>(mockInQueues_.get(), WideDataStorage::instance(), OrderStorage::instance(),
                                         defaultClearingId_, defaultAccountId_, answerWait);
        factory_ = std::make_unique<FIX::SessionFactory>(*gateway_, stores_, nullptr);
        session_ = factory_->create(sid_, settings_);
        session_->setResponder(this);
        sent_.clear();
        nextSeqNum_ = 1;
    }

    void TearDown() override
    {
        if (nullptr != session_)
        {
            factory_->destroy(session_); // before the gateway, its application
        }
        factory_.reset();
        FixGatewayInboundTest::TearDown();
    }

    // FIX::Responder: the connection to CLIENT_A
    bool send(const std::string &raw) override
    {
        sent_.push_back(raw);
        return true;
    }
    void disconnect() override
    {
        disconnected_ = true;
    }

    /// Hands the session msg as CLIENT_A sends it: from CLIENT_A, with its next MsgSeqNum
    void receive(FIX::Message msg)
    {
        FIX::Header &header = msg.getHeader();
        header.setField(FIX::BeginString(sid_.getBeginString().getValue()));
        header.setField(FIX::SenderCompID(sid_.getTargetCompID().getValue()));
        header.setField(FIX::TargetCompID(sid_.getSenderCompID().getValue()));
        header.setField(FIX::MsgSeqNum(nextSeqNum_++));
        header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
        session_->next(msg.toString(), FIX::UtcTimeStamp::now());
    }

    /// CLIENT_A logs on, as config/fix.cfg's clients do; what the session answers is forgotten
    void logOn()
    {
        receive(makeLogon("", ""));
        ASSERT_TRUE(session_->isLoggedOn()) << sentText();
        sent_.clear();
    }

    const FIX::DataDictionary &transportDictionary() const
    {
        return session_->getDataDictionaryProvider().getSessionDataDictionary(sid_.getBeginString());
    }
    const FIX::DataDictionary &applicationDictionary() const
    {
        return session_->getDataDictionaryProvider().getApplicationDataDictionary(
            FIX::ApplVerID(FIX::ApplVerID_FIX50_SP2));
    }

    /// A message the session sent, as a counterparty parses it
    FIX::Message parsed(const std::string &raw) const
    {
        return FIX::Message(raw, transportDictionary(), applicationDictionary(), true);
    }

    /// Why a counterparty that validates, as QuickFIX does by default, refuses raw, a message the session sent: empty
    /// if it doesn't. QuickFIX answers such a message with a Reject (35=3), and never hands it to its application.
    std::string refusalOf(const std::string &raw) const
    {
        try
        {
            const FIX::Message msg = parsed(raw);
            if (msg.isApp())
            {
                FIX::DataDictionary::validate(msg, &transportDictionary(), &applicationDictionary());
            }
            else
            {
                transportDictionary().validate(msg);
            }
            return "";
        }
        catch (const std::exception &ex)
        {
            return ex.what();
        }
    }

    /// What the session sent of MsgType type, parsed
    std::vector<FIX::Message> sentOfType(const std::string &type) const
    {
        std::vector<FIX::Message> found;
        for (const std::string &raw : sent_)
        {
            FIX::Message msg = parsed(raw);
            if (type == msg.getHeader().getField(FIX::FIELD::MsgType))
            {
                found.push_back(msg);
            }
        }
        return found;
    }

    /// Every message the session sent, one a line with '|' for SOH, for a failure to show
    std::string sentText() const
    {
        std::string text;
        for (std::string raw : sent_)
        {
            std::replace(raw.begin(), raw.end(), '\x01', '|');
            text += raw + "\n";
        }
        return text;
    }

    /// Fails for each message the session sent that a counterparty would refuse
    void expectEverythingSentIsTaken() const
    {
        for (const std::string &raw : sent_)
        {
            EXPECT_EQ("", refusalOf(raw)) << raw;
        }
    }

    FIX::MemoryStoreFactory stores_;
    std::unique_ptr<FIX::SessionFactory> factory_;
    FIX::Session *session_ = nullptr;
    FIX::Dictionary settings_; // config/fix.cfg's, for a session that never ends
    FIX::SessionID sid_;
    std::vector<std::string> sent_; // what the session sent, raw
    bool disconnected_ = false;
    int nextSeqNum_ = 1;
};

TEST_F(FixtSessionTest, TheSettingsMakeAFixtSessionWithFix50Sp2)
{
    // config/fix.cfg's sessions are FIXT.1.1's, with FIX 5.0 SP2's messages, whose ExecType has the L a stop's release
    // is reported with (#236); FIX 4.4's ends at I
    EXPECT_TRUE(sid_.isFIXT()) << sid_.toString();
    EXPECT_EQ(FIX::ApplVerID_FIX50_SP2, session_->getSenderDefaultApplVerID());
    EXPECT_EQ(FIX::BeginString_FIXT11, transportDictionary().getVersion());
    // QuickFIX names a dictionary by its major and minor version alone
    EXPECT_EQ("FIX.5.0", applicationDictionary().getVersion());
    const FIX::SessionSettings cfg((test::sourceRoot() / "config" / "fix.cfg").string());
    EXPECT_EQ("FIX50SP2.xml", std::filesystem::path(cfg.get().getString(FIX::APP_DATA_DICTIONARY)).filename());
    EXPECT_TRUE(applicationDictionary().isFieldValue(FIX::FIELD::ExecType,
                                                     std::string(1, FIX::ExecType_TRIGGERED_OR_ACTIVATED_BY_SYSTEM)));
    // No other version has a dictionary, so the session would check a message of one against nothing: the gateway
    // refuses them (below)
    EXPECT_EQ("", session_->getDataDictionaryProvider()
                      .getApplicationDataDictionary(FIX::ApplVerID(FIX::ApplVerID_FIX50))
                      .getVersion());
}

TEST_F(FixtSessionTest, ALogonIsAnsweredWithFix50Sp2AsTheDefault)
{
    receive(makeLogon("", ""));
    ASSERT_TRUE(session_->isLoggedOn()) << sentText();
    const std::vector<FIX::Message> logons = sentOfType(FIX::MsgType_Logon);
    ASSERT_EQ(1u, logons.size()) << sentText();
    EXPECT_EQ(FIX::BeginString_FIXT11, fieldOf(logons[0].getHeader(), FIX::FIELD::BeginString));
    EXPECT_EQ(FIX::ApplVerID_FIX50_SP2, fieldOf(logons[0], FIX::FIELD::DefaultApplVerID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, ALogonOfAnotherVersionIsRefused)
{
    // The session checks a counterparty's messages against the dictionary of the version its Logon names in
    // DefaultApplVerID (1137), and has none but FIX 5.0 SP2's. FIX 5.0's value, FIX 4.4's, and FIX 5.0 SP2's name
    // where its value belongs: each Logon is answered with a Logout saying why, and the connection closed.
    for (const char *version : { FIX::ApplVerID_FIX50, FIX::ApplVerID_FIX44, "FIX.5.0SP2" })
    {
        sent_.clear();
        disconnected_ = false;
        nextSeqNum_ = 1;
        session_->setResponder(this); // a new connection
        FIXT11::Logon logon = makeLogon("", "");
        logon.set(FIX::DefaultApplVerID(version));
        receive(logon);
        EXPECT_FALSE(session_->isLoggedOn()) << version;
        EXPECT_TRUE(disconnected_) << version;
        const std::vector<FIX::Message> logouts = sentOfType(FIX::MsgType_Logout);
        ASSERT_EQ(1u, logouts.size()) << version << ": " << sentText();
        EXPECT_THAT(fieldOf(logouts[0], FIX::FIELD::Text), testing::HasSubstr("DefaultApplVerID (1137) must be 9"))
            << version;
        expectEverythingSentIsTaken();
    }
}

TEST_F(FixtSessionTest, AMessageOfAnotherVersionIsRefused)
{
    // A message may name its own version, in ApplVerID (1128), which the session would check it against no dictionary
    // for. Refused with a Reject naming 1128, SessionRejectReason 18 (invalid or unsupported application version), and
    // not queued; the session goes on.
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    auto order = makeNewOrderSingle("ORD-FIX50", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    order.getHeader().setField(FIX::ApplVerID(FIX::ApplVerID_FIX50));
    receive(order);
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_Reject);
    ASSERT_EQ(1u, rejects.size()) << sentText();
    EXPECT_EQ("2", fieldOf(rejects[0], FIX::FIELD::RefSeqNum));
    EXPECT_EQ(FIX::MsgType_NewOrderSingle, fieldOf(rejects[0], FIX::FIELD::RefMsgType));
    EXPECT_EQ("1128", fieldOf(rejects[0], FIX::FIELD::RefTagID));
    EXPECT_EQ(std::to_string(FIX::SessionRejectReason_INVALID_UNSUPPORTED_APPLICATION_VERSION),
              fieldOf(rejects[0], FIX::FIELD::SessionRejectReason));
    expectEverythingSentIsTaken();
    EXPECT_EQ(3u, session_->getExpectedTargetNum());
}

TEST_F(FixtSessionTest, AMessageOfFix50Sp2OrOfNoVersionIsTaken)
{
    // QuickFIX's FIX 5.0 SP2 messages name it, 1128=9; one that names none is of the Logon's version
    ASSERT_NO_FATAL_FAILURE(logOn());
    std::vector<OrderEntry *> queued;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .Times(2)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                queued.push_back(evt.order_);
            }));
    auto named = makeNewOrderSingle("ORD-FIX50SP2", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    ASSERT_EQ(FIX::ApplVerID_FIX50_SP2, fieldOf(named.getHeader(), FIX::FIELD::ApplVerID));
    receive(named);
    auto unnamed = makeNewOrderSingle("ORD-DEFAULT", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    unnamed.getHeader().removeField(FIX::FIELD::ApplVerID);
    receive(unnamed);
    EXPECT_TRUE(sent_.empty()) << sentText();
    for (OrderEntry *order : queued)
    {
        delete order;
    }
}

TEST_F(FixtSessionTest, AValueFix50Sp2DoesntHaveIsRefused)
{
    // The session checks each message against FIX 5.0 SP2's dictionary, which has no OrdType Z
    ASSERT_FALSE(applicationDictionary().isFieldValue(FIX::FIELD::OrdType, "Z"));
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    receive(makeNewOrderSingle("ORD-Z", "aaa", FIX::Side_BUY, 'Z', 10.25, 100));
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_Reject);
    ASSERT_EQ(1u, rejects.size()) << sentText();
    EXPECT_EQ("40", fieldOf(rejects[0], FIX::FIELD::RefTagID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AMessageTypeTheGatewayDoesntTakeIsRefusedReadably)
{
    // A BusinessMessageReject (35=j), unsupported message type (380=3). QuickFIX's own carries the session's
    // DefaultApplVerID (1137), which FIX 5.0 SP2's BusinessMessageReject doesn't have, so a counterparty that validates
    // refused it; the gateway sends its own.
    ASSERT_NO_FATAL_FAILURE(logOn());
    FIX50SP2::News news;
    news.set(FIX::Headline("not taken"));
    receive(news);
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_BusinessMessageReject);
    ASSERT_EQ(1u, rejects.size()) << sentText();
    EXPECT_EQ("2", fieldOf(rejects[0], FIX::FIELD::RefSeqNum));
    EXPECT_EQ(FIX::MsgType_News, fieldOf(rejects[0], FIX::FIELD::RefMsgType));
    EXPECT_EQ(std::to_string(FIX::BusinessRejectReason_UNSUPPORTED_MESSAGE_TYPE),
              fieldOf(rejects[0], FIX::FIELD::BusinessRejectReason));
    expectEverythingSentIsTaken();
    EXPECT_EQ(3u, session_->getExpectedTargetNum());
}

TEST_F(FixtSessionTest, ARequestWithoutAFieldTheGatewayNeedsIsRefusedReadably)
{
    // FIX 5.0 SP2 doesn't require a cancel's OrigClOrdID (41), which FIX 4.4 did, nor a new order's OrderQty (38): the
    // session lets them through, and the gateway, which needs them, refuses them with a BusinessMessageReject,
    // conditionally required field missing (380=5), that names the field, as QuickFIX's own would, without its 1137
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>())).Times(0);
    auto cancel = makeCancelRequest("ORD-1", "CXL-NO-41");
    cancel.removeField(FIX::FIELD::OrigClOrdID);
    receive(cancel);
    auto order = makeNewOrderSingle("ORD-NO-38", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.25, 100);
    order.removeField(FIX::FIELD::OrderQty);
    receive(order);
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_BusinessMessageReject);
    ASSERT_EQ(2u, rejects.size()) << sentText();
    const std::string missing = std::to_string(FIX::BusinessRejectReason_CONDITIONALLY_REQUIRED_FIELD_MISSING);
    EXPECT_EQ(missing, fieldOf(rejects[0], FIX::FIELD::BusinessRejectReason));
    EXPECT_EQ("CXL-NO-41", fieldOf(rejects[0], FIX::FIELD::BusinessRejectRefID));
    EXPECT_THAT(fieldOf(rejects[0], FIX::FIELD::Text), testing::HasSubstr("(41)"));
    EXPECT_EQ(missing, fieldOf(rejects[1], FIX::FIELD::BusinessRejectReason));
    EXPECT_THAT(fieldOf(rejects[1], FIX::FIELD::Text), testing::HasSubstr("(38)"));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, TheGatewaysOwnRefusalsAreTaken)
{
    // What the gateway refuses before the engine sees it: an order for a symbol it doesn't know (150=8), and a cancel
    // and a replace of an order the session doesn't have (35=9)
    ASSERT_NO_FATAL_FAILURE(logOn());
    receive(makeNewOrderSingle("ORD-UNKNOWN", "NONEXISTENT", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100));
    receive(makeCancelRequest("NO-SUCH", "CXL-UNKNOWN"));
    receive(makeLimitReplace("NO-SUCH", "RPL-UNKNOWN"));
    EXPECT_EQ(1u, sentOfType(FIX::MsgType_ExecutionReport).size()) << sentText();
    EXPECT_EQ(2u, sentOfType(FIX::MsgType_OrderCancelReject).size()) << sentText();
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, TheEnginesRefusalsAreTaken)
{
    // What the engine refuses, as the gateway reports it: a new order and a replacement whose ClOrdID is in use, a
    // cancel too late, and a business reject naming an order
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *order = saveOrderOf(sid_, "ORD-1");
    const std::string source = FixGateway::makeSourceString(sid_);
    gateway_->sendOrderReject(makeOrderReject(false), source);
    gateway_->sendOrderReject(makeOrderReject(true), source);
    Queues::CancelRejectEvent tooLate;
    tooLate.id_ = order->orderId_;
    tooLate.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    tooLate.ordStatus_ = FILLED_ORDSTATUS;
    tooLate.requestClOrdId_ = "CXL-LATE";
    gateway_->sendCancelReject(tooLate, source);
    gateway_->sendBusinessReject(order->orderId_, "refused");
    EXPECT_EQ(1u, sentOfType(FIX::MsgType_ExecutionReport).size()) << sentText();
    EXPECT_EQ(2u, sentOfType(FIX::MsgType_OrderCancelReject).size()) << sentText();
    EXPECT_EQ(1u, sentOfType(FIX::MsgType_BusinessMessageReject).size()) << sentText();
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, EveryKindOfExecutionReportIsTaken)
{
    // Each ExecType the gateway sends, on an order of the session's. A stop's release, 150=L (#236), is why the gateway
    // left FIX 4.4.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *order = saveOrderOf(sid_, "ORD-1");
    order->tif_ = GTC_TIF;
    size_t reports = 0;
    for (const ExecType type :
         { NEW_EXECTYPE, DFD_EXECTYPE, REJECT_EXECTYPE, EXPIRED_EXECTYPE, SUSPENDED_EXECTYPE, STATUS_EXECTYPE,
           RESTATED_EXECTYPE, PEND_CANCEL_EXECTYPE, PEND_REPLACE_EXECTYPE, TRIGGERED_EXECTYPE })
    {
        auto report = makeReport<RejectExecEntry>(type, NEW_ORDSTATUS);
        if (REJECT_EXECTYPE == type)
        {
            report.orderStatus_ = REJECTED_ORDSTATUS;
            report.rejectReason_ = "There is no market for this instrument!";
        }
        gateway_->sendExecutionReport(&report, *order);
        ++reports;
    }
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    trade.lastQty_ = 40;
    trade.lastPx_ = 10.5;
    trade.tradeDate_ = 20261016;
    gateway_->sendExecutionReport(&trade, *order);
    auto correct = makeReport<ExecCorrectExecEntry>(CORRECT_EXECTYPE, PARTFILL_ORDSTATUS);
    gateway_->sendExecutionReport(&correct, *order);
    auto cancel = makeReport<ExecutionEntry>(CANCEL_EXECTYPE, CANCELED_ORDSTATUS);
    gateway_->sendExecutionReport(&cancel, *order, "CXL-1"); // a cancel's acknowledgement (#98)
    order->origClOrderId_ = WideDataStorage::instance()->add(new RawDataEntry(STRING_RAWDATATYPE, "ORD-0", 5));
    auto replacement = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, NEW_ORDSTATUS);
    replacement.origOrderId_ = IdT(6, 1);
    gateway_->sendExecutionReport(&replacement, *order);
    reports += 4;
    EXPECT_EQ(reports, sentOfType(FIX::MsgType_ExecutionReport).size()) << sentText();
    expectEverythingSentIsTaken();
    // Each with the order's OrderQty (38), as a counterparty reads it (#384)
    for (const FIX::Message &sent : sentOfType(FIX::MsgType_ExecutionReport))
    {
        EXPECT_EQ(std::to_string(order->orderQty_), fieldOf(sent, FIX::FIELD::OrderQty))
            << "ExecType " << fieldOf(sent, FIX::FIELD::ExecType);
    }
}

TEST_F(FixtSessionTest, ASwapsTradeReportIsRefusedForItsTradeLinkIdAlone)
{
    // FIX 5.0 SP2 has LegLastQty (1418) in the report's NoLegs group, where FIX 4.4 had no such field. TradeLinkID
    // (820), which pairs a swap's two fills (#199), is a field of neither version's ExecutionReport, so a counterparty
    // that validates refuses the report for it, as it did on FIX 4.4 (#378). Nothing else in it is refused.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *order = saveOrderOf(sid_, "SWAP-1");
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->settlDate_ = 1792022400000ULL;    // 2026-10-15, midnight UTC
    order->farSettlDate_ = 1794700800000ULL; // 2026-11-15
    order->farPrice_ = 1.0872;
    auto trade = makeReport<TradeExecEntry>(TRADE_EXECTYPE, PARTFILL_ORDSTATUS);
    trade.execLegType_ = FAR_LEG;
    trade.tradeLink_ = 4242;
    trade.lastQty_ = 100;
    trade.lastPx_ = 1.0872;
    gateway_->sendExecutionReport(&trade, *order);
    ASSERT_EQ(1u, sent_.size()) << sentText();
    EXPECT_EQ("Tag not defined for this message type: 820", refusalOf(sent_[0]));
    EXPECT_EQ(1u, parsed(sent_[0]).groupCount(FIX::FIELD::NoLegs));
}

TEST_F(FixtSessionTest, ARequestForAnOrderTheEngineHasntAnsweredIsRefusedAsPendingNew)
{
    // A cancel or a replace that names an order the session sent just before waits for the engine to store or refuse
    // it (#380). If the engine still hasn't answered when the wait ends, the request is refused with the order Pending
    // New (39=A), not as unknown: the order may yet be acknowledged.
    makeSession(std::chrono::milliseconds(50));
    ASSERT_NO_FATAL_FAILURE(logOn());
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    receive(makeNewOrderSingle("ORD-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100));
    ASSERT_NE(nullptr, pushed);
    receive(makeCancelRequest("ORD-1", "CXL-1"));
    receive(makeLimitReplace("ORD-1", "RPL-1"));

    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_OrderCancelReject);
    ASSERT_EQ(2u, rejects.size()) << sentText();
    EXPECT_EQ("1", fieldOf(rejects[0], FIX::FIELD::CxlRejResponseTo));
    EXPECT_EQ("CXL-1", fieldOf(rejects[0], FIX::FIELD::ClOrdID));
    EXPECT_EQ("Cancel rejected: the order isn't acknowledged yet", fieldOf(rejects[0], FIX::FIELD::Text));
    EXPECT_EQ("2", fieldOf(rejects[1], FIX::FIELD::CxlRejResponseTo));
    EXPECT_EQ("RPL-1", fieldOf(rejects[1], FIX::FIELD::ClOrdID));
    EXPECT_EQ("Replace refused: the order isn't acknowledged yet", fieldOf(rejects[1], FIX::FIELD::Text));
    for (const FIX::Message &reject : rejects)
    {
        EXPECT_EQ("NONE", fieldOf(reject, FIX::FIELD::OrderID));
        EXPECT_EQ("ORD-1", fieldOf(reject, FIX::FIELD::OrigClOrdID));
        EXPECT_EQ(std::string(1, FIX::OrdStatus_PENDING_NEW), fieldOf(reject, FIX::FIELD::OrdStatus));
        EXPECT_EQ(std::to_string(FIX::CxlRejReason_OTHER), fieldOf(reject, FIX::FIELD::CxlRejReason));
    }
    EXPECT_EQ(0u, gateway_->waitingRequests());
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, TheEnginesRefusalOfTheOrderEndsTheWaitOfARequestForIt)
{
    // A request waiting for its order is answered once the engine refuses the order: the order's rejection goes first,
    // then the request's, as for an unknown order (#380)
    ASSERT_NO_FATAL_FAILURE(logOn());
    std::unique_ptr<OrderEntry> pushed;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    receive(makeNewOrderSingle("ORD-2", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100));
    ASSERT_NE(nullptr, pushed);
    sent_.clear();

    // The cancel comes on the session's thread, which waits; the engine's refusal on one of its own
    std::thread client(
        [&]
        {
            receive(makeCancelRequest("ORD-2", "CXL-2"));
        });
    bool waited = false;
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
         !waited && (std::chrono::steady_clock::now() < deadline);)
    {
        waited = (1u == gateway_->waitingRequests());
        if (!waited)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const std::string source = FixGateway::makeSourceString(sid_);
    Queues::OrderRejectEvent refusal; // as the engine refuses an order it couldn't store, by its stored ClOrdID
    refusal.clOrderId_ = FixGateway::sessionClOrdId(source, "ORD-2");
    refusal.symbol_ = "aaa";
    refusal.side_ = BUY_SIDE;
    refusal.orderQty_ = 100;
    refusal.reason_ = "Order refused: it could not be stored";
    const auto start = std::chrono::steady_clock::now();
    gateway_->sendOrderReject(refusal, source);
    client.join();
    EXPECT_TRUE(waited);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30)); // not the minute's timeout

    ASSERT_EQ(2u, sent_.size()) << sentText();
    const FIX::Message rejection = parsed(sent_[0]);
    const FIX::Message cancelReject = parsed(sent_[1]);
    EXPECT_EQ(FIX::MsgType_ExecutionReport, fieldOf(rejection.getHeader(), FIX::FIELD::MsgType));
    EXPECT_EQ("ORD-2", fieldOf(rejection, FIX::FIELD::ClOrdID));
    EXPECT_EQ(std::string(1, FIX::ExecType_REJECTED), fieldOf(rejection, FIX::FIELD::ExecType));
    EXPECT_EQ(FIX::MsgType_OrderCancelReject, fieldOf(cancelReject.getHeader(), FIX::FIELD::MsgType));
    EXPECT_EQ("CXL-2", fieldOf(cancelReject, FIX::FIELD::ClOrdID));
    EXPECT_EQ("ORD-2", fieldOf(cancelReject, FIX::FIELD::OrigClOrdID));
    EXPECT_EQ(std::to_string(FIX::CxlRejReason_UNKNOWN_ORDER), fieldOf(cancelReject, FIX::FIELD::CxlRejReason));
    EXPECT_EQ(std::string(1, FIX::OrdStatus_REJECTED), fieldOf(cancelReject, FIX::FIELD::OrdStatus));
    expectEverythingSentIsTaken();
}

// =============================================================================
// OrderStatusRequest (35=H), as EP284's section G answers one (#383)
// =============================================================================

/// An OrderStatusRequest for the session's buy of "aaa", naming the order by ClOrdID (11), OrderID (37), both, or
/// neither, as given, with reqId as its OrdStatusReqID (790) if not empty
FIX50SP2::OrderStatusRequest makeStatusRequest(const std::string &clOrdId, const std::string &orderId = std::string(),
                                               const std::string &reqId = std::string())
{
    FIX50SP2::OrderStatusRequest msg;
    if (!clOrdId.empty())
    {
        msg.set(FIX::ClOrdID(clOrdId));
    }
    if (!orderId.empty())
    {
        msg.set(FIX::OrderID(orderId));
    }
    if (!reqId.empty())
    {
        msg.set(FIX::OrdStatusReqID(reqId));
    }
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::Side(FIX::Side_BUY));
    return msg;
}

/// Fails unless status is an answer to a status request, ExecType Order Status (150=I) with ExecID 0, which says
/// ordStatus of the order orderId, with these quantities
void expectStatus(const FIX::Message &status, char ordStatus, const std::string &orderId, QuantityT orderQty,
                  QuantityT cumQty, QuantityT leavesQty)
{
    EXPECT_EQ(std::string(1, FIX::ExecType_ORDER_STATUS), fieldOf(status, FIX::FIELD::ExecType));
    EXPECT_EQ("0", fieldOf(status, FIX::FIELD::ExecID));
    EXPECT_EQ(std::string(1, ordStatus), fieldOf(status, FIX::FIELD::OrdStatus));
    EXPECT_EQ(orderId, fieldOf(status, FIX::FIELD::OrderID));
    EXPECT_EQ(std::to_string(orderQty), fieldOf(status, FIX::FIELD::OrderQty));
    EXPECT_EQ(std::to_string(cumQty), fieldOf(status, FIX::FIELD::CumQty));
    EXPECT_EQ(std::to_string(leavesQty), fieldOf(status, FIX::FIELD::LeavesQty));
    EXPECT_EQ(std::string(1, FIX::Side_BUY), fieldOf(status, FIX::FIELD::Side));
    EXPECT_EQ("aaa", fieldOf(status, FIX::FIELD::Symbol));
}

/// Gives a stored order the state a test describes
void setState(OrderEntry *order, OrderStatus status, QuantityT orderQty, QuantityT cumQty, QuantityT leavesQty)
{
    order->status_ = status;
    order->orderQty_ = orderQty;
    order->cumQty_ = cumQty;
    order->leavesQty_ = leavesQty;
}

TEST_F(FixtSessionTest, AStatusRequestIsAnsweredWithTheOrdersStateAndQuantities)
{
    // EP284's G.1.c from its row 5: an ExecutionReport of ExecType Order Status (150=I), with ExecID 0, as FIX gives
    // every one, the order's OrdStatus and quantities, and the request's OrdStatusReqID (790) when it has one
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0); // the engine never hears of it
    OrderEntry *order = saveOrderOf(sid_, "ORD-1");
    const std::string orderId = std::to_string(order->orderId_.id_);
    setState(order, NEW_ORDSTATUS, 100, 0, 100);
    receive(makeStatusRequest("ORD-1", "", "STATUS-1"));
    setState(order, PARTFILL_ORDSTATUS, 100, 40, 60);
    order->avgPx_ = 10.5;
    receive(makeStatusRequest("ORD-1"));
    setState(order, FILLED_ORDSTATUS, 100, 100, 0);
    receive(makeStatusRequest("ORD-1"));

    const std::vector<FIX::Message> answers = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(3u, answers.size()) << sentText();
    expectStatus(answers[0], FIX::OrdStatus_NEW, orderId, 100, 0, 100);
    EXPECT_EQ("STATUS-1", fieldOf(answers[0], FIX::FIELD::OrdStatusReqID));
    expectStatus(answers[1], FIX::OrdStatus_PARTIALLY_FILLED, orderId, 100, 40, 60);
    EXPECT_EQ("10.5", fieldOf(answers[1], FIX::FIELD::AvgPx));
    EXPECT_EQ("(not set)", fieldOf(answers[1], FIX::FIELD::OrdStatusReqID));
    expectStatus(answers[2], FIX::OrdStatus_FILLED, orderId, 100, 100, 0);
    for (const FIX::Message &answer : answers)
    {
        EXPECT_EQ("ORD-1", fieldOf(answer, FIX::FIELD::ClOrdID));
        EXPECT_EQ("(not set)", fieldOf(answer, FIX::FIELD::OrigClOrdID));
        EXPECT_EQ("(not set)", fieldOf(answer, FIX::FIELD::OrdRejReason));
    }
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusAnswersQuantitiesAreTheBytesQuickFixWritesForTheDouble)
{
    // A status answer's quantities are written as the whole numbers they are (#396), with the bytes QuickFIX writes for
    // the doubles its QTY fields take, up to the largest quantity. A GT order that traded on an earlier day has five:
    // OrderQty, CumQty and LeavesQty, and its figures for the day, DayOrderQty and DayCumQty (#244).
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *order = saveOrderOf(sid_, "ORD-1");
    const QuantityT most = std::numeric_limits<QuantityT>::max();
    setState(order, PARTFILL_ORDSTATUS, most, most - 1, 1);
    order->tif_ = GTC_TIF;
    order->dayOrderQty_ = most - 2;
    order->dayCumQty_ = 0;
    receive(makeStatusRequest("ORD-1"));

    const std::vector<FIX::Message> answers = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(1u, answers.size()) << sentText();
    EXPECT_EQ(qtyAsDouble(most), fieldOf(answers[0], FIX::FIELD::OrderQty));
    EXPECT_EQ(qtyAsDouble(most - 1), fieldOf(answers[0], FIX::FIELD::CumQty));
    EXPECT_EQ(qtyAsDouble(1), fieldOf(answers[0], FIX::FIELD::LeavesQty));
    EXPECT_EQ(qtyAsDouble(most - 2), fieldOf(answers[0], FIX::FIELD::DayOrderQty));
    EXPECT_EQ(qtyAsDouble(0), fieldOf(answers[0], FIX::FIELD::DayCumQty));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestForAnOrderTheSessionDoesntHaveIsAnsweredAsUnknown)
{
    // EP284's G.1.a: OrdStatus Rejected (39=8), OrdRejReason unknown order (103=5), OrderID NONE and no quantities. An
    // order of another session's is unknown to this one, as for a cancel (#71), by either id, and so is an OrderID that
    // isn't an order number.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *theirs = saveOrderOf(OTHER_SID, "ORD-B");
    receive(makeStatusRequest("NO-SUCH"));
    receive(makeStatusRequest("ORD-B"));
    receive(makeStatusRequest("", std::to_string(theirs->orderId_.id_)));
    receive(makeStatusRequest("", "ORD-B"));

    const std::vector<FIX::Message> answers = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(4u, answers.size()) << sentText();
    for (const FIX::Message &answer : answers)
    {
        expectStatus(answer, FIX::OrdStatus_REJECTED, "NONE", 0, 0, 0);
        EXPECT_EQ(std::to_string(FIX::OrdRejReason_UNKNOWN_ORDER), fieldOf(answer, FIX::FIELD::OrdRejReason));
        EXPECT_EQ("Unknown order", fieldOf(answer, FIX::FIELD::Text));
    }
    EXPECT_EQ("NO-SUCH", fieldOf(answers[0], FIX::FIELD::ClOrdID));
    EXPECT_EQ("ORD-B", fieldOf(answers[1], FIX::FIELD::ClOrdID));
    EXPECT_EQ("(not set)", fieldOf(answers[2], FIX::FIELD::ClOrdID));
    EXPECT_EQ("(not set)", fieldOf(answers[3], FIX::FIELD::ClOrdID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestForAnOrderNotYetAcknowledgedIsPendingNew)
{
    // EP284's G.1.c, rows 1 to 6: a status request sent right after its order, before the engine acknowledges it, is
    // answered Pending New (39=A), with the OrderQty the order was sent with and nothing filled: before the engine
    // stores it, with no OrderID yet, and once stored, until its acknowledgement is out. Then New.
    ASSERT_NO_FATAL_FAILURE(logOn());
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    receive(makeNewOrderSingle("ORD-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100));
    ASSERT_NE(nullptr, pushed);
    receive(makeStatusRequest("ORD-1"));
    OrderEntry *order = saveOrderOf(sid_, "ORD-1"); // as the engine stores it
    setState(order, NEW_ORDSTATUS, 100, 0, 100);
    receive(makeStatusRequest("ORD-1"));
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    ack.orderId_ = order->orderId_;
    gateway_->sendExecutionReport(&ack, *order);
    receive(makeStatusRequest("ORD-1"));

    const std::string orderId = std::to_string(order->orderId_.id_);
    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(4u, reports.size()) << sentText();
    expectStatus(reports[0], FIX::OrdStatus_PENDING_NEW, "NONE", 100, 0, 100);
    expectStatus(reports[1], FIX::OrdStatus_PENDING_NEW, orderId, 100, 0, 100);
    EXPECT_EQ(std::string(1, FIX::ExecType_NEW), fieldOf(reports[2], FIX::FIELD::ExecType));
    expectStatus(reports[3], FIX::OrdStatus_NEW, orderId, 100, 0, 100);
    for (const FIX::Message &report : reports)
    {
        EXPECT_EQ("ORD-1", fieldOf(report, FIX::FIELD::ClOrdID));
    }
    expectEverythingSentIsTaken();
}

/// Fails unless reject refuses a new order as a duplicate, as EP284's F.1.a combines one (#382): ExecType Rejected
/// (150=8) with an ExecID of its own, OrdRejReason duplicate order (103=6) and why in Text, under the ClOrdID clOrdId,
/// with the status of the order orderId, which has that ClOrdID: its ordStatus and quantities, not the refused order's
void expectDuplicateReject(const FIX::Message &reject, const std::string &clOrdId, char ordStatus,
                           const std::string &orderId, QuantityT orderQty, QuantityT cumQty, QuantityT leavesQty)
{
    EXPECT_EQ(std::string(1, FIX::ExecType_REJECTED), fieldOf(reject, FIX::FIELD::ExecType));
    EXPECT_THAT(fieldOf(reject, FIX::FIELD::ExecID), testing::StartsWith("R"));
    EXPECT_EQ(std::to_string(FIX::OrdRejReason_DUPLICATE_ORDER), fieldOf(reject, FIX::FIELD::OrdRejReason));
    EXPECT_EQ("Order refused: ClOrdID " + clOrdId + " is already in use", fieldOf(reject, FIX::FIELD::Text));
    EXPECT_EQ(clOrdId, fieldOf(reject, FIX::FIELD::ClOrdID));
    EXPECT_EQ(std::string(1, ordStatus), fieldOf(reject, FIX::FIELD::OrdStatus));
    EXPECT_EQ(orderId, fieldOf(reject, FIX::FIELD::OrderID));
    EXPECT_EQ(std::to_string(orderQty), fieldOf(reject, FIX::FIELD::OrderQty));
    EXPECT_EQ(std::to_string(cumQty), fieldOf(reject, FIX::FIELD::CumQty));
    EXPECT_EQ(std::to_string(leavesQty), fieldOf(reject, FIX::FIELD::LeavesQty));
    EXPECT_EQ(std::string(1, FIX::Side_BUY), fieldOf(reject, FIX::FIELD::Side));
    EXPECT_EQ("aaa", fieldOf(reject, FIX::FIELD::Symbol));
}

TEST_F(FixtSessionTest, ANewOrderThatRepeatsAClOrdIdStillOnItsWayIsRefusedAsPendingNew)
{
    // A new order that repeats the ClOrdID of one the engine hasn't acknowledged is refused at once as a duplicate, with
    // the first order's status, Pending New (39=A) with its OrderQty, and no OrderID until the engine stores it (#382).
    // It never reaches the engine, where it was refused as a duplicate or, stored first, refused the first order. A
    // status request is answered with the first order meanwhile (#383), then New once its acknowledgement is out.
    ASSERT_NO_FATAL_FAILURE(logOn());
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    receive(makeNewOrderSingle("ORD-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100));
    ASSERT_NE(nullptr, pushed);
    receive(makeNewOrderSingle("ORD-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 300));
    receive(makeStatusRequest("ORD-1"));
    OrderEntry *order = saveOrderOf(sid_, "ORD-1"); // as the engine stores the first
    setState(order, NEW_ORDSTATUS, 100, 0, 100);
    receive(makeNewOrderSingle("ORD-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 300));
    auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    ack.orderId_ = order->orderId_;
    gateway_->sendExecutionReport(&ack, *order);
    receive(makeStatusRequest("ORD-1"));

    const std::string orderId = std::to_string(order->orderId_.id_);
    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(5u, reports.size()) << sentText();
    expectDuplicateReject(reports[0], "ORD-1", FIX::OrdStatus_PENDING_NEW, "NONE", 100, 0, 100);
    expectStatus(reports[1], FIX::OrdStatus_PENDING_NEW, "NONE", 100, 0, 100);
    expectDuplicateReject(reports[2], "ORD-1", FIX::OrdStatus_PENDING_NEW, orderId, 100, 0, 100);
    EXPECT_EQ(std::string(1, FIX::ExecType_NEW), fieldOf(reports[3], FIX::FIELD::ExecType));
    expectStatus(reports[4], FIX::OrdStatus_NEW, orderId, 100, 0, 100);
    EXPECT_EQ("ORD-1", fieldOf(reports[1], FIX::FIELD::ClOrdID));
    EXPECT_NE(fieldOf(reports[0], FIX::FIELD::ExecID), fieldOf(reports[2], FIX::FIELD::ExecID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestForAReplacedOrderIsAnsweredWithTheOrderThatReplacedIt)
{
    // EP284's G.1.c, rows 16 to 19: after Replace (Y,X), a status request for X is answered with Y's state, naming Y in
    // ClOrdID (11) and X in OrigClOrdID (41) "to allow tie back", and one for Y with Y's. Until Y's acknowledgement is
    // out, the replace is still pending for the client: X, Pending Replace (39=E), with what it had left.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, REPLACED_ORDSTATUS, 100, 40, 0); // as a replace leaves it
    OrderEntry *y = saveOrderOf(sid_, "ORD-Y");
    y->origOrderId_ = x->orderId_;
    y->origClOrderId_ = x->clOrderId_;
    setState(y, PARTFILL_ORDSTATUS, 120, 40, 80);
    receive(makeStatusRequest("ORD-X"));
    auto ack = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, PARTFILL_ORDSTATUS);
    ack.orderId_ = y->orderId_;
    ack.origOrderId_ = x->orderId_;
    gateway_->sendExecutionReport(&ack, *y);
    receive(makeStatusRequest("ORD-X"));
    receive(makeStatusRequest("ORD-Y"));

    const std::string xId = std::to_string(x->orderId_.id_);
    const std::string yId = std::to_string(y->orderId_.id_);
    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(4u, reports.size()) << sentText();
    expectStatus(reports[0], FIX::OrdStatus_PENDING_REPLACE, xId, 100, 40, 60);
    EXPECT_EQ("ORD-X", fieldOf(reports[0], FIX::FIELD::ClOrdID));
    EXPECT_EQ("(not set)", fieldOf(reports[0], FIX::FIELD::OrigClOrdID));
    EXPECT_EQ(std::string(1, FIX::ExecType_REPLACED), fieldOf(reports[1], FIX::FIELD::ExecType));
    expectStatus(reports[2], FIX::OrdStatus_PARTIALLY_FILLED, yId, 120, 40, 80);
    EXPECT_EQ("ORD-Y", fieldOf(reports[2], FIX::FIELD::ClOrdID));
    EXPECT_EQ("ORD-X", fieldOf(reports[2], FIX::FIELD::OrigClOrdID));
    expectStatus(reports[3], FIX::OrdStatus_PARTIALLY_FILLED, yId, 120, 40, 80);
    EXPECT_EQ("ORD-Y", fieldOf(reports[3], FIX::FIELD::ClOrdID));
    EXPECT_EQ("(not set)", fieldOf(reports[3], FIX::FIELD::OrigClOrdID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestForAReplacementNotYetAcknowledgedIsItsOriginalPendingReplace)
{
    // A replacement the engine hasn't acknowledged, stored or not: the replace is pending, so the answer is its
    // original's state, Pending Replace (39=E), under the replacement's ClOrdID and naming the original's, as EP284's
    // G.1.c acknowledges a replace (row 14). A request for the original says the same under its own ClOrdID.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, PARTFILL_ORDSTATUS, 100, 40, 60);
    std::unique_ptr<OrderEntry> replacement; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replacement.reset(evt.replacementOrder_);
            }));
    receive(makeLimitReplace("ORD-X", "ORD-Y"));
    ASSERT_NE(nullptr, replacement);
    receive(makeStatusRequest("ORD-Y"));
    OrderEntry *y = saveOrderOf(sid_, "ORD-Y"); // as the engine stores it, and replaces X
    y->origOrderId_ = x->orderId_;
    y->origClOrderId_ = x->clOrderId_;
    setState(y, PARTFILL_ORDSTATUS, 200, 40, 160);
    setState(x, REPLACED_ORDSTATUS, 100, 40, 0);
    receive(makeStatusRequest("ORD-Y"));
    receive(makeStatusRequest("ORD-X"));

    const std::string xId = std::to_string(x->orderId_.id_);
    const std::vector<FIX::Message> answers = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(3u, answers.size()) << sentText();
    for (const FIX::Message &answer : answers)
    {
        expectStatus(answer, FIX::OrdStatus_PENDING_REPLACE, xId, 100, 40, 60);
    }
    for (size_t i : { 0u, 1u })
    {
        EXPECT_EQ("ORD-Y", fieldOf(answers[i], FIX::FIELD::ClOrdID));
        EXPECT_EQ("ORD-X", fieldOf(answers[i], FIX::FIELD::OrigClOrdID));
    }
    EXPECT_EQ("ORD-X", fieldOf(answers[2], FIX::FIELD::ClOrdID));
    EXPECT_EQ("(not set)", fieldOf(answers[2], FIX::FIELD::OrigClOrdID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AReplacementStoredBeforeTheGatewayWasMadeIsKnown)
{
    // The server restores its orders before it makes the gateway, which learns from them which order replaced which:
    // a status request for an order replaced before a restart is answered with its replacement, as for one since
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, REPLACED_ORDSTATUS, 100, 0, 0);
    OrderEntry *y = saveOrderOf(sid_, "ORD-Y");
    y->origOrderId_ = x->orderId_;
    y->origClOrderId_ = x->clOrderId_;
    setState(y, NEW_ORDSTATUS, 150, 0, 150);
    makeSession(std::chrono::seconds(60)); // a gateway made after them, as at a restart
    ASSERT_NO_FATAL_FAILURE(logOn());
    receive(makeStatusRequest("ORD-X"));

    const std::vector<FIX::Message> answers = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(1u, answers.size()) << sentText();
    expectStatus(answers[0], FIX::OrdStatus_NEW, std::to_string(y->orderId_.id_), 150, 0, 150);
    EXPECT_EQ("ORD-Y", fieldOf(answers[0], FIX::FIELD::ClOrdID));
    EXPECT_EQ("ORD-X", fieldOf(answers[0], FIX::FIELD::OrigClOrdID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestNamesTheOrderByOrderIdAloneOrByBothIds)
{
    // FIX 5.0 SP2's OrderStatusRequest may name the order by OrderID (37) alone. Naming it by both, they must name the
    // same order: an OrderID of another order's names none, and is answered as unknown, saying why.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *one = saveOrderOf(sid_, "ORD-1");
    setState(one, NEW_ORDSTATUS, 100, 0, 100);
    OrderEntry *two = saveOrderOf(sid_, "ORD-2");
    setState(two, NEW_ORDSTATUS, 50, 0, 50);
    const std::string oneId = std::to_string(one->orderId_.id_);
    receive(makeStatusRequest("", oneId));
    receive(makeStatusRequest("ORD-1", oneId));
    receive(makeStatusRequest("ORD-1", std::to_string(two->orderId_.id_)));

    const std::vector<FIX::Message> answers = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(3u, answers.size()) << sentText();
    for (size_t i : { 0u, 1u })
    {
        expectStatus(answers[i], FIX::OrdStatus_NEW, oneId, 100, 0, 100);
        EXPECT_EQ("ORD-1", fieldOf(answers[i], FIX::FIELD::ClOrdID));
    }
    expectStatus(answers[2], FIX::OrdStatus_REJECTED, "NONE", 0, 0, 0);
    EXPECT_EQ("ORD-1", fieldOf(answers[2], FIX::FIELD::ClOrdID));
    EXPECT_EQ(std::to_string(FIX::OrdRejReason_UNKNOWN_ORDER), fieldOf(answers[2], FIX::FIELD::OrdRejReason));
    EXPECT_THAT(fieldOf(answers[2], FIX::FIELD::Text), testing::HasSubstr("OrderID (37)"));
    EXPECT_THAT(fieldOf(answers[2], FIX::FIELD::Text), testing::HasSubstr("ClOrdID (11) ORD-1"));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestAlongAReplaceMayGiveTheOrderIdItsAnswerReports)
{
    // Along a replace, the OrderID (37) given with the ClOrdID (11) may be the one the answer reports: the original's,
    // which the client's replace named, for the replacement not yet acknowledged, and the replacement's, which its
    // acknowledgement gave, for the original. Another order's names neither.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, PARTFILL_ORDSTATUS, 100, 40, 60);
    OrderEntry *other = saveOrderOf(sid_, "ORD-O");
    setState(other, NEW_ORDSTATUS, 10, 0, 10);
    const std::string xId = std::to_string(x->orderId_.id_);
    const std::string otherId = std::to_string(other->orderId_.id_);
    std::unique_ptr<OrderEntry> replacement; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replacement.reset(evt.replacementOrder_);
            }));
    receive(makeLimitReplace("ORD-X", "ORD-Y"));
    ASSERT_NE(nullptr, replacement);
    receive(makeStatusRequest("ORD-Y", xId));
    receive(makeStatusRequest("ORD-Y", otherId));
    OrderEntry *y = saveOrderOf(sid_, "ORD-Y"); // as the engine stores it, and replaces X
    y->origOrderId_ = x->orderId_;
    y->origClOrderId_ = x->clOrderId_;
    setState(y, PARTFILL_ORDSTATUS, 200, 40, 160);
    setState(x, REPLACED_ORDSTATUS, 100, 40, 0);
    auto ack = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, PARTFILL_ORDSTATUS);
    ack.orderId_ = y->orderId_;
    ack.origOrderId_ = x->orderId_;
    gateway_->sendExecutionReport(&ack, *y);
    const std::string yId = std::to_string(y->orderId_.id_);
    receive(makeStatusRequest("ORD-X", yId));
    receive(makeStatusRequest("ORD-X", xId));
    receive(makeStatusRequest("ORD-X", otherId));

    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(6u, reports.size()) << sentText();
    expectStatus(reports[0], FIX::OrdStatus_PENDING_REPLACE, xId, 100, 40, 60);
    EXPECT_EQ("ORD-Y", fieldOf(reports[0], FIX::FIELD::ClOrdID));
    EXPECT_EQ("ORD-X", fieldOf(reports[0], FIX::FIELD::OrigClOrdID));
    EXPECT_EQ(std::string(1, FIX::ExecType_REPLACED), fieldOf(reports[2], FIX::FIELD::ExecType));
    for (size_t i : { 3u, 4u })
    {
        expectStatus(reports[i], FIX::OrdStatus_PARTIALLY_FILLED, yId, 200, 40, 160);
        EXPECT_EQ("ORD-Y", fieldOf(reports[i], FIX::FIELD::ClOrdID));
        EXPECT_EQ("ORD-X", fieldOf(reports[i], FIX::FIELD::OrigClOrdID));
    }
    for (size_t i : { 1u, 5u })
    {
        expectStatus(reports[i], FIX::OrdStatus_REJECTED, "NONE", 0, 0, 0);
        EXPECT_EQ(std::to_string(FIX::OrdRejReason_UNKNOWN_ORDER), fieldOf(reports[i], FIX::FIELD::OrdRejReason));
        EXPECT_THAT(fieldOf(reports[i], FIX::FIELD::Text), testing::HasSubstr("OrderID (37) " + otherId));
    }
    EXPECT_EQ("ORD-Y", fieldOf(reports[1], FIX::FIELD::ClOrdID));
    EXPECT_EQ("ORD-X", fieldOf(reports[5], FIX::FIELD::ClOrdID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AStatusRequestThatNamesNoOrderIsRefusedReadably)
{
    // FIX 5.0 SP2 requires neither ClOrdID (11) nor OrderID (37), and the gateway needs one: a BusinessMessageReject,
    // conditionally required field missing (380=5), naming ClOrdID, as for a cancel without its OrigClOrdID
    ASSERT_NO_FATAL_FAILURE(logOn());
    receive(makeStatusRequest(""));
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_BusinessMessageReject);
    ASSERT_EQ(1u, rejects.size()) << sentText();
    EXPECT_EQ(std::to_string(FIX::BusinessRejectReason_CONDITIONALLY_REQUIRED_FIELD_MISSING),
              fieldOf(rejects[0], FIX::FIELD::BusinessRejectReason));
    EXPECT_EQ(FIX::MsgType_OrderStatusRequest, fieldOf(rejects[0], FIX::FIELD::RefMsgType));
    EXPECT_THAT(fieldOf(rejects[0], FIX::FIELD::Text), testing::HasSubstr("(11)"));
    EXPECT_TRUE(sentOfType(FIX::MsgType_ExecutionReport).empty()) << sentText();
    expectEverythingSentIsTaken();
}

// =============================================================================
// A new order whose ClOrdID the session has sent, as EP284's F.1 answers one (#382)
// =============================================================================

/// msg, marked PossResend (97) Y, as a message that may have been sent before, or N
template <typename Message> Message withPossResend(Message msg, bool possResend)
{
    msg.getHeader().setField(FIX::PossResend(possResend));
    return msg;
}

TEST_F(FixtSessionTest, ANewOrderThatRepeatsAWorkingOrdersClOrdIdIsRefusedWithThatOrdersState)
{
    // EP284's F.1.a: a new order whose ClOrdID a working order of the session's has is refused as a duplicate order
    // (103=6), and the reject carries the working order's status: its OrderID, Partially Filled, 10000/1000/9000, not
    // the refused order's 15000. A NewOrderMultileg is refused the same way. The gateway refuses them: the engine never
    // sees them.
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    OrderEntry *order = saveOrderOf(sid_, "X");
    setState(order, PARTFILL_ORDSTATUS, 10000, 1000, 9000);
    receive(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 15000));
    receive(makeFxSwap("X"));

    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(2u, rejects.size()) << sentText();
    for (const FIX::Message &reject : rejects)
    {
        expectDuplicateReject(reject, "X", FIX::OrdStatus_PARTIALLY_FILLED, std::to_string(order->orderId_.id_), 10000,
                              1000, 9000);
    }
    EXPECT_NE(fieldOf(rejects[0], FIX::FIELD::ExecID), fieldOf(rejects[1], FIX::FIELD::ExecID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, ANewOrderSentAgainWithPossResendIsAnsweredWithItsState)
{
    // EP284's F.1.b: once X has been received, X sent again with PossResend (97) Y is answered with X's state, ExecType
    // Order Status (150=I), as a status request for it is (row 4); X again without PossResend, or with it N, is refused
    // as a duplicate with X's status (row 6); and Y with PossResend Y, which the session hasn't sent, is a new order
    // (rows 7 and 8)
    ASSERT_NO_FATAL_FAILURE(logOn());
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    OrderEntry *order = saveOrderOf(sid_, "X");
    setState(order, NEW_ORDSTATUS, 10000, 0, 10000);
    receive(withPossResend(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 10000), true));
    receive(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 20000));
    receive(withPossResend(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 20000), false));
    receive(withPossResend(makeNewOrderSingle("Y", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 15000), true));

    const std::string orderId = std::to_string(order->orderId_.id_);
    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(3u, reports.size()) << sentText();
    expectStatus(reports[0], FIX::OrdStatus_NEW, orderId, 10000, 0, 10000);
    EXPECT_EQ("X", fieldOf(reports[0], FIX::FIELD::ClOrdID));
    EXPECT_EQ("(not set)", fieldOf(reports[0], FIX::FIELD::OrdRejReason));
    expectDuplicateReject(reports[1], "X", FIX::OrdStatus_NEW, orderId, 10000, 0, 10000);
    expectDuplicateReject(reports[2], "X", FIX::OrdStatus_NEW, orderId, 10000, 0, 10000);
    ASSERT_NE(nullptr, pushed);
    const RawDataEntry &clOrd = pushed->clOrderId_.get();
    EXPECT_EQ(FixGateway::sessionClOrdId(FixGateway::makeSourceString(sid_), "Y"),
              std::string(clOrd.data_, clOrd.length_));
    EXPECT_EQ(15000u, pushed->orderQty_);
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, ANewOrderThatRepeatsTheClOrdIdOfAReplacedOrderIsRefusedWithThatOrdersState)
{
    // The reject goes by the refused order's ClOrdID, so it carries the status of the order that has that ClOrdID, its
    // own, and not that of the order that replaced it, which a status request is answered with (#383): Pending Replace
    // (39=E) until the replacement's acknowledgement is out, then 39=4, as the gateway reports a replaced order (#74).
    // PossResend Y is answered as that status request is.
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    OrderEntry *x = saveOrderOf(sid_, "X");
    setState(x, REPLACED_ORDSTATUS, 100, 40, 0); // as a replace leaves it
    OrderEntry *y = saveOrderOf(sid_, "Y");
    y->origOrderId_ = x->orderId_;
    y->origClOrderId_ = x->clOrderId_;
    setState(y, PARTFILL_ORDSTATUS, 120, 40, 80);
    receive(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 300));
    auto ack = makeReport<ReplaceExecEntry>(REPLACE_EXECTYPE, PARTFILL_ORDSTATUS);
    ack.orderId_ = y->orderId_;
    ack.origOrderId_ = x->orderId_;
    gateway_->sendExecutionReport(&ack, *y); // from which the gateway learns that Y replaced X (#383)
    receive(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 300));
    receive(withPossResend(makeNewOrderSingle("X", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100), true));

    const std::string xId = std::to_string(x->orderId_.id_);
    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(4u, reports.size()) << sentText();
    expectDuplicateReject(reports[0], "X", FIX::OrdStatus_PENDING_REPLACE, xId, 100, 40, 60);
    EXPECT_EQ(std::string(1, FIX::ExecType_REPLACED), fieldOf(reports[1], FIX::FIELD::ExecType));
    expectDuplicateReject(reports[2], "X", FIX::OrdStatus_CANCELED, xId, 100, 40, 0);
    expectStatus(reports[3], FIX::OrdStatus_PARTIALLY_FILLED, std::to_string(y->orderId_.id_), 120, 40, 80);
    EXPECT_EQ("Y", fieldOf(reports[3], FIX::FIELD::ClOrdID));
    EXPECT_EQ("X", fieldOf(reports[3], FIX::FIELD::OrigClOrdID));
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, TheEnginesRefusalOfADuplicateCarriesTheStateOfTheOrderThatHasItsClOrdId)
{
    // The gateway refuses a session's duplicates before the engine sees them (#382), and the engine refuses one it gets
    // as a duplicate (#67): that refusal also carries the state of the order that has the ClOrdID, as F.1.a combines
    // them, read from the store alone, since the gateway's record of what it sent would be the duplicate's own
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *order = saveOrderOf(sid_, "X");
    setState(order, PARTFILL_ORDSTATUS, 10000, 1000, 9000);
    const std::string source = FixGateway::makeSourceString(sid_);
    Queues::OrderRejectEvent refusal; // as Processor::rejectUnstoredOrder() makes it, by the stored ClOrdID
    refusal.clOrderId_ = FixGateway::sessionClOrdId(source, "X");
    refusal.symbol_ = "aaa";
    refusal.side_ = BUY_SIDE;
    refusal.orderQty_ = 15000;
    refusal.duplicateClOrderId_ = true;
    refusal.reason_ = "Order refused: ClOrdID " + refusal.clOrderId_ + " is already in use";
    gateway_->sendOrderReject(refusal, source);

    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(1u, reports.size()) << sentText();
    expectDuplicateReject(reports[0], "X", FIX::OrdStatus_PARTIALLY_FILLED, std::to_string(order->orderId_.id_), 10000,
                          1000, 9000);
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, AReplaceWhoseClOrdIdIsInUseIsRefusedBeforeTheEngine)
{
    // Replace (Y,X) with a Y the session already uses, for an order stored or still on its way, or X's own, is refused at
    // once with an OrderCancelReject, 434=2 and CxlRejReason 102=6, describing X, as the engine refuses it (#403). It
    // never reaches the engine. Left to the engine, the replacement held Y in the record until that refusal, and a
    // status request for Y was answered with X, Pending Replace (39=E): here each is answered with the order that has Y.
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, PARTFILL_ORDSTATUS, 100, 40, 60);
    OrderEntry *stored = saveOrderOf(sid_, "ORD-S");
    setState(stored, NEW_ORDSTATUS, 50, 0, 50);
    receive(makeNewOrderSingle("ORD-P", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 70));
    ASSERT_NE(nullptr, pushed);
    const std::vector<std::string> inUse{ "ORD-S", "ORD-P", "ORD-X" }; // stored, on its way, the original's own
    for (const std::string &clOrdId : inUse)
    {
        receive(makeLimitReplace("ORD-X", clOrdId));
    }
    receive(makeStatusRequest("ORD-S"));
    receive(makeStatusRequest("ORD-P"));

    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_OrderCancelReject);
    ASSERT_EQ(inUse.size(), rejects.size()) << sentText();
    for (size_t i = 0; i < rejects.size(); ++i)
    {
        SCOPED_TRACE(inUse[i]);
        EXPECT_EQ(inUse[i], fieldOf(rejects[i], FIX::FIELD::ClOrdID));
        EXPECT_EQ(std::string(1, FIX::CxlRejResponseTo_ORDER_CANCEL_REPLACE_REQUEST),
                  fieldOf(rejects[i], FIX::FIELD::CxlRejResponseTo));
        EXPECT_EQ(std::to_string(FIX::CxlRejReason_DUPLICATE_CL_ORD_ID), fieldOf(rejects[i], FIX::FIELD::CxlRejReason));
        EXPECT_EQ("Replace refused: ClOrdID " + inUse[i] + " is already in use", fieldOf(rejects[i], FIX::FIELD::Text));
        EXPECT_EQ("ORD-X", fieldOf(rejects[i], FIX::FIELD::OrigClOrdID));
        EXPECT_EQ(std::to_string(x->orderId_.id_), fieldOf(rejects[i], FIX::FIELD::OrderID));
        EXPECT_EQ(std::string(1, FIX::OrdStatus_PARTIALLY_FILLED), fieldOf(rejects[i], FIX::FIELD::OrdStatus));
    }
    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(2u, reports.size()) << sentText();
    expectStatus(reports[0], FIX::OrdStatus_NEW, std::to_string(stored->orderId_.id_), 50, 0, 50);
    EXPECT_EQ("(not set)", fieldOf(reports[0], FIX::FIELD::OrigClOrdID));
    expectStatus(reports[1], FIX::OrdStatus_PENDING_NEW, "NONE", 70, 0, 70);
    EXPECT_EQ(PARTFILL_ORDSTATUS, x->status_);
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, ARequestThatNamesARefusedReplacementIsDecidedAgainstItsOriginal)
{
    // EP284's D.2.c: Replace (Y,X) is refused, and a replace that names Y, the last ClOrdID not refused when the client
    // sent it, is decided against X, the order Y was to replace (#398): handed to the engine as a replace of X, or
    // refused by the gateway naming X in OrigClOrdID (41). A cancel that names Y cancels X. The gateway's refusals are
    // remembered, and the engine's.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, PARTFILL_ORDSTATUS, 100, 40, 60);
    const std::string source = FixGateway::makeSourceString(sid_);
    std::vector<IdT> replaced;
    std::vector<std::unique_ptr<OrderEntry>> replacements; // never reach an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .Times(2)
        .WillRepeatedly(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replaced.push_back(evt.id_);
                replacements.emplace_back(evt.replacementOrder_);
            }));
    std::vector<OrderCancelEvent> cancels;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderCancelEvent &evt)
            {
                cancels.push_back(evt);
            }));

    auto unknownSymbol = makeLimitReplace("ORD-X", "ORD-Y");
    unknownSymbol.set(FIX::Symbol("NO-SUCH"));
    receive(unknownSymbol);                      // refused by the gateway
    receive(makeLimitReplace("ORD-Y", "ORD-Z")); // a replace of X
    auto noPrice = makeLimitReplace("ORD-Y", "ORD-W");
    noPrice.set(FIX::Price(0));
    receive(noPrice);                             // refused by the gateway, as a replace of X
    receive(makeCancelRequest("ORD-Y", "CXL-Y")); // a cancel of X
    engineRefuses(sid_, "ORD-R", "ORD-X");
    receive(makeLimitReplace("ORD-R", "ORD-Q")); // a replace of X

    ASSERT_EQ(2u, replaced.size());
    const std::vector<std::string> replacedBy{ "ORD-Z", "ORD-Q" };
    for (size_t i = 0; i < replaced.size(); ++i)
    {
        SCOPED_TRACE(replacedBy[i]);
        EXPECT_EQ(x->orderId_, replaced[i]);
        const RawDataEntry &clOrd = replacements[i]->clOrderId_.get();
        EXPECT_EQ(FixGateway::sessionClOrdId(source, replacedBy[i]), std::string(clOrd.data_, clOrd.length_));
        const RawDataEntry &origClOrd = replacements[i]->origClOrderId_.get();
        EXPECT_EQ(FixGateway::sessionClOrdId(source, "ORD-X"), std::string(origClOrd.data_, origClOrd.length_));
    }
    ASSERT_EQ(1u, cancels.size());
    EXPECT_EQ(x->orderId_, cancels[0].id_);
    EXPECT_EQ("CXL-Y", cancels[0].requestClOrdId_);

    const std::vector<std::string> refused{ "ORD-Y", "ORD-W", "ORD-R" };
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_OrderCancelReject);
    ASSERT_EQ(refused.size(), rejects.size()) << sentText();
    for (size_t i = 0; i < rejects.size(); ++i)
    {
        SCOPED_TRACE(refused[i]);
        EXPECT_EQ(refused[i], fieldOf(rejects[i], FIX::FIELD::ClOrdID));
        EXPECT_EQ("ORD-X", fieldOf(rejects[i], FIX::FIELD::OrigClOrdID));
        EXPECT_EQ(std::to_string(x->orderId_.id_), fieldOf(rejects[i], FIX::FIELD::OrderID));
        EXPECT_EQ(std::to_string(FIX::CxlRejReason_OTHER), fieldOf(rejects[i], FIX::FIELD::CxlRejReason));
    }
    EXPECT_EQ(PARTFILL_ORDSTATUS, x->status_);
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, ARefusalForAClOrdIdInUseIsNotRemembered)
{
    // A replacement refused because its ClOrdID is in use (102=6) names another order by it, so a request that names
    // that ClOrdID isn't decided against the order the replacement was to replace (#398). Refused so by the engine, or
    // by the gateway while the new order with the ClOrdID was on its way and was then refused itself, the ClOrdID is
    // answered as an unknown order (102=1), as before #398.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, PARTFILL_ORDSTATUS, 100, 40, 60);
    const std::string source = FixGateway::makeSourceString(sid_);
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>())).Times(0);
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>())).Times(0);
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));

    Queues::OrderRejectEvent byEngine; // the engine's refusal of Replace (D,X), whose ClOrdID was in use
    byEngine.replacement_ = true;
    byEngine.duplicateClOrderId_ = true;
    byEngine.clOrderId_ = FixGateway::sessionClOrdId(source, "ORD-D");
    byEngine.origClOrderId_ = FixGateway::sessionClOrdId(source, "ORD-X");
    byEngine.refusal_ = CancelRejectEvent::OTHER;
    byEngine.reason_ = "Replace refused: ClOrdID ORD-D is already in use";
    gateway_->sendOrderReject(byEngine, source);
    receive(makeLimitReplace("ORD-D", "ORD-E"));
    receive(makeNewOrderSingle("ORD-P", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 70)); // on its way
    ASSERT_NE(nullptr, pushed);
    receive(makeLimitReplace("ORD-X", "ORD-P")); // refused by the gateway, ORD-P being in use (#403)
    engineRefuses(sid_, "ORD-P");                // and the new order by the engine
    receive(makeCancelRequest("ORD-P", "CXL-P"));

    struct Expected
    {
        std::string clOrdId, origClOrdId, orderId;
        int cxlRejReason;
    };
    const std::string xId = std::to_string(x->orderId_.id_);
    const std::vector<Expected> expected{ { "ORD-D", "ORD-X", xId, FIX::CxlRejReason_DUPLICATE_CL_ORD_ID },
                                          { "ORD-E", "ORD-D", "NONE", FIX::CxlRejReason_UNKNOWN_ORDER },
                                          { "ORD-P", "ORD-X", xId, FIX::CxlRejReason_DUPLICATE_CL_ORD_ID },
                                          { "CXL-P", "ORD-P", "NONE", FIX::CxlRejReason_UNKNOWN_ORDER } };
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_OrderCancelReject);
    ASSERT_EQ(expected.size(), rejects.size()) << sentText();
    for (size_t i = 0; i < rejects.size(); ++i)
    {
        SCOPED_TRACE(expected[i].clOrdId);
        EXPECT_EQ(expected[i].clOrdId, fieldOf(rejects[i], FIX::FIELD::ClOrdID));
        EXPECT_EQ(expected[i].origClOrdId, fieldOf(rejects[i], FIX::FIELD::OrigClOrdID));
        EXPECT_EQ(expected[i].orderId, fieldOf(rejects[i], FIX::FIELD::OrderID));
        EXPECT_EQ(std::to_string(expected[i].cxlRejReason), fieldOf(rejects[i], FIX::FIELD::CxlRejReason));
    }
    EXPECT_EQ(std::string(1, FIX::CxlRejResponseTo_ORDER_CANCEL_REQUEST),
              fieldOf(rejects[3], FIX::FIELD::CxlRejResponseTo));
    ASSERT_EQ(1u, sentOfType(FIX::MsgType_ExecutionReport).size()) << sentText(); // the new order's refusal
    expectEverythingSentIsTaken();
}

TEST_F(FixtSessionTest, ARefusedReplacementsClOrdIdNamesWhatItWasLastSentFor)
{
    // A ClOrdID names what the session sent it for last (#398). A refused replacement's, sent again for a new order that
    // is refused too, by the gateway or by the engine, names no order: a request that names it is answered as unknown
    // (102=1), not decided against the order the replace was for. Sent again for another replace that is refused, it
    // names the order that replace was for.
    ASSERT_NO_FATAL_FAILURE(logOn());
    OrderEntry *x = saveOrderOf(sid_, "ORD-X");
    setState(x, PARTFILL_ORDSTATUS, 100, 40, 60);
    OrderEntry *v = saveOrderOf(sid_, "ORD-V");
    setState(v, NEW_ORDSTATUS, 50, 0, 50);
    const std::string source = FixGateway::makeSourceString(sid_);
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent &>())).Times(0);
    std::vector<IdT> replaced;
    std::vector<std::unique_ptr<OrderEntry>> replacements; // never reach an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderReplaceEvent &evt)
            {
                replaced.push_back(evt.id_);
                replacements.emplace_back(evt.replacementOrder_);
            }));
    std::unique_ptr<OrderEntry> pushed; // never reaches an engine
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>()))
        .WillOnce(Invoke(
            [&](const std::string &, const OrderEvent &evt)
            {
                pushed.reset(evt.order_);
            }));
    auto refusedReplace = [&](const std::string &origClOrdId, const std::string &clOrdId)
    {
        auto replace = makeLimitReplace(origClOrdId, clOrdId);
        replace.set(FIX::Symbol("NO-SUCH")); // refused by the gateway
        receive(replace);
    };

    refusedReplace("ORD-X", "ORD-Y1");
    receive(makeNewOrderSingle("ORD-Y1", "NO-SUCH", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 70)); // the gateway's
    receive(makeLimitReplace("ORD-Y1", "ORD-Z1"));
    refusedReplace("ORD-X", "ORD-Y2");
    receive(makeNewOrderSingle("ORD-Y2", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 70));
    ASSERT_NE(nullptr, pushed);
    engineRefuses(sid_, "ORD-Y2");
    receive(makeCancelRequest("ORD-Y2", "CXL-Y2"));
    refusedReplace("ORD-X", "ORD-Y3");
    refusedReplace("ORD-V", "ORD-Y3");
    receive(makeLimitReplace("ORD-Y3", "ORD-Z3")); // a replace of V

    ASSERT_EQ(1u, replaced.size());
    EXPECT_EQ(v->orderId_, replaced[0]);
    const RawDataEntry &origClOrd = replacements[0]->origClOrderId_.get();
    EXPECT_EQ(FixGateway::sessionClOrdId(source, "ORD-V"), std::string(origClOrd.data_, origClOrd.length_));
    const std::vector<FIX::Message> rejects = sentOfType(FIX::MsgType_OrderCancelReject);
    ASSERT_EQ(6u, rejects.size()) << sentText(); // Y1, Z1, Y2, the cancel, and Y3 twice
    for (size_t i : { 1u, 3u })
    {
        SCOPED_TRACE(fieldOf(rejects[i], FIX::FIELD::ClOrdID));
        EXPECT_EQ("NONE", fieldOf(rejects[i], FIX::FIELD::OrderID));
        EXPECT_EQ(std::to_string(FIX::CxlRejReason_UNKNOWN_ORDER), fieldOf(rejects[i], FIX::FIELD::CxlRejReason));
    }
    EXPECT_EQ("ORD-Y1", fieldOf(rejects[1], FIX::FIELD::OrigClOrdID));
    EXPECT_EQ("ORD-Y2", fieldOf(rejects[3], FIX::FIELD::OrigClOrdID));
    ASSERT_EQ(2u, sentOfType(FIX::MsgType_ExecutionReport).size()) << sentText(); // the new orders' refusals
    expectEverythingSentIsTaken();
}

// =============================================================================
// WorkingIndicator (636), as EP284's E.1.f sends it (#387)
// =============================================================================

TEST_F(FixtSessionTest, AHeldStopSaysItIsNotWorkingUntilItsRelease)
{
    // A stop order held off the book is acknowledged with WorkingIndicator N, and its release (150=L) says Y. What else
    // describes the order while it is New says the same: a status answer, and the refusal of a new order that repeats
    // its ClOrdID (#382), which carries its state. A limit order, in the book at once, says Y, and an order that has
    // traded has no 636. A counterparty that validates takes each.
    ASSERT_NO_FATAL_FAILURE(logOn());
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent &>())).Times(0);
    OrderEntry *stop = saveOrderOf(sid_, "STOP-1");
    stop->ordType_ = STOP_ORDERTYPE;
    stop->stopPx_ = 10.5;
    setState(stop, NEW_ORDSTATUS, 100, 0, 100);
    const std::string stopId = std::to_string(stop->orderId_.id_);
    const auto ack = makeReport<ExecutionEntry>(NEW_EXECTYPE, NEW_ORDSTATUS);
    gateway_->sendExecutionReport(&ack, *stop);
    receive(makeStatusRequest("STOP-1"));
    receive(makeNewOrderSingle("STOP-1", "aaa", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100));
    stop->stopTriggered_ = true; // as its release leaves it
    const auto release = makeReport<ExecutionEntry>(TRIGGERED_EXECTYPE, NEW_ORDSTATUS);
    gateway_->sendExecutionReport(&release, *stop);
    receive(makeStatusRequest("STOP-1"));
    OrderEntry *limit = saveOrderOf(sid_, "LMT-1");
    setState(limit, NEW_ORDSTATUS, 100, 0, 100);
    gateway_->sendExecutionReport(&ack, *limit);
    receive(makeStatusRequest("LMT-1"));
    setState(limit, PARTFILL_ORDSTATUS, 100, 40, 60);
    receive(makeStatusRequest("LMT-1"));

    const std::vector<FIX::Message> reports = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(8u, reports.size()) << sentText();
    // The held stop: its acknowledgement, a status answer and a duplicate's refusal
    EXPECT_EQ(std::string(1, FIX::ExecType_NEW), fieldOf(reports[0], FIX::FIELD::ExecType));
    EXPECT_EQ("N", fieldOf(reports[0], FIX::FIELD::WorkingIndicator));
    expectStatus(reports[1], FIX::OrdStatus_NEW, stopId, 100, 0, 100);
    EXPECT_EQ("N", fieldOf(reports[1], FIX::FIELD::WorkingIndicator));
    expectDuplicateReject(reports[2], "STOP-1", FIX::OrdStatus_NEW, stopId, 100, 0, 100);
    EXPECT_EQ("N", fieldOf(reports[2], FIX::FIELD::WorkingIndicator));
    // Its release, and a status answer after it
    EXPECT_EQ(std::string(1, FIX::ExecType_TRIGGERED_OR_ACTIVATED_BY_SYSTEM),
              fieldOf(reports[3], FIX::FIELD::ExecType));
    EXPECT_EQ(std::string(1, FIX::OrdStatus_NEW), fieldOf(reports[3], FIX::FIELD::OrdStatus));
    EXPECT_EQ("Y", fieldOf(reports[3], FIX::FIELD::WorkingIndicator));
    expectStatus(reports[4], FIX::OrdStatus_NEW, stopId, 100, 0, 100);
    EXPECT_EQ("Y", fieldOf(reports[4], FIX::FIELD::WorkingIndicator));
    // The limit order: its acknowledgement, a status answer, and one once it has traded
    EXPECT_EQ(std::string(1, FIX::ExecType_NEW), fieldOf(reports[5], FIX::FIELD::ExecType));
    EXPECT_EQ("Y", fieldOf(reports[5], FIX::FIELD::WorkingIndicator));
    expectStatus(reports[6], FIX::OrdStatus_NEW, std::to_string(limit->orderId_.id_), 100, 0, 100);
    EXPECT_EQ("Y", fieldOf(reports[6], FIX::FIELD::WorkingIndicator));
    expectStatus(reports[7], FIX::OrdStatus_PARTIALLY_FILLED, std::to_string(limit->orderId_.id_), 100, 40, 60);
    EXPECT_EQ("(not set)", fieldOf(reports[7], FIX::FIELD::WorkingIndicator));
    expectEverythingSentIsTaken();
}

} // anonymous namespace

#endif // BUILD_FIX
