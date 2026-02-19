/**
 Concurrent Order Processor library - PGWriteBehind Integration Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 These tests require a live PostgreSQL instance.
 Set the PG_CONNECTION_STRING environment variable to enable.
*/

#include <gtest/gtest.h>
#include <cstdlib>
#include <thread>
#include <chrono>

#include "PGWriteBehind.h"
#include "PGWriteRequest.h"

using namespace COP::PG;

namespace {

std::string getConnString() {
    const char* env = std::getenv("PG_CONNECTION_STRING");
    return env ? std::string(env) : std::string();
}

} // anonymous namespace

class PGWriteBehindTest : public ::testing::Test {
protected:
    void SetUp() override {
        connString_ = getConnString();
        if (connString_.empty())
            GTEST_SKIP() << "PG_CONNECTION_STRING not set; skipping PGWriteBehind integration tests";
    }

    std::string connString_;
};

TEST_F(PGWriteBehindTest, EnqueueInstrumentAndShutdown) {
    PGWriteBehind writer(connString_);

    InstrumentWrite w{"TEST_INSTR", "SEC001", "CUSIP"};
    writer.enqueue(PGWriteRequest{std::move(w)});

    EXPECT_EQ(1u, writer.totalEnqueued());

    writer.shutdown();

    EXPECT_EQ(1u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
}

TEST_F(PGWriteBehindTest, EnqueueAccountAndShutdown) {
    PGWriteBehind writer(connString_);

    AccountWrite w{"TEST_ACCT", "TEST_FIRM", "PRINCIPAL"};
    writer.enqueue(PGWriteRequest{std::move(w)});

    writer.shutdown();

    EXPECT_EQ(1u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
}

TEST_F(PGWriteBehindTest, EnqueueClearingAndShutdown) {
    PGWriteBehind writer(connString_);

    ClearingWrite w{"TEST_CLR_FIRM"};
    writer.enqueue(PGWriteRequest{std::move(w)});

    writer.shutdown();

    EXPECT_EQ(1u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
}

TEST_F(PGWriteBehindTest, EnqueueOrderAndShutdown) {
    PGWriteBehind writer(connString_);

    // Ensure reference data exists first
    writer.enqueue(PGWriteRequest{InstrumentWrite{"TEST_ORD_INSTR", "SECORD", "SRC"}});
    writer.enqueue(PGWriteRequest{AccountWrite{"TEST_ORD_ACCT", "TEST_ORD_FIRM", "AGENCY"}});
    writer.enqueue(PGWriteRequest{ClearingWrite{"TEST_ORD_CLR"}});

    OrderWrite ow;
    ow.orderId = 99999;
    ow.orderDate = 20260212;
    ow.clOrderId = "CL001";
    ow.origClOrderId = "";
    ow.source = "SRC";
    ow.destination = "DST";
    ow.instrumentSymbol = "TEST_ORD_INSTR";
    ow.accountName = "TEST_ORD_ACCT";
    ow.clearingFirm = "TEST_ORD_CLR";
    ow.side = "BUY";
    ow.ordType = "LIMIT";
    ow.status = "RECEIVED_NEW";
    ow.tif = "DAY";
    ow.capacity = "AGENCY";
    ow.currency = "USD";
    ow.settlType = "REGULAR";
    ow.price = 100.50;
    ow.stopPx = 0.0;
    ow.avgPx = 0.0;
    ow.dayAvgPx = 0.0;
    ow.minQty = 0;
    ow.orderQty = 500;
    ow.leavesQty = 500;
    ow.cumQty = 0;
    ow.dayOrderQty = 0;
    ow.dayCumQty = 0;
    ow.expireTime = 0;
    ow.settlDate = 0;

    writer.enqueue(PGWriteRequest{std::move(ow)});

    writer.shutdown();

    EXPECT_EQ(4u, writer.totalEnqueued());
    EXPECT_EQ(4u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
}

TEST_F(PGWriteBehindTest, MultipleEnqueuesAndCounters) {
    PGWriteBehind writer(connString_);

    for (int i = 0; i < 10; ++i) {
        std::string symbol = "MULTI_INSTR_" + std::to_string(i);
        writer.enqueue(PGWriteRequest{InstrumentWrite{symbol, "SEC", "SRC"}});
    }

    EXPECT_EQ(10u, writer.totalEnqueued());

    writer.shutdown();

    EXPECT_EQ(10u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
}
