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
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include "FixGateway.h"
#include "MultiOutQueues.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockQueues.h"

using namespace COP;
using namespace COP::App;
using namespace COP::Store;
using namespace COP::Queues;
using namespace test;
using ::testing::_;
using ::testing::Invoke;

namespace {

const FIX::SessionID TEST_SID("FIX.4.4", "CLIENT_A", "ORDER_PROCESSOR", "");

// =============================================================================
// Enum Conversion Tests
// =============================================================================

class FixEnumTest : public ::testing::Test {};

TEST_F(FixEnumTest, SideConversion) {
    EXPECT_EQ(BUY_SIDE,        FixGateway::toSide(FIX::Side_BUY));
    EXPECT_EQ(SELL_SIDE,       FixGateway::toSide(FIX::Side_SELL));
    EXPECT_EQ(SELL_SHORT_SIDE, FixGateway::toSide(FIX::Side_SELL_SHORT));
    EXPECT_EQ(INVALID_SIDE,    FixGateway::toSide('Z'));
}

TEST_F(FixEnumTest, SideRoundTrip) {
    EXPECT_EQ(FIX::Side_BUY,        FixGateway::fromSide(BUY_SIDE));
    EXPECT_EQ(FIX::Side_SELL,       FixGateway::fromSide(SELL_SIDE));
    EXPECT_EQ(FIX::Side_SELL_SHORT, FixGateway::fromSide(SELL_SHORT_SIDE));
    EXPECT_EQ(FIX::Side_CROSS,      FixGateway::fromSide(CROSS_SIDE));
}

TEST_F(FixEnumTest, OrdTypeConversion) {
    EXPECT_EQ(MARKET_ORDERTYPE,    FixGateway::toOrdType(FIX::OrdType_MARKET));
    EXPECT_EQ(LIMIT_ORDERTYPE,     FixGateway::toOrdType(FIX::OrdType_LIMIT));
    EXPECT_EQ(STOP_ORDERTYPE,      FixGateway::toOrdType(FIX::OrdType_STOP));
    EXPECT_EQ(STOPLIMIT_ORDERTYPE, FixGateway::toOrdType(FIX::OrdType_STOP_LIMIT));
    EXPECT_EQ(FXSWAP_ORDERTYPE,    FixGateway::toOrdType(FIX::OrdType_FOREX_SWAP));
    EXPECT_EQ(INVALID_ORDERTYPE,   FixGateway::toOrdType('Z'));
}

TEST_F(FixEnumTest, TifConversion) {
    EXPECT_EQ(DAY_TIF,     FixGateway::toTif(FIX::TimeInForce_DAY));
    EXPECT_EQ(GTC_TIF,     FixGateway::toTif(FIX::TimeInForce_GOOD_TILL_CANCEL));
    EXPECT_EQ(IOC_TIF,     FixGateway::toTif(FIX::TimeInForce_IMMEDIATE_OR_CANCEL));
    EXPECT_EQ(FOK_TIF,     FixGateway::toTif(FIX::TimeInForce_FILL_OR_KILL));
    EXPECT_EQ(GTD_TIF,     FixGateway::toTif(FIX::TimeInForce_GOOD_TILL_DATE));
    EXPECT_EQ(OPG_TIF,     FixGateway::toTif(FIX::TimeInForce_AT_THE_OPENING));
    EXPECT_EQ(ATCLOSE_TIF, FixGateway::toTif(FIX::TimeInForce_AT_THE_CLOSE));
    EXPECT_EQ(INVALID_TIF, FixGateway::toTif('Z'));
}

TEST_F(FixEnumTest, CurrencyConversion) {
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

TEST_F(FixEnumTest, OrdStatusConversion) {
    EXPECT_EQ(FIX::OrdStatus_NEW,              FixGateway::fromOrdStatus(NEW_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_PARTIALLY_FILLED, FixGateway::fromOrdStatus(PARTFILL_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_FILLED,           FixGateway::fromOrdStatus(FILLED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_CANCELED,         FixGateway::fromOrdStatus(CANCELED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_REJECTED,         FixGateway::fromOrdStatus(REJECTED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_REPLACED,         FixGateway::fromOrdStatus(REPLACED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_EXPIRED,          FixGateway::fromOrdStatus(EXPIRED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_SUSPENDED,        FixGateway::fromOrdStatus(SUSPENDED_ORDSTATUS));
    EXPECT_EQ(FIX::OrdStatus_DONE_FOR_DAY,     FixGateway::fromOrdStatus(DFD_ORDSTATUS));
}

TEST_F(FixEnumTest, ExecTypeConversion) {
    EXPECT_EQ(FIX::ExecType_NEW,            FixGateway::fromExecType(NEW_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_TRADE,          FixGateway::fromExecType(TRADE_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_CANCELED,       FixGateway::fromExecType(CANCEL_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_REJECTED,       FixGateway::fromExecType(REJECT_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_REPLACED,       FixGateway::fromExecType(REPLACE_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_EXPIRED,        FixGateway::fromExecType(EXPIRED_EXECTYPE));
    EXPECT_EQ(FIX::ExecType_PENDING_CANCEL, FixGateway::fromExecType(PEND_CANCEL_EXECTYPE));
}

// =============================================================================
// FixGateway Inbound Tests
// =============================================================================

class FixGatewayInboundTest : public ProcessorFixture {
protected:
    void SetUp() override {
        ProcessorFixture::SetUp();
        mockInQueues_ = std::make_unique<MockInQueues>();
        gateway_ = std::make_unique<FixGateway>(
            mockInQueues_.get(),
            WideDataStorage::instance(),
            OrderStorage::instance());
    }

    void TearDown() override {
        gateway_.reset();
        mockInQueues_.reset();
        ProcessorFixture::TearDown();
    }

    FIX44::NewOrderSingle makeNewOrderSingle(
        const std::string& clOrdId,
        const std::string& symbol,
        char side, char ordType,
        double price, double qty,
        char tif = FIX::TimeInForce_DAY)
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
            msg.set(FIX::Price(price));

        return msg;
    }

protected:
    std::unique_ptr<MockInQueues> mockInQueues_;
    std::unique_ptr<FixGateway> gateway_;
};

TEST_F(FixGatewayInboundTest, NewOrderSingle_PushesToQueue) {
    // Expect one OrderEvent push
    OrderEntry* capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent&>()))
        .WillOnce(Invoke([&](const std::string& source, const OrderEvent& evt) {
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

TEST_F(FixGatewayInboundTest, NewOrderSingle_MarketOrder) {
    OrderEntry* capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent&>()))
        .WillOnce(Invoke([&](const std::string&, const OrderEvent& evt) {
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

TEST_F(FixGatewayInboundTest, NewOrderSingle_UnknownSymbol_NoPush) {
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent&>())).Times(0);

    auto msg = makeNewOrderSingle("ORD003", "NONEXISTENT", FIX::Side_BUY, FIX::OrdType_LIMIT, 10.0, 100);
    gateway_->onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, CancelRequest_PushesToQueue) {
    // First create an order so we can cancel it
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry* saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);

    // Get the clOrderId string
    const auto& clOrd = saved->clOrderId_.get();
    std::string clOrdStr(clOrd.data_, clOrd.length_);

    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderCancelEvent&>()))
        .WillOnce(Invoke([&](const std::string& source, const OrderCancelEvent& evt) {
            EXPECT_TRUE(source.find("FIX:") != std::string::npos);
            EXPECT_EQ(saved->orderId_, evt.id_);
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

TEST_F(FixGatewayInboundTest, ReplaceRequest_PushesToQueue) {
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry* saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    ASSERT_NE(nullptr, saved);
    saved->status_ = NEW_ORDSTATUS;

    const auto& clOrd = saved->clOrderId_.get();
    std::string clOrdStr(clOrd.data_, clOrd.length_);

    OrderEntry* capturedReplacement = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderReplaceEvent&>()))
        .WillOnce(Invoke([&](const std::string& source, const OrderReplaceEvent& evt) {
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
    EXPECT_DOUBLE_EQ(15.50, capturedReplacement->price_);
    EXPECT_EQ(200u, capturedReplacement->orderQty_);
    EXPECT_EQ(200u, capturedReplacement->leavesQty_);

    delete capturedReplacement;
}

// =============================================================================
// NewOrderMultileg — FX Swap via standard FIX
// =============================================================================

TEST_F(FixGatewayInboundTest, NewOrderMultileg_FxSwap_PushesToQueue) {
    OrderEntry* capturedOrder = nullptr;
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent&>()))
        .WillOnce(Invoke([&](const std::string& source, const OrderEvent& evt) {
            EXPECT_TRUE(source.find("FIX:") != std::string::npos);
            capturedOrder = evt.order_;
        }));

    FIX::UtcTimeStamp now;
    FIX44::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("SWAP-001"));
    msg.set(FIX::Side(FIX::Side_BUY));  // near-leg side
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
    EXPECT_DOUBLE_EQ(1.2650, capturedOrder->price_);     // near (matches root side)
    EXPECT_DOUBLE_EQ(1.2680, capturedOrder->farPrice_);  // far (opposite side)
    EXPECT_EQ(1000u, capturedOrder->settlDate_);
    EXPECT_EQ(2000u, capturedOrder->farSettlDate_);
    EXPECT_EQ(1000000u, capturedOrder->orderQty_);

    delete capturedOrder;
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_NonSwapOrdType_Rejected) {
    // NewOrderMultileg with non-FXSwap OrdType should be rejected (no push)
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent&>())).Times(0);

    FIX::UtcTimeStamp now;
    FIX44::NewOrderMultileg msg;
    msg.set(FIX::ClOrdID("MLEG-BAD"));
    msg.set(FIX::Side(FIX::Side_BUY));
    msg.set(FIX::TransactTime(now));
    msg.set(FIX::OrdType(FIX::OrdType_LIMIT));  // not FX Swap
    msg.set(FIX::Symbol("aaa"));
    msg.set(FIX::OrderQty(100));

    gateway_->onMessage(msg, TEST_SID);
}

TEST_F(FixGatewayInboundTest, NewOrderMultileg_TooFewLegs_Rejected) {
    EXPECT_CALL(*mockInQueues_, push(_, testing::An<const OrderEvent&>())).Times(0);

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

TEST_F(FixGatewayInboundTest, SessionMapPopulatedOnLogon) {
    std::string source = FixGateway::makeSourceString(TEST_SID);

    EXPECT_FALSE(gateway_->hasFixSession(source));
    gateway_->onLogon(TEST_SID);
    EXPECT_TRUE(gateway_->hasFixSession(source));
    gateway_->onLogout(TEST_SID);
    EXPECT_FALSE(gateway_->hasFixSession(source));
}

TEST_F(FixGatewayInboundTest, MakeSourceString_Format) {
    std::string source = FixGateway::makeSourceString(TEST_SID);
    EXPECT_EQ("FIX:CLIENT_A->ORDER_PROCESSOR", source);
}

// =============================================================================
// MultiOutQueues Tests
// =============================================================================

class MultiOutQueuesTest : public ::testing::Test {};

TEST_F(MultiOutQueuesTest, FansOutToAllDelegates) {
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    ExecReportEvent evt(nullptr);
    EXPECT_CALL(mock1, push(testing::An<const ExecReportEvent&>(), _)).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const ExecReportEvent&>(), _)).Times(1);
    multi.push(evt, "target");
}

TEST_F(MultiOutQueuesTest, FansOutCancelReject) {
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    CancelRejectEvent evt;
    EXPECT_CALL(mock1, push(testing::An<const CancelRejectEvent&>(), _)).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const CancelRejectEvent&>(), _)).Times(1);
    multi.push(evt, "target");
}

TEST_F(MultiOutQueuesTest, FansOutBusinessReject) {
    MockOutQueues mock1, mock2;
    MultiOutQueues multi;
    multi.addDelegate(&mock1);
    multi.addDelegate(&mock2);

    BusinessRejectEvent evt;
    EXPECT_CALL(mock1, push(testing::An<const BusinessRejectEvent&>(), _)).Times(1);
    EXPECT_CALL(mock2, push(testing::An<const BusinessRejectEvent&>(), _)).Times(1);
    multi.push(evt, "target");
}

TEST_F(MultiOutQueuesTest, EmptyDelegatesNoCrash) {
    MultiOutQueues multi;
    ExecReportEvent evt(nullptr);
    multi.push(evt, "target");
    SUCCEED();
}

} // anonymous namespace

#endif // BUILD_FIX
