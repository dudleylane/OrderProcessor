/**
 Concurrent Order Processor library - PGWriteBehind Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <pqxx/pqxx>

#include "PGWriteBehind.h"
#include "PGWriteRequest.h"

using namespace COP;
using namespace COP::PG;

namespace
{

/// The database PG_CONNECTION_STRING names, which scripts/pg-for-tests.sh starts with the schema (#151)
std::string connectionString()
{
    const char *env = std::getenv("PG_CONNECTION_STRING");
    return (nullptr != env) ? std::string(env) : std::string();
}

/// The reference data every test order names, written ahead of it as the server's startup sync does
void writeReferenceData(PGWriteBehind &writer)
{
    writer.enqueue(PGWriteRequest{ InstrumentWrite{ "PGTEST_INSTR", "SECORD", "SRC" } });
    writer.enqueue(PGWriteRequest{ AccountWrite{ "PGTEST_ACCT", "PGTEST_FIRM", "AGENCY" } });
    writer.enqueue(PGWriteRequest{ ClearingWrite{ "PGTEST_CLR" } });
}

/// An order as PGRequestBuilder makes one. Its id was drawn on 2026-10-15 at 22:13:20 UTC: IdT::date_ is Unix seconds.
OrderWrite makeOrder(u64 orderId)
{
    OrderWrite w;
    w.orderId = orderId;
    w.orderDate = 1792022400 + 80000;
    w.clOrderId = "CL-" + std::to_string(orderId);
    w.source = "SRC";
    w.destination = "DST";
    w.instrumentSymbol = "PGTEST_INSTR";
    w.accountName = "PGTEST_ACCT";
    w.clearingFirm = "PGTEST_CLR";
    w.side = "BUY";
    w.ordType = "LIMIT";
    w.status = "NEW";
    w.tif = "DAY";
    w.capacity = "AGENCY";
    w.currency = "USD";
    w.settlType = "REGULAR";
    w.price = 100.5;
    w.stopPx = 0.0;
    w.avgPx = 0.0;
    w.dayAvgPx = 0.0;
    w.minQty = 0;
    w.orderQty = 500;
    w.leavesQty = 500;
    w.cumQty = 0;
    w.dayOrderQty = 0;
    w.dayCumQty = 0;
    w.expireTime = 0;
    w.settlDate = 0;
    return w;
}

class PGWriteBehindTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        connection_ = connectionString();
        if (connection_.empty())
        {
            GTEST_SKIP() << "PG_CONNECTION_STRING isn't set: scripts/pg-for-tests.sh starts a database for these";
        }
    }

    /// Removes what an earlier run left of an order, so a test sees only its own writes
    void forget(u64 orderId) const
    {
        pqxx::connection conn(connection_);
        pqxx::work txn(conn);
        txn.exec("DELETE FROM orders WHERE order_id = " + std::to_string(orderId));
        txn.commit();
    }

    /// The first value sql selects, as text; empty for no row or NULL
    std::string select(const std::string &sql) const
    {
        pqxx::connection conn(connection_);
        pqxx::nontransaction txn(conn);
        const pqxx::result rows = txn.exec(sql);
        return (rows.empty() || rows[0][0].is_null()) ? std::string() : rows[0][0].as<std::string>();
    }

    std::string connection_;
};

/// Whether ready() holds within ten seconds
template <typename Ready> bool eventually(Ready ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

/// connection with an application_name, by which pg_stat_activity tells the writer's backend from the test's own
std::string withApplicationName(const std::string &connection, const std::string &name)
{
    if ((0 == connection.rfind("postgresql://", 0)) || (0 == connection.rfind("postgres://", 0)))
    {
        return connection + ((std::string::npos == connection.find('?')) ? "?" : "&") + "application_name=" + name;
    }
    return connection + " application_name=" + name;
}

/// A local port nothing listens on
int closedPort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(addr);
    ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &length);
    ::close(fd);
    return ntohs(addr.sin_port);
}

} // anonymous namespace

TEST_F(PGWriteBehindTest, ReferenceDataAndAnOrderLand)
{
    forget(151001);
    PGWriteBehind writer(connection_);
    writeReferenceData(writer);
    writer.enqueue(PGWriteRequest{ makeOrder(151001) });
    writer.shutdown();

    EXPECT_EQ(4u, writer.totalEnqueued());
    EXPECT_EQ(4u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
    // Its instrument, account and clearing firm are found, not left NULL
    EXPECT_EQ("PGTEST_INSTR", select("SELECT i.symbol FROM orders o JOIN instruments i ON i.id = o.instrument_id "
                                     "WHERE o.order_id = 151001"));
    EXPECT_EQ("PGTEST_ACCT", select("SELECT a.account FROM orders o JOIN accounts a ON a.id = o.account_id "
                                    "WHERE o.order_id = 151001"));
}

TEST_F(PGWriteBehindTest, AnOrdersDatesAndTimesLandInTheirUnits)
{
    forget(151002);
    // order_date from IdT::date_'s Unix seconds, and the times from the engine's milliseconds. They were read as
    // YYYYMMDD and seconds, so a real order's dates were nonsense or an error (#151).
    PGWriteBehind writer(connection_);
    writeReferenceData(writer);
    OrderWrite order = makeOrder(151002);
    order.settlDate = 1794700800000ULL;  // 2026-11-15, midnight UTC, as the frontend sends a date
    order.expireTime = 1792022400123ULL; // 2026-10-15 00:00:00.123 UTC
    writer.enqueue(PGWriteRequest{ std::move(order) });
    writer.shutdown();
    ASSERT_EQ(0u, writer.totalErrors());

    EXPECT_EQ("2026-10-15", select("SELECT order_date::text FROM orders WHERE order_id = 151002"));
    EXPECT_EQ("2026-11-15", select("SELECT settl_date::text FROM orders WHERE order_id = 151002"));
    EXPECT_EQ(
        "1792022400123",
        select("SELECT (extract(epoch FROM expire_time) * 1000)::bigint::text FROM orders WHERE order_id = 151002"));
}

TEST_F(PGWriteBehindTest, ASwapsFarLegLandsInItsRow)
{
    forget(151009);
    forget(151010);
    // On this branch an order may be an FX swap, whose far leg is its own price and settlement date: without its
    // columns the mirror kept half of every swap. Any other order's far leg is NULL.
    PGWriteBehind writer(connection_);
    writeReferenceData(writer);
    OrderWrite swap = makeOrder(151009);
    swap.ordType = "FX_SWAP";
    swap.settlDate = 1794700800000ULL; // 2026-11-15, the near leg
    swap.farPrice = 1.0875;
    swap.farSettlDate = 1797379200000ULL; // 2026-12-16, the far leg
    writer.enqueue(PGWriteRequest{ std::move(swap) });
    writer.enqueue(PGWriteRequest{ makeOrder(151010) });
    writer.shutdown();
    ASSERT_EQ(0u, writer.totalErrors());

    EXPECT_EQ("1.08750000", select("SELECT far_price::text FROM orders WHERE order_id = 151009"));
    EXPECT_EQ("2026-12-16", select("SELECT far_settl_date::text FROM orders WHERE order_id = 151009"));
    EXPECT_EQ("2026-11-15", select("SELECT settl_date::text FROM orders WHERE order_id = 151009"));
    EXPECT_EQ(
        "true",
        select("SELECT (far_price IS NULL AND far_settl_date IS NULL)::text FROM orders WHERE order_id = 151010"));
}

TEST_F(PGWriteBehindTest, AQuantityAboveIntegersRangeLands)
{
    forget(151003);
    // The engine's quantities are unsigned 32-bit; they were cast to int, for INTEGER columns (#151)
    PGWriteBehind writer(connection_);
    writeReferenceData(writer);
    OrderWrite order = makeOrder(151003);
    order.orderQty = 4000000000u;
    order.leavesQty = 4000000000u;
    writer.enqueue(PGWriteRequest{ std::move(order) });
    writer.shutdown();
    ASSERT_EQ(0u, writer.totalErrors());

    EXPECT_EQ("4000000000", select("SELECT order_qty::text FROM orders WHERE order_id = 151003"));
    EXPECT_EQ("4000000000", select("SELECT leaves_qty::text FROM orders WHERE order_id = 151003"));
}

TEST_F(PGWriteBehindTest, AStopsReleaseReachesItsRow)
{
    // stop_triggered says whether a stop order's stop was reached (#236). The query copy couldn't tell a held stop from
    // a released one.
    forget(151236);
    OrderWrite held = makeOrder(151236);
    held.ordType = "STOPLIMIT";
    held.stopPx = 101.0;
    {
        PGWriteBehind writer(connection_);
        writeReferenceData(writer);
        writer.enqueue(PGWriteRequest{ OrderWrite(held) });
        writer.shutdown();
        ASSERT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("false", select("SELECT stop_triggered::text FROM orders WHERE order_id = 151236"));

    OrderWrite released = held;
    released.stopTriggered = true;
    {
        PGWriteBehind writer(connection_);
        writer.enqueue(PGWriteRequest{ std::move(released) });
        writer.shutdown();
        ASSERT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("true", select("SELECT stop_triggered::text FROM orders WHERE order_id = 151236"));
    forget(151236);
}

TEST_F(PGWriteBehindTest, ALaterWriteOfAnOrderReplacesItsRowAndADeleteRemovesIt)
{
    forget(151004);
    {
        PGWriteBehind writer(connection_);
        writeReferenceData(writer);
        writer.enqueue(PGWriteRequest{ makeOrder(151004) });
        OrderWrite filled = makeOrder(151004);
        filled.status = "FILLED";
        filled.price = 101.25;
        filled.cumQty = 500;
        filled.leavesQty = 0;
        writer.enqueue(PGWriteRequest{ std::move(filled) });
        writer.shutdown();
        ASSERT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("FILLED", select("SELECT status::text FROM orders WHERE order_id = 151004"));
    EXPECT_EQ("101.25000000", select("SELECT price::text FROM orders WHERE order_id = 151004"));
    EXPECT_EQ("1", select("SELECT count(*) FROM orders WHERE order_id = 151004"));

    // A rollback that leaves LMDB no version of the order (#151)
    {
        PGWriteBehind writer(connection_);
        writer.enqueue(PGWriteRequest{ DeleteOrderWrite{ 151004, 1792022400 + 80000 } });
        writer.shutdown();
        ASSERT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("0", select("SELECT count(*) FROM orders WHERE order_id = 151004"));
}

TEST_F(PGWriteBehindTest, ARequestPostgresRefusesDoesntStopTheRestOfItsBatch)
{
    forget(151005);
    PGWriteBehind writer(connection_);
    writer.enqueue(PGWriteRequest{ InstrumentWrite{ "PGTEST_BATCH_A", "S", "SRC" } });
    OrderWrite bad = makeOrder(151005);
    bad.status = "NO_SUCH_STATUS"; // the order_status type has no such value
    writer.enqueue(PGWriteRequest{ std::move(bad) });
    writer.enqueue(PGWriteRequest{ InstrumentWrite{ "PGTEST_BATCH_B", "S", "SRC" } });
    writer.shutdown();

    EXPECT_EQ(2u, writer.totalWritten());
    EXPECT_EQ(1u, writer.totalErrors());
    EXPECT_EQ("2", select("SELECT count(*) FROM instruments WHERE symbol IN ('PGTEST_BATCH_A', 'PGTEST_BATCH_B')"));
    EXPECT_EQ("0", select("SELECT count(*) FROM orders WHERE order_id = 151005"));
}

TEST_F(PGWriteBehindTest, ManyWritesLandInOrder)
{
    forget(151006);
    // More than a batch: each lands, and the last write of the order is the one its row keeps
    PGWriteBehind writer(connection_);
    writeReferenceData(writer);
    const unsigned writes = 3 * PGWriteBehind::BATCH_SIZE + 7;
    for (unsigned i = 1; i <= writes; ++i)
    {
        OrderWrite order = makeOrder(151006);
        order.cumQty = i;
        writer.enqueue(PGWriteRequest{ std::move(order) });
    }
    writer.shutdown();

    // Writes of the order that were still queued when a later one came were replaced, not written (#231)
    EXPECT_EQ(writes + 3, writer.totalWritten() + writer.totalCoalesced());
    EXPECT_EQ(0u, writer.totalErrors());
    EXPECT_EQ(std::to_string(writes), select("SELECT cum_qty::text FROM orders WHERE order_id = 151006"));
}

TEST_F(PGWriteBehindTest, AConnectionThatIsLostIsReplacedAndNothingIsLost)
{
    forget(151007);
    // The writer's backend is terminated between two writes, as a database restart or a firewall dropping an idle
    // connection would end it: the next write finds it gone, and lands on a new connection (#151)
    const std::string writers = "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'pgwb_151007'";
    PGWriteBehind writer(withApplicationName(connection_, "pgwb_151007"));
    writeReferenceData(writer);
    writer.enqueue(PGWriteRequest{ makeOrder(151007) });
    ASSERT_TRUE(eventually(
        [&]
        {
            return 4u == writer.totalWritten();
        }));
    ASSERT_EQ("1", select(writers));
    ASSERT_EQ("t", select("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                          "WHERE application_name = 'pgwb_151007'"));
    ASSERT_TRUE(eventually(
        [&]
        {
            return "0" == select(writers);
        }));

    OrderWrite filled = makeOrder(151007);
    filled.status = "FILLED";
    filled.cumQty = 500;
    filled.leavesQty = 0;
    writer.enqueue(PGWriteRequest{ std::move(filled) });
    writer.shutdown();

    EXPECT_EQ(5u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
    EXPECT_EQ(0u, writer.totalDropped());
    EXPECT_EQ("FILLED", select("SELECT status::text FROM orders WHERE order_id = 151007"));
}

TEST_F(PGWriteBehindTest, AConnectionEndedInTheMiddleOfAWriteIsReplacedAndNothingIsLost)
{
    forget(151008);
    // The writer's backend is terminated while its write waits for a lock the test holds, as a restart would end it in
    // the middle of a write: that write is lost with the connection, and lands on a new one (#151)
    const std::string writersBackend = " FROM pg_stat_activity WHERE application_name = 'pgwb_151008'";
    PGWriteBehind writer(withApplicationName(connection_, "pgwb_151008"));
    writeReferenceData(writer);
    writer.enqueue(PGWriteRequest{ makeOrder(151008) });
    ASSERT_TRUE(eventually(
        [&]
        {
            return 4u == writer.totalWritten();
        }));

    pqxx::connection locker(connection_);
    pqxx::work lock(locker);
    lock.exec("LOCK TABLE orders IN ACCESS EXCLUSIVE MODE");
    OrderWrite filled = makeOrder(151008);
    filled.status = "FILLED";
    filled.cumQty = 500;
    filled.leavesQty = 0;
    writer.enqueue(PGWriteRequest{ std::move(filled) });
    ASSERT_TRUE(eventually(
        [&]
        {
            return "Lock" == select("SELECT wait_event_type" + writersBackend);
        }));
    ASSERT_EQ("t", select("SELECT pg_terminate_backend(pid)" + writersBackend));
    lock.abort(); // the write, made again on the next connection, can take the table now
    writer.shutdown();

    EXPECT_EQ(5u, writer.totalWritten());
    EXPECT_EQ(0u, writer.totalErrors());
    EXPECT_EQ(0u, writer.totalDropped());
    EXPECT_EQ("FILLED", select("SELECT status::text FROM orders WHERE order_id = 151008"));
}

TEST(PGWriteBehindOfflineTest, ShutdownDoesntWaitForADatabaseThatIsntThere)
{
    // libpq waits for a connection forever by default, and the writer connects at shutdown too (#151)
    PGWriteBehind writer("host=127.0.0.1 port=" + std::to_string(closedPort()) + " dbname=oms user=oms");
    for (int i = 0; i < 3; ++i)
    {
        writer.enqueue(PGWriteRequest{ InstrumentWrite{ "OFFLINE_" + std::to_string(i), "S", "SRC" } });
    }
    const auto start = std::chrono::steady_clock::now();
    writer.shutdown();
    const auto took = std::chrono::steady_clock::now() - start;

    EXPECT_LT(took, std::chrono::seconds(15));
    EXPECT_EQ(0u, writer.totalWritten());
    EXPECT_EQ(3u, writer.totalDropped());
}

TEST(PGWriteBehindOfflineTest, AWriteOfARowStillQueuedReplacesTheQueuedOne)
{
    // While Postgres can't be reached, the queue holds a write per row, not one per change (#231). The writer takes the
    // first request at once, and keeps it while it waits to connect again, so the rest stay queued.
    PGWriteBehind writer("host=127.0.0.1 port=" + std::to_string(closedPort()) + " dbname=oms user=oms");
    writer.enqueue(PGWriteRequest{ InstrumentWrite{ "OFFLINE_A", "S", "SRC" } });
    ASSERT_TRUE(eventually(
        [&]
        {
            return writer.queuedRows().empty();
        }));

    OrderWrite first = makeOrder(231001);
    first.cumQty = 1;
    writer.enqueue(PGWriteRequest{ std::move(first) });
    OrderWrite second = makeOrder(231001);
    second.cumQty = 2;
    writer.enqueue(PGWriteRequest{ std::move(second) }); // replaces the first, in its place
    // The writer has OFFLINE_A's first write, so this one is queued again, behind it
    writer.enqueue(PGWriteRequest{ InstrumentWrite{ "OFFLINE_A", "S2", "SRC" } });
    writer.enqueue(PGWriteRequest{ makeOrder(231002) });
    // A rollback that left LMDB no version of 231001: the delete replaces its queued write
    writer.enqueue(PGWriteRequest{ DeleteOrderWrite{ 231001, makeOrder(231001).orderDate } });
    // 231001 keeps its first place, ahead of what was queued after it, as reference data stays ahead of its orders
    const std::string date = std::to_string(makeOrder(231001).orderDate);
    EXPECT_EQ((std::vector<std::string>{ "order 231001 " + date, "instrument OFFLINE_A", "order 231002 " + date }),
              writer.queuedRows());
    EXPECT_EQ(2u, writer.totalCoalesced());

    writer.shutdown();
    EXPECT_EQ(6u, writer.totalEnqueued());
    EXPECT_EQ(4u, writer.totalDropped()); // the one the writer held, and the three queued
    EXPECT_EQ(writer.totalEnqueued(),
              writer.totalWritten() + writer.totalErrors() + writer.totalDropped() + writer.totalCoalesced());
}

TEST(PGWriteBehindOfflineTest, AConnectTimeoutIsAddedWhenTheStringSetsNone)
{
    EXPECT_EQ("host=db dbname=oms connect_timeout=5", PGWriteBehind::withConnectTimeout("host=db dbname=oms"));
    EXPECT_EQ("host=db connect_timeout=2", PGWriteBehind::withConnectTimeout("host=db connect_timeout=2"));
    EXPECT_EQ("postgresql://u@db/oms?connect_timeout=5", PGWriteBehind::withConnectTimeout("postgresql://u@db/oms"));
    EXPECT_EQ("postgres://u@db/oms?sslmode=disable&connect_timeout=5",
              PGWriteBehind::withConnectTimeout("postgres://u@db/oms?sslmode=disable"));
    EXPECT_EQ("connect_timeout=5", PGWriteBehind::withConnectTimeout(""));
}
