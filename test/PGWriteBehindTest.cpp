/**
 Concurrent Order Processor library - PGWriteBehind Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <pqxx/pqxx>

#include "PGRequestBuilder.h"
#include "PGWriteBehind.h"
#include "PGWriteRequest.h"
#include "TestAux.h"

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

/// connection whose search path is schema first, then public: a table of schema's hides public's table of that name
std::string withSearchPath(const std::string &connection, const std::string &schema)
{
    if ((0 == connection.rfind("postgresql://", 0)) || (0 == connection.rfind("postgres://", 0)))
    {
        return connection + ((std::string::npos == connection.find('?')) ? "?" : "&") + "options=-csearch_path%3D" +
               schema + "%2Cpublic";
    }
    return connection + " options='-csearch_path=" + schema + ",public'";
}

/// How many of lines contain text
std::size_t linesWith(const std::vector<std::string> &lines, const std::string &text)
{
    return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(),
                                                  [&text](const std::string &line)
                                                  {
                                                      return std::string::npos != line.find(text);
                                                  }));
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

TEST_F(PGWriteBehindTest, APriceOfTenBillionOrMoreLands)
{
    forget(151538);
    // The price columns were NUMERIC(18,8), which refused 1e10 or more, though the engine accepts any finite price
    // (#538)
    PGWriteBehind writer(connection_);
    writeReferenceData(writer);
    OrderWrite order = makeOrder(151538);
    order.price = 1e12;
    order.stopPx = 2.5e10;
    order.avgPx = 1.5e10;
    order.dayAvgPx = 1.25e10;
    writer.enqueue(PGWriteRequest{ std::move(order) });
    writer.shutdown();
    ASSERT_EQ(0u, writer.totalErrors());

    EXPECT_EQ("true", select("SELECT (price = 1000000000000 AND stop_px = 25000000000 AND avg_px = 15000000000 AND "
                             "day_avg_px = 12500000000)::text FROM orders WHERE order_id = 151538"));
    forget(151538);
}

TEST_F(PGWriteBehindTest, AStatementTheSchemaRefusesIsLoggedOnceAndTheWritesLandOnceItMatches)
{
    // A schema that refused one of the writer's statements was logged as "can't connect to PostgreSQL" at every retry,
    // and read as a database out of reach (#590). Here the writer's search path finds a schema of its own first, whose
    // orders table lacks the order upsert's columns, so the upsert can't be prepared. Once that table is dropped, the
    // path finds public's, and the queued writes land.
    const std::string schema = "pgtest_590_" + std::to_string(::getpid());
    const auto run = [this](const std::string &sql)
    {
        pqxx::connection conn(connection_);
        pqxx::work txn(conn);
        txn.exec(sql);
        txn.commit();
    };
    run("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
    run("CREATE SCHEMA " + schema);
    run("CREATE TABLE " + schema + ".orders (order_id BIGINT, order_date TIMESTAMPTZ)");
    forget(151590);

    test::CapturedLog log;
    u64 refusals = 0;
    {
        PGWriteBehind writer(withSearchPath(connection_, schema));
        writeReferenceData(writer);
        writer.enqueue(PGWriteRequest{ makeOrder(151590) });
        EXPECT_TRUE(eventually(
            [&writer]
            {
                return 2 <= writer.totalSchemaRefusals();
            }))
            << "the writer didn't try again after the schema refused its statement";
        EXPECT_EQ(0u, writer.totalWritten());

        run("DROP TABLE " + schema + ".orders");
        EXPECT_TRUE(eventually(
            [&writer]
            {
                return 4 <= writer.totalWritten();
            }))
            << "the writes didn't land once the schema matched";
        writer.shutdown();
        refusals = writer.totalSchemaRefusals();
        EXPECT_EQ(0u, writer.totalErrors());
        EXPECT_EQ(0u, writer.totalDropped());
    }
    // The writer's thread has ended, so the log can be read (TestAux.h: not while a thread the test started logs)
    const std::vector<std::string> lines = log.lines();
    EXPECT_LE(2u, refusals);
    EXPECT_EQ(1u, linesWith(lines, "schema refused a statement")) << "each refusal of a run was logged";
    EXPECT_EQ(0u, linesWith(lines, "can't connect")) << "a refused statement was logged as a connection failure";
    EXPECT_EQ(1u, linesWith(lines, "schema takes the server's statements now"));
    EXPECT_EQ("NEW", select("SELECT status::text FROM orders WHERE order_id = 151590"));

    run("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
    forget(151590);
}

TEST_F(PGWriteBehindTest, ASchemaOfAnotherVersionIsRefusedOnceAndTheWritesLandOnceItIsMigrated)
{
    // The writer reads the schema's version as it connects (#590), and refuses one of another version than its
    // statements are written for, as it refuses a statement the schema refuses: logged once, counted, tried again. Here
    // its search path finds a schema of its own first, whose schema_version is one ahead; once it reads the server's,
    // the queued writes land.
    const std::string schema = "pgtest_590v_" + std::to_string(::getpid());
    const std::string ahead = std::to_string(COP::PG::SCHEMA_VERSION + 1);
    const auto run = [this](const std::string &sql)
    {
        pqxx::connection conn(connection_);
        pqxx::work txn(conn);
        txn.exec(sql);
        txn.commit();
    };
    run("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
    run("CREATE SCHEMA " + schema);
    run("CREATE TABLE " + schema + ".schema_version (version INTEGER PRIMARY KEY)");
    run("INSERT INTO " + schema + ".schema_version VALUES (" + ahead + ")");
    forget(151591);

    test::CapturedLog log;
    u64 refusals = 0;
    {
        PGWriteBehind writer(withSearchPath(connection_, schema));
        writeReferenceData(writer);
        writer.enqueue(PGWriteRequest{ makeOrder(151591) });
        EXPECT_TRUE(eventually(
            [&writer]
            {
                return 2 <= writer.totalSchemaRefusals();
            }))
            << "the writer didn't try again after refusing the schema's version";
        EXPECT_EQ(0u, writer.totalWritten());

        run("UPDATE " + schema + ".schema_version SET version = " + std::to_string(COP::PG::SCHEMA_VERSION));
        EXPECT_TRUE(eventually(
            [&writer]
            {
                return 4 <= writer.totalWritten();
            }))
            << "the writes didn't land once the schema was at the server's version";
        writer.shutdown();
        refusals = writer.totalSchemaRefusals();
        EXPECT_EQ(0u, writer.totalErrors());
        EXPECT_EQ(0u, writer.totalDropped());
    }
    const std::vector<std::string> lines = log.lines();
    EXPECT_LE(2u, refusals);
    EXPECT_EQ(1u, linesWith(lines, "schema is version " + ahead + ", and the server's statements are written for"))
        << "each refusal of a run was logged";
    EXPECT_EQ(0u, linesWith(lines, "can't connect"));
    EXPECT_EQ(1u, linesWith(lines, "schema takes the server's statements now"));
    EXPECT_EQ("NEW", select("SELECT status::text FROM orders WHERE order_id = 151591"));

    run("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
    forget(151591);
}

TEST_F(PGWriteBehindTest, ASchemaMadeBeforeVersioningIsMirroredWithAWarning)
{
    // A volume made before the schema was versioned has no schema_version (#590): the writer warns, and mirrors to it
    // as before, since refusing would stop every existing mirror at the upgrade. It does so on every connection, with
    // the warning once a run: the writer's backend is terminated between two writes, and the second lands on a new
    // connection with no second warning. public's table is renamed for the case, and named back after it.
    const std::string hidden = "schema_version_590_" + std::to_string(::getpid());
    const auto run = [this](const std::string &sql)
    {
        pqxx::connection conn(connection_);
        pqxx::work txn(conn);
        txn.exec(sql);
        txn.commit();
    };
    run("ALTER TABLE schema_version RENAME TO " + hidden);
    // Named back however the case ends: an assertion that stops it, or an exception, would otherwise leave the cases
    // after it, which share the database, with no schema_version
    struct AtExit
    {
        std::function<void()> fn_;
        ~AtExit()
        {
            fn_();
        }
    };
    const AtExit nameBack{ [&]
                           {
                               try
                               {
                                   run("ALTER TABLE " + hidden + " RENAME TO schema_version");
                               }
                               catch (const std::exception &e)
                               {
                                   ADD_FAILURE() << "public's schema_version wasn't named back: " << e.what();
                               }
                           } };
    forget(151592);

    test::CapturedLog log;
    {
        const std::string writers = "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'pgwb_151592'";
        PGWriteBehind writer(withApplicationName(connection_, "pgwb_151592"));
        writeReferenceData(writer);
        writer.enqueue(PGWriteRequest{ makeOrder(151592) });
        EXPECT_TRUE(eventually(
            [&writer]
            {
                return 4 <= writer.totalWritten();
            }))
            << "the writes didn't land on a schema made before versioning";
        ASSERT_EQ("t", select("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                              "WHERE application_name = 'pgwb_151592'"));
        ASSERT_TRUE(eventually(
            [&]
            {
                return "0" == select(writers);
            }));
        OrderWrite filled = makeOrder(151592);
        filled.status = "FILLED";
        filled.cumQty = 500;
        filled.leavesQty = 0;
        writer.enqueue(PGWriteRequest{ std::move(filled) });
        writer.shutdown();
        EXPECT_EQ(5u, writer.totalWritten()) << "the write after the reconnect didn't land";
        EXPECT_EQ(0u, writer.totalSchemaRefusals());
        EXPECT_EQ(0u, writer.totalErrors());
    }
    const std::vector<std::string> lines = log.lines();
    EXPECT_EQ(1u, linesWith(lines, "schema has no version"))
        << "the writer didn't warn of an unversioned schema, or warned again as it reconnected";
    EXPECT_EQ("FILLED", select("SELECT status::text FROM orders WHERE order_id = 151592"));
    forget(151592);
}

TEST_F(PGWriteBehindTest, TheFirstMigrationBringsAVolumeMadeBeforeVersioningToVersionOne)
{
    // docker/migrations' first migration (#590), applied in one transaction as scripts/pg-migrate.sh applies it, to the
    // tables of a volume made before #151: quantities INTEGER, ClOrdIDs of 64 characters, prices NUMERIC(18,8), and no
    // stop_triggered or day_start. A search path that finds a schema of the case's own first stands for that volume.
    // After it the columns are init-db's and the schema is at version 1; applied again, it changes nothing.
    const std::string schema = "pgtest_590m_" + std::to_string(::getpid());
    const auto run = [this](const std::string &sql)
    {
        pqxx::connection conn(connection_);
        pqxx::work txn(conn);
        txn.exec(sql);
        txn.commit();
    };
    run("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
    run("CREATE SCHEMA " + schema);
    run("CREATE TABLE " + schema +
        ".orders (order_qty INTEGER, min_qty INTEGER, leaves_qty INTEGER, cum_qty INTEGER, day_order_qty INTEGER, "
        "day_cum_qty INTEGER, cl_order_id VARCHAR(64), orig_cl_order_id VARCHAR(64), source VARCHAR(64), "
        "destination VARCHAR(64), price NUMERIC(18,8), stop_px NUMERIC(18,8), avg_px NUMERIC(18,8), "
        "day_avg_px NUMERIC(18,8))");
    run("CREATE TABLE " + schema + ".executions (last_qty INTEGER, new_qty INTEGER)");
    std::ifstream in(test::sourceRoot() / "docker/migrations/001_version_the_schema.sql");
    ASSERT_TRUE(in) << "can't read docker/migrations/001_version_the_schema.sql";
    const std::string migration((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto migrate = [&]
    {
        pqxx::connection conn(withSearchPath(connection_, schema));
        pqxx::work txn(conn);
        txn.exec(migration);
        txn.commit();
    };
    const auto column = [&](const std::string &table, const std::string &name, const std::string &field)
    {
        return select("SELECT coalesce(" + field +
                      "::text, 'none') FROM information_schema.columns WHERE table_schema = '" + schema +
                      "' AND table_name = '" + table + "' AND column_name = '" + name + "'");
    };

    ASSERT_NO_THROW(migrate());
    for (const char *qty : { "order_qty", "min_qty", "leaves_qty", "cum_qty", "day_order_qty", "day_cum_qty" })
    {
        EXPECT_EQ("bigint", column("orders", qty, "data_type")) << qty;
    }
    for (const char *text : { "cl_order_id", "orig_cl_order_id", "source", "destination" })
    {
        EXPECT_EQ("text", column("orders", text, "data_type")) << text;
    }
    for (const char *price : { "price", "stop_px", "avg_px", "day_avg_px" })
    {
        EXPECT_EQ("none", column("orders", price, "numeric_precision")) << price << " still has a precision";
    }
    EXPECT_EQ("boolean", column("orders", "stop_triggered", "data_type"));
    EXPECT_EQ("timestamp with time zone", column("orders", "day_start", "data_type"));
    EXPECT_EQ("bigint", column("executions", "last_qty", "data_type"));
    EXPECT_EQ("bigint", column("executions", "new_qty", "data_type"));
    EXPECT_EQ("1", select("SELECT max(version)::text FROM " + schema + ".schema_version"));

    ASSERT_NO_THROW(migrate()) << "applied again";
    EXPECT_EQ("1", select("SELECT count(*)::text FROM " + schema + ".schema_version"));

    run("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
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

TEST_F(PGWriteBehindTest, AnOrdersBusinessDayReachesItsRow)
{
    // day_start says which business day the day figures are for (#244): NULL for none, then the day's start
    forget(151244);
    OrderWrite order = makeOrder(151244);
    {
        PGWriteBehind writer(connection_);
        writeReferenceData(writer);
        writer.enqueue(PGWriteRequest{ OrderWrite(order) });
        writer.shutdown();
        ASSERT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("true", select("SELECT (day_start IS NULL)::text FROM orders WHERE order_id = 151244"));

    order.dayStart = 1792098000000ULL; // 2026-10-15 21:00 UTC, 17:00 in New York
    order.dayCumQty = 20;
    {
        PGWriteBehind writer(connection_);
        writer.enqueue(PGWriteRequest{ OrderWrite(order) });
        writer.shutdown();
        ASSERT_EQ(0u, writer.totalErrors());
    }
    EXPECT_EQ("1792098000",
              select("SELECT extract(epoch FROM day_start)::bigint::text FROM orders WHERE order_id = 151244"));
    EXPECT_EQ("20", select("SELECT day_cum_qty::text FROM orders WHERE order_id = 151244"));
    forget(151244);
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
    // As a number: NUMERIC with no precision keeps the scale it is given, where (18,8) padded it to eight places (#538)
    EXPECT_EQ("true", select("SELECT (price = 101.25)::text FROM orders WHERE order_id = 151004"));
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
