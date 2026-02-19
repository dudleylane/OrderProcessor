/**
 Concurrent Order Processor library - PGEnumStrings Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).
*/

#include <gtest/gtest.h>
#include "PGEnumStrings.h"

using namespace COP;
using namespace COP::PG;

// --- Side ---

TEST(PGEnumStringsTest, SideMappings) {
    EXPECT_STREQ("BUY",        toSQL(BUY_SIDE));
    EXPECT_STREQ("SELL",       toSQL(SELL_SIDE));
    EXPECT_STREQ("BUY_MINUS",  toSQL(BUY_MINUS_SIDE));
    EXPECT_STREQ("SELL_PLUS",  toSQL(SELL_PLUS_SIDE));
    EXPECT_STREQ("SELL_SHORT", toSQL(SELL_SHORT_SIDE));
    EXPECT_STREQ("CROSS",      toSQL(CROSS_SIDE));
    EXPECT_EQ(nullptr,         toSQL(INVALID_SIDE));
}

// --- OrderType ---

TEST(PGEnumStringsTest, OrderTypeMappings) {
    EXPECT_STREQ("MARKET",    toSQL(MARKET_ORDERTYPE));
    EXPECT_STREQ("LIMIT",     toSQL(LIMIT_ORDERTYPE));
    EXPECT_STREQ("STOP",      toSQL(STOP_ORDERTYPE));
    EXPECT_STREQ("STOPLIMIT", toSQL(STOPLIMIT_ORDERTYPE));
    EXPECT_EQ(nullptr,        toSQL(INVALID_ORDERTYPE));
}

// --- OrderStatus ---

TEST(PGEnumStringsTest, OrderStatusMappings) {
    EXPECT_STREQ("RECEIVED_NEW",    toSQL(RECEIVEDNEW_ORDSTATUS));
    EXPECT_STREQ("REJECTED",        toSQL(REJECTED_ORDSTATUS));
    EXPECT_STREQ("PENDING_NEW",     toSQL(PENDINGNEW_ORDSTATUS));
    EXPECT_STREQ("PENDING_REPLACE", toSQL(PENDINGREPLACE_ORDSTATUS));
    EXPECT_STREQ("NEW",             toSQL(NEW_ORDSTATUS));
    EXPECT_STREQ("PARTIAL_FILL",    toSQL(PARTFILL_ORDSTATUS));
    EXPECT_STREQ("FILLED",          toSQL(FILLED_ORDSTATUS));
    EXPECT_STREQ("EXPIRED",         toSQL(EXPIRED_ORDSTATUS));
    EXPECT_STREQ("DONE_FOR_DAY",    toSQL(DFD_ORDSTATUS));
    EXPECT_STREQ("SUSPENDED",       toSQL(SUSPENDED_ORDSTATUS));
    EXPECT_STREQ("REPLACED",        toSQL(REPLACED_ORDSTATUS));
    EXPECT_STREQ("CANCELED",        toSQL(CANCELED_ORDSTATUS));
    EXPECT_EQ(nullptr,              toSQL(INVALID_ORDSTATUS));
}

// --- TimeInForce ---

TEST(PGEnumStringsTest, TimeInForceMappings) {
    EXPECT_STREQ("DAY",     toSQL(DAY_TIF));
    EXPECT_STREQ("GTD",     toSQL(GTD_TIF));
    EXPECT_STREQ("GTC",     toSQL(GTC_TIF));
    EXPECT_STREQ("FOK",     toSQL(FOK_TIF));
    EXPECT_STREQ("IOC",     toSQL(IOC_TIF));
    EXPECT_STREQ("OPG",     toSQL(OPG_TIF));
    EXPECT_STREQ("ATCLOSE", toSQL(ATCLOSE_TIF));
    EXPECT_EQ(nullptr,      toSQL(INVALID_TIF));
}

// --- Capacity ---

TEST(PGEnumStringsTest, CapacityMappings) {
    EXPECT_STREQ("AGENCY",                   toSQL(AGENCY_CAPACITY));
    EXPECT_STREQ("PRINCIPAL",                toSQL(PRINCIPAL_CAPACITY));
    EXPECT_STREQ("PROPRIETARY",              toSQL(PROPRIETARY_CAPACITY));
    EXPECT_STREQ("INDIVIDUAL",               toSQL(INDIVIDUAL_CAPACITY));
    EXPECT_STREQ("RISKLESS_PRINCIPAL",       toSQL(RISKLESS_PRINCIPAL_CAPACITY));
    EXPECT_STREQ("AGENT_FOR_ANOTHER_MEMBER", toSQL(AGENT_FOR_ANOTHER_MEMBER_CAPACITY));
    EXPECT_EQ(nullptr,                       toSQL(INVALID_CAPACITY));
}

// --- Currency ---

TEST(PGEnumStringsTest, CurrencyMappings) {
    EXPECT_STREQ("USD", toSQL(USD_CURRENCY));
    EXPECT_STREQ("EUR", toSQL(EUR_CURRENCY));
    EXPECT_EQ(nullptr,  toSQL(INVALID_CURRENCY));
}

// --- SettlTypeBase ---

TEST(PGEnumStringsTest, SettlTypeMappings) {
    EXPECT_STREQ("REGULAR",        toSQL(_0_SETTLTYPE));
    EXPECT_STREQ("CASH",           toSQL(_1_SETTLTYPE));
    EXPECT_STREQ("NEXT_DAY",       toSQL(_2_SETTLTYPE));
    EXPECT_STREQ("T_PLUS_2",       toSQL(_3_SETTLTYPE));
    EXPECT_STREQ("T_PLUS_3",       toSQL(_4_SETTLTYPE));
    EXPECT_STREQ("T_PLUS_4",       toSQL(_5_SETTLTYPE));
    EXPECT_STREQ("T_PLUS_5",       toSQL(_6_SETTLTYPE));
    EXPECT_STREQ("SELLERS_OPTION", toSQL(_7_SETTLTYPE));
    EXPECT_STREQ("WHEN_ISSUED",    toSQL(_8_SETTLTYPE));
    EXPECT_STREQ("T_PLUS_1",       toSQL(_9_SETTLTYPE));
    EXPECT_STREQ("BUYERS_OPTION",  toSQL(_B_SETTLTYPE));
    EXPECT_STREQ("SPECIAL_TRADE",  toSQL(_C_SETTLTYPE));
    EXPECT_STREQ("TENOR",          toSQL(TENOR_SETTLTYPE));
    EXPECT_EQ(nullptr,             toSQL(INVALID_SETTLTYPE));
}

// --- AccountType ---

TEST(PGEnumStringsTest, AccountTypeMappings) {
    EXPECT_STREQ("PRINCIPAL", toSQL(PRINCIPAL_ACCOUNTTYPE));
    EXPECT_STREQ("AGENCY",    toSQL(AGENCY_ACCOUNTTYPE));
    EXPECT_EQ(nullptr,        toSQL(INVALID_ACCOUNTTYPE));
}
