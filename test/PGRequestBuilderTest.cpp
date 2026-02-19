/**
 Concurrent Order Processor library - PGRequestBuilder Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include "PGRequestBuilder.h"
#include "PGEnumStrings.h"
#include "TestFixtures.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::PG;

class PGRequestBuilderTest : public test::SingletonFixture {};

TEST_F(PGRequestBuilderTest, FromInstrument) {
    InstrumentEntry instr;
    instr.symbol_ = "AAPL";
    instr.securityId_ = "037833100";
    instr.securityIdSource_ = "CUSIP";

    auto w = PGRequestBuilder::fromInstrument(instr);

    EXPECT_EQ("AAPL", w.symbol);
    EXPECT_EQ("037833100", w.securityId);
    EXPECT_EQ("CUSIP", w.securityIdSource);
}

TEST_F(PGRequestBuilderTest, FromAccount) {
    AccountEntry acct;
    acct.account_ = "ACCT001";
    acct.firm_ = "FirmA";
    acct.type_ = PRINCIPAL_ACCOUNTTYPE;

    auto w = PGRequestBuilder::fromAccount(acct);

    EXPECT_EQ("ACCT001", w.account);
    EXPECT_EQ("FirmA", w.firm);
    EXPECT_EQ("PRINCIPAL", w.type);
}

TEST_F(PGRequestBuilderTest, FromAccountAgency) {
    AccountEntry acct;
    acct.account_ = "ACCT002";
    acct.firm_ = "FirmB";
    acct.type_ = AGENCY_ACCOUNTTYPE;

    auto w = PGRequestBuilder::fromAccount(acct);

    EXPECT_EQ("AGENCY", w.type);
}

TEST_F(PGRequestBuilderTest, FromClearing) {
    ClearingEntry clr;
    clr.firm_ = "CLRFirm";

    auto w = PGRequestBuilder::fromClearing(clr);

    EXPECT_EQ("CLRFirm", w.firm);
}

TEST_F(PGRequestBuilderTest, FromOrder) {
    auto order = test::createCorrectOrder();

    // Assign a known order ID
    order->orderId_ = IdT(12345, 20260212);

    auto w = PGRequestBuilder::fromOrder(*order);

    // Check order ID fields
    EXPECT_EQ(12345u, w.orderId);
    EXPECT_EQ(20260212u, w.orderDate);

    // Check resolved instrument
    EXPECT_EQ("AAASymbl", w.instrumentSymbol);

    // Check resolved account
    EXPECT_EQ("ACT", w.accountName);

    // Check resolved clearing
    EXPECT_EQ("CLRFirm", w.clearingFirm);

    // Check resolved source/destination
    EXPECT_EQ("CLNT", w.source);
    EXPECT_EQ("NASDAQ", w.destination);

    // Check resolved clOrderId
    EXPECT_EQ("TestClOrderId", w.clOrderId);

    // Check enum mappings
    EXPECT_EQ("BUY", w.side);
    EXPECT_EQ("LIMIT", w.ordType);
    EXPECT_EQ("RECEIVED_NEW", w.status);
    EXPECT_EQ("DAY", w.tif);
    EXPECT_EQ("PRINCIPAL", w.capacity);
    EXPECT_EQ("USD", w.currency);
    EXPECT_EQ("T_PLUS_2", w.settlType);

    // Check scalar fields
    EXPECT_DOUBLE_EQ(1.46, w.price);
    EXPECT_EQ(77u, w.orderQty);
    EXPECT_EQ(175u, w.expireTime);
    EXPECT_EQ(225u, w.settlDate);
}

TEST_F(PGRequestBuilderTest, FromOrderCheckAllQuantities) {
    auto order = test::createCorrectOrder();
    order->orderId_ = IdT(99, 20260101);
    order->minQty_ = 5;
    order->leavesQty_ = 50;
    order->cumQty_ = 27;
    order->dayOrderQty_ = 100;
    order->dayCumQty_ = 30;
    order->avgPx_ = 1.50;
    order->dayAvgPx_ = 1.48;
    order->stopPx_ = 1.40;

    auto w = PGRequestBuilder::fromOrder(*order);

    EXPECT_EQ(5u, w.minQty);
    EXPECT_EQ(50u, w.leavesQty);
    EXPECT_EQ(27u, w.cumQty);
    EXPECT_EQ(100u, w.dayOrderQty);
    EXPECT_EQ(30u, w.dayCumQty);
    EXPECT_DOUBLE_EQ(1.50, w.avgPx);
    EXPECT_DOUBLE_EQ(1.48, w.dayAvgPx);
    EXPECT_DOUBLE_EQ(1.40, w.stopPx);
}
