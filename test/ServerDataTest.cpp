/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "TestAux.h"

#include "ServerData.h"
#include "ExpiryScheduler.h"
#include "MockQueues.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderCodec.h"

using namespace COP;
using namespace COP::Store;

namespace
{
const size_t MAP_SIZE = 16 << 20;
const u64 ID_BLOCK = 100;

/// The UTC time, in milliseconds since the epoch
DateTimeT utc(int year, unsigned month, unsigned day, int hour)
{
    using namespace std::chrono;
    const sys_days date = year_month_day(std::chrono::year(year), std::chrono::month(month), std::chrono::day(day));
    return static_cast<DateTimeT>(duration_cast<milliseconds>((date + hours(hour)).time_since_epoch()).count());
}

/// A file's bytes
std::string bytesOf(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// An order's record as the dispatcher writes it: its type and format version in the first 4 bytes (#149), then the
/// order as it is
std::string orderRecord(const OrderEntry &val)
{
    const u32 word = (Codec::OrderCodec::FORMAT_VERSION << 16) | StorageRecordDispatcher::ORDER_RECORDTYPE;
    std::string record(reinterpret_cast<const char *>(&word), sizeof(word));
    IdT id;
    u32 version = 0;
    Codec::OrderCodec::encode(val, &record, &id, &version);
    return record;
}

/// The server's load (app/ServerData.cpp), over a directory a test writes first (#165). Each start makes the singletons
/// app/main.cpp makes, so a restart in the middle of a test is a new process over the same directory.
class ServerDataTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        start();
        dir_ = test::uniqueTestPath("server-data");
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override
    {
        stop();
        std::filesystem::remove_all(dir_);
    }
    void start()
    {
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();
    }
    void stop()
    {
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }
    void restart()
    {
        stop();
        start();
    }
    App::ServerData load()
    {
        return App::loadServerData(dir_, MAP_SIZE, ID_BLOCK, "CLRFirm", "ACT");
    }
    App::ServerData load(u32 keepClosedDays, DateTimeT now)
    {
        return App::loadServerData(dir_, MAP_SIZE, ID_BLOCK, "CLRFirm", "ACT", keepClosedDays, now);
    }

    /// An order of the given number, as the transaction that last persisted it wrote it
    std::unique_ptr<OrderEntry> order(const SourceIdT &instrument, u64 number, OrderStatus status, QuantityT leaves)
    {
        std::unique_ptr<OrderEntry> val = test::createCorrectOrder(instrument);
        test::assignClOrderId(val.get());
        val->orderId_ = IdT(number, 20261014);
        val->status_ = status;
        val->orderQty_ = 100;
        val->leavesQty_ = leaves;
        val->cumQty_ = 100 - leaves;
        val->price_ = 10.0;
        // New, or Filled, and no cancel or replace pending: the ids #150 pins
        val->stateMachinePersistance_.orderData_ = val.get();
        val->stateMachinePersistance_.stateZone1Id_ = (FILLED_ORDSTATUS == status) ? 4 : 2;
        val->stateMachinePersistance_.stateZone2Id_ = 8;
        return val;
    }

    std::string dir_;
};
} // namespace

TEST_F(ServerDataTest, AnEmptyDirectoryLoadsWithNothingToContinueFrom)
{
    App::ServerData data = load();
    EXPECT_EQ(0u, data.lastOrderNumber);
    EXPECT_EQ(0u, data.lastIssued);
    EXPECT_FALSE(data.defaultClearingId.isValid()) << "nothing seeded the clearing firm";
    EXPECT_FALSE(data.defaultAccountId.isValid()) << "nothing seeded the account";
    EXPECT_TRUE(data.dispatcher->ordersToMatch().empty());
}

TEST_F(ServerDataTest, ADirectoryWithReferenceDataButNotTheServersDefaultsRefusesTheStart)
{
    // A mistyped --clearing-firm on a seeded directory started a server that refused every order, with a warning the
    // only sign; an identity whose account the directory lacks refused the start (#225). Now a directory that holds
    // reference data must hold the clearing firm, and the default account unless --ws-credentials makes it unused
    // (#640). The fixture's names are the ones test::createCorrectOrder() stores; these are others.
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        test::addInstrument("DEFAULTS");
        test::addAccount("DESK", "DeskFirm", PRINCIPAL_ACCOUNTTYPE);
        test::addClearing("HOUSE");
    }
    auto refusal = [&](const std::string &clearingFirm, const std::string &account, bool accountUsed)
    {
        restart();
        try
        {
            App::loadServerData(dir_, MAP_SIZE, ID_BLOCK, clearingFirm, account, 0, 0, accountUsed);
        }
        catch (const std::runtime_error &e)
        {
            return std::string(e.what());
        }
        return std::string();
    };
    EXPECT_EQ("", refusal("HOUSE", "DESK", true)) << "both there";
    EXPECT_NE(std::string::npos, refusal("HOUSF", "DESK", true).find("--clearing-firm HOUSF is not in"));
    EXPECT_NE(std::string::npos, refusal("HOUSF", "DESK", false).find("--clearing-firm HOUSF is not in"))
        << "every order needs the clearing firm, with identities too";
    EXPECT_NE(std::string::npos, refusal("HOUSE", "DESX", true).find("--default-account DESX is not in"));
    EXPECT_EQ("", refusal("HOUSE", "DESX", false)) << "under --ws-credentials the default account is unused";
}

TEST_F(ServerDataTest, AnEmptyDirectoryStartsWithoutTheServersDefaults)
{
    // As compose's first start does, before seedData seeds the volume (#161): a warning, and no refusal (#640)
    test::CapturedLog log;
    {
        const App::ServerData data = load();
        EXPECT_FALSE(data.defaultClearingId.isValid());
        EXPECT_FALSE(data.defaultAccountId.isValid());
    }
    const std::vector<std::string> lines = log.lines();
    const auto logged = [&](const std::string &text)
    {
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string &line)
                           {
                               return std::string::npos != line.find(text);
                           });
    };
    EXPECT_TRUE(logged("--clearing-firm CLRFirm is not in"));
    EXPECT_TRUE(logged("--default-account ACT is not in"));
}

TEST_F(ServerDataTest, IdsContinuePastTheBlockTheLastStartReserved)
{
    // A start reserves a block of ids, saving a limit past them before issuing any (#81). After a crash, which never
    // saved the exact next id, the next start continues past the whole block: the first start's first id is 1, so its
    // limit is 1 + 100.
    {
        App::ServerData first = load();
        EXPECT_EQ(0u, first.lastIssued);
    }
    restart();
    App::ServerData second = load();
    EXPECT_EQ(0u, second.lastOrderNumber);
    EXPECT_EQ(ID_BLOCK, second.lastIssued);
    EXPECT_GT(IdTGenerator::instance()->getId().id_, ID_BLOCK);
}

TEST_F(ServerDataTest, IdsContinuePastTheLargerOfTheLastOrderAndTheSavedLimit)
{
    // lastIssued = max(the largest order number restored, the saved limit - 1): no new order may take a restored
    // order's number (#58), and no id may repeat one issued below the limit (#81)
    struct Case
    {
        u64 orderNumber_;
        u64 limit_;
        u64 lastIssued_;
    };
    for (const Case &c : { Case{ 500, 101, 500 }, Case{ 500, 1000, 999 } })
    {
        std::filesystem::remove_all(dir_);
        {
            LMDBStorage lmdb(MAP_SIZE);
            StorageRecordDispatcher writer;
            writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
            lmdb.load(dir_, &writer);
            // A record names its instrument, which a load looks up: the directory has it, as a server's always does
            WideDataStorage::instance()->bindStorage(&writer);
            const SourceIdT instrument = test::addInstrument("IDS");
            writer.save(*order(instrument, c.orderNumber_, FILLED_ORDSTATUS, 0));
            writer.saveIdLimit(c.limit_);
        }
        restart();
        {
            App::ServerData data = load();
            EXPECT_EQ(c.orderNumber_, data.lastOrderNumber);
            EXPECT_EQ(c.lastIssued_, data.lastIssued)
                << "an order numbered " << c.orderNumber_ << ", a limit of " << c.limit_;
            EXPECT_GT(IdTGenerator::instance()->getId().id_, c.lastIssued_);
        }
        restart();
    }
}

TEST_F(ServerDataTest, ARestingOrderIsRestoredIntoTheBook)
{
    // The two-pass load: the first restores the instruments, which the book is made from, and the second the orders,
    // into it
    SourceIdT instrument;
    IdT resting;
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        instrument = test::addInstrument("RESTING");
        std::unique_ptr<OrderEntry> val = order(instrument, 7, NEW_ORDSTATUS, 100);
        resting = val->orderId_;
        writer.save(*val);
    }
    restart();
    App::ServerData data = load();
    EXPECT_EQ(resting, data.orderBook->getTop(instrument, BUY_SIDE));
    EXPECT_EQ(7u, data.lastOrderNumber);
}

TEST_F(ServerDataTest, AnOpenOrderOnASideTheBookDoesntHoldDoesntStopTheLoad)
{
    // An order on BUY_MINUS, SELL_PLUS, SELL_SHORT or CROSS that rested was stored NEW until isValid() refused one, and
    // every later start failed: "Can't start: Unable to restore order into book - side is not supported!" (#510). The
    // directory loads now, that order out of the book.
    SourceIdT instrument;
    IdT resting;
    IdT unheld;
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        instrument = test::addInstrument("SIDES");
        std::unique_ptr<OrderEntry> val = order(instrument, 7, NEW_ORDSTATUS, 100);
        val->side_ = SELL_SHORT_SIDE;
        unheld = val->orderId_;
        writer.save(*val);
        val = order(instrument, 8, NEW_ORDSTATUS, 100);
        resting = val->orderId_;
        writer.save(*val);
    }
    restart();
    App::ServerData data = load();
    EXPECT_EQ(resting, data.orderBook->getTop(instrument, BUY_SIDE));
    EXPECT_FALSE(data.orderBook->getTop(instrument, SELL_SIDE).isValid());
    const OrderEntry *restored = OrderStorage::instance()->locateByOrderId(unheld);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(NEW_ORDSTATUS, restored->status_);
    EXPECT_EQ(8u, data.lastOrderNumber);
}

TEST_F(ServerDataTest, RestoredDayAndGtdOrdersThatAreLiveGetTheirTimersBack)
{
    // A restart lost every timer, and nothing set them again (#240). One whose time passed while the server was down
    // fires as soon as the timers start.
    const DateTimeT now = 1792022400000ULL + 9ULL * 60 * 60 * 1000; // 2026-10-15 09:00 UTC, 05:00 in New York
    const DateTimeT hour = 60ULL * 60 * 1000;
    SourceIdT instrument;
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        instrument = test::addInstrument("TIMERS");
        const auto save = [&](u64 number, TimeInForce tif, OrderStatus status, DateTimeT created, DateTimeT expires)
        {
            std::unique_ptr<OrderEntry> val = order(instrument, number, status, (FILLED_ORDSTATUS == status) ? 0 : 100);
            val->tif_ = tif;
            val->creationTime_ = created;
            val->expireTime_ = expires;
            val->dayStart_ = Proc::ExpiryScheduler::dayStartOf(created); // as the processor sets it (#244)
            writer.save(*val);
        };
        save(1, GTD_TIF, NEW_ORDSTATUS, now - hour, now + hour);        // later
        save(2, GTD_TIF, NEW_ORDSTATUS, now - 2 * hour, now - hour);    // passed while down
        save(3, DAY_TIF, NEW_ORDSTATUS, now - 24 * hour, 0);            // yesterday's: ended at 21:00 UTC
        save(4, DAY_TIF, NEW_ORDSTATUS, now - hour, 0);                 // today's: ends at 21:00 UTC
        save(5, GTD_TIF, FILLED_ORDSTATUS, now - 2 * hour, now - hour); // done: no timer
        save(6, GTC_TIF, NEW_ORDSTATUS, now - hour, 0);                 // no end: restated at 21:00 UTC (#244)
    }
    restart();
    App::ServerData data = load();

    ::testing::NiceMock<test::MockInQueues> queues;
    std::vector<IdT> fired;
    ON_CALL(queues, push(::testing::_, ::testing::An<const Queues::TimerEvent &>()))
        .WillByDefault(
            [&fired](const std::string &, const Queues::TimerEvent &evnt)
            {
                fired.push_back(evnt.id_);
            });
    Proc::ExpiryScheduler expiry(&queues,
                                 [now]
                                 {
                                     return now;
                                 });
    EXPECT_EQ(5u, App::scheduleRestoredExpiries(*OrderStorage::instance(), &expiry)) << "4 expiries, 1 restatement";
    EXPECT_EQ(2u, expiry.fireDue());
    std::sort(fired.begin(), fired.end(),
              [](const IdT &a, const IdT &b)
              {
                  return a.id_ < b.id_;
              });
    EXPECT_EQ((std::vector<IdT>{ IdT(2, 20261014), IdT(3, 20261014) }), fired);
    EXPECT_EQ(3u, expiry.pending());
}

TEST_F(ServerDataTest, AnOrderWhoseDayEndedWhileTheServerWasDownIsRestatedAtOnce)
{
    // A restart sets each live order's restatement again (#244): one whose business day ended while the server was down
    // is restated as the timers start, and so is one from a record before format version 4, whose figures are for no
    // day
    const DateTimeT now = 1792022400000ULL + 9ULL * 60 * 60 * 1000; // 2026-10-15 09:00 UTC, 05:00 in New York
    const DateTimeT hour = 60ULL * 60 * 1000;
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        const SourceIdT instrument = test::addInstrument("RESTATED");
        const auto save = [&](u64 number, OrderStatus status, DateTimeT dayStart)
        {
            std::unique_ptr<OrderEntry> val = order(instrument, number, status, (FILLED_ORDSTATUS == status) ? 0 : 100);
            val->tif_ = GTC_TIF;
            val->creationTime_ = (0 == dayStart) ? 1000 : dayStart + hour;
            val->dayStart_ = dayStart;
            writer.save(*val);
        };
        save(1, NEW_ORDSTATUS, now - 36 * hour);    // the day that began 17:00 EDT on the 13th, which ended while down
        save(2, NEW_ORDSTATUS, now - 12 * hour);    // today's, which ends at 21:00 UTC
        save(3, NEW_ORDSTATUS, 0);                  // for no day
        save(4, FILLED_ORDSTATUS, now - 36 * hour); // done: no timer
    }
    restart();
    App::ServerData data = load();

    ::testing::NiceMock<test::MockInQueues> queues;
    std::vector<std::pair<IdT, Queues::TimerEvent::TimerType>> fired;
    ON_CALL(queues, push(::testing::_, ::testing::An<const Queues::TimerEvent &>()))
        .WillByDefault(
            [&fired](const std::string &, const Queues::TimerEvent &evnt)
            {
                fired.emplace_back(evnt.id_, evnt.timerType_);
            });
    Proc::ExpiryScheduler expiry(&queues,
                                 [now]
                                 {
                                     return now;
                                 });
    EXPECT_EQ(3u, App::scheduleRestoredExpiries(*OrderStorage::instance(), &expiry));
    EXPECT_EQ(2u, expiry.fireDue());
    std::sort(fired.begin(), fired.end(),
              [](const auto &a, const auto &b)
              {
                  return a.first.id_ < b.first.id_;
              });
    ASSERT_EQ(2u, fired.size());
    EXPECT_EQ(IdT(1, 20261014), fired[0].first);
    EXPECT_EQ(IdT(3, 20261014), fired[1].first);
    EXPECT_EQ(Queues::TimerEvent::RESTATEMENT, fired[0].second);
    EXPECT_EQ(Queues::TimerEvent::RESTATEMENT, fired[1].second);
    EXPECT_EQ(1u, expiry.pending()) << "today's order isn't restated until its day ends";
}

TEST_F(ServerDataTest, ARestoredOpgOrAtCloseOrderIsNamedInTheLogAndRestsAsBefore)
{
    // An order taken before #601 refused OPG and ATCLOSE keeps its time in force, and trades as a GTC order, as it did.
    // The start names each live one, and sets the timers it set before: a restatement each, as for a GTC order.
    const DateTimeT now = 1792022400000ULL + 9ULL * 60 * 60 * 1000; // 2026-10-15 09:00 UTC, 05:00 in New York
    const DateTimeT hour = 60ULL * 60 * 1000;
    SourceIdT instrument;
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        instrument = test::addInstrument("AUCTIONS");
        const auto save = [&](u64 number, TimeInForce tif, OrderStatus status, Side side, PriceT price)
        {
            std::unique_ptr<OrderEntry> val = order(instrument, number, status, (FILLED_ORDSTATUS == status) ? 0 : 100);
            val->tif_ = tif;
            val->side_ = side;
            val->price_ = price;
            val->creationTime_ = now - hour;
            val->dayStart_ = Proc::ExpiryScheduler::dayStartOf(now - hour); // as the processor sets it (#244)
            writer.save(*val);
        };
        save(1, OPG_TIF, NEW_ORDSTATUS, BUY_SIDE, 10.0);
        save(2, ATCLOSE_TIF, NEW_ORDSTATUS, SELL_SIDE, 11.0);
        save(3, OPG_TIF, FILLED_ORDSTATUS, BUY_SIDE, 10.0); // done: not named
        save(4, GTC_TIF, NEW_ORDSTATUS, BUY_SIDE, 9.0);
    }
    restart();
    test::CapturedLog log;
    {
        App::ServerData data = load();
        EXPECT_EQ(IdT(1, 20261014), data.orderBook->getTop(instrument, BUY_SIDE));
        EXPECT_EQ(IdT(2, 20261014), data.orderBook->getTop(instrument, SELL_SIDE));

        ::testing::NiceMock<test::MockInQueues> queues;
        Proc::ExpiryScheduler expiry(&queues,
                                     [now]
                                     {
                                         return now;
                                     });
        EXPECT_EQ(3u, App::scheduleRestoredExpiries(*OrderStorage::instance(), &expiry)) << "a restatement each";
        EXPECT_EQ(0u, expiry.fireDue());
    }
    const std::vector<std::string> lines = log.lines();
    const auto count = [&](const std::string &text)
    {
        return std::count_if(lines.begin(), lines.end(),
                             [&](const std::string &line)
                             {
                                 return std::string::npos != line.find(text);
                             });
    };
    EXPECT_EQ(1, count("Order 1 is OPG, which the server no longer takes: it trades as a GTC order until it is filled "
                       "or canceled, and a replace must give it another time in force (#601)"));
    EXPECT_EQ(1, count("Order 2 is ATCLOSE, which the server no longer takes"));
    EXPECT_EQ(2, count("(#601)")) << "only the two live orders are named";
}

TEST_F(ServerDataTest, ARecordOfATypeThisBuildDoesNotKnowStopsTheLoad)
{
    // An unknown record type throws out of the load, and app/main.cpp exits with the reason (#203), rather than start
    // without what the record held (#165)
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        const u32 unknownType = 77;
        std::string record(reinterpret_cast<const char *>(&unknownType), sizeof(unknownType));
        record += "anything";
        lmdb.save(IdT(7, 7), record.data(), record.size());
    }
    restart();
    try
    {
        App::ServerData data = load();
        ADD_FAILURE() << "a record of type 77 was loaded";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_NE(std::string::npos, std::string(ex.what()).find("Invalid record type")) << ex.what();
    }
}

TEST_F(ServerDataTest, ACheckRestoresWhatAStartRestoresAndLeavesTheDataFileAsItWas)
{
    // seedData --check restores the directory as a start does, with the environment read-only (#591): it finds what the
    // start finds, and data.mdb's bytes are the same after it. The start, which saves its id limit, changes them.
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        const SourceIdT instrument = test::addInstrument("CHECKED");
        writer.save(*order(instrument, 7, FILLED_ORDSTATUS, 0));
        std::unique_ptr<OrderEntry> sell = order(instrument, 8, NEW_ORDSTATUS, 100);
        sell->side_ = SELL_SIDE;
        writer.save(*sell);
        // A buy at the sell's price, which traded with it: a crash lost the fills, so a start matches it (#207)
        writer.save(*order(instrument, 9, NEW_ORDSTATUS, 100));
    }
    const std::string dataFile = dir_ + "/data.mdb";
    const std::string written = bytesOf(dataFile);
    ASSERT_FALSE(written.empty());

    restart();
    const App::DataCheck found = App::checkServerData(dir_, MAP_SIZE);
    EXPECT_EQ(written, bytesOf(dataFile)) << "the check wrote to the data file";
    EXPECT_EQ(1u, found.instruments);
    EXPECT_EQ(3u, found.orders);
    EXPECT_EQ(1u, found.ordersToMatch) << "the buy whose fills a crash lost";

    restart();
    App::ServerData data = load();
    size_t accounts = 0;
    size_t clearingFirms = 0;
    WideDataStorage::instance()->forEachAccount(
        [&accounts](const auto &, const auto &)
        {
            ++accounts;
        });
    WideDataStorage::instance()->forEachClearing(
        [&clearingFirms](const auto &, const auto &)
        {
            ++clearingFirms;
        });
    EXPECT_LT(0u, accounts);
    EXPECT_EQ(accounts, found.accounts);
    EXPECT_EQ(clearingFirms, found.clearingFirms);
    EXPECT_EQ(OrderStorage::instance()->orderCount(), found.orders);
    EXPECT_EQ(data.dispatcher->ordersToMatch().size(), found.ordersToMatch);
    EXPECT_NE(written, bytesOf(dataFile)) << "a start writes its id limit, so a write would have shown";
}

TEST_F(ServerDataTest, ACheckReadsADataFileItMayNotWrite)
{
    // The check opens the environment read-only (#591), so it reads a data file its user may not write. A start, which
    // opens it for writing, is refused the same file, unless its user is root, whom the file's mode doesn't stop.
    // LMDB's lock file is another matter: every reader opens lock.mdb for writing, and creates it beside a copy that
    // has none, so a copy in a directory its user may not write is refused, naming lock.mdb.
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        test::addInstrument("READONLY");
    }
    namespace fs = std::filesystem;
    const fs::path dataFile = fs::path(dir_) / "data.mdb";
    fs::permissions(dataFile, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    const std::string written = bytesOf(dataFile.string());

    restart();
    EXPECT_EQ(1u, App::checkServerData(dir_, MAP_SIZE).instruments);
    EXPECT_EQ(written, bytesOf(dataFile.string()));
    if (0 != geteuid())
    {
        restart();
        EXPECT_THROW(load(), std::runtime_error) << "a start wrote to a data file its user may not write";

        const fs::path copy = fs::path(dir_ + "-copy");
        fs::remove_all(copy);
        fs::create_directory(copy);
        fs::copy_file(dataFile, copy / "data.mdb"); // as mdb_copy leaves one: data.mdb alone
        fs::permissions(copy, fs::perms::owner_read | fs::perms::owner_exec);
        restart();
        try
        {
            App::checkServerData(copy.string(), MAP_SIZE);
            ADD_FAILURE() << "the check read a copy where LMDB can't make its lock file";
        }
        catch (const std::runtime_error &ex)
        {
            EXPECT_NE(std::string::npos, std::string(ex.what()).find("lock.mdb")) << ex.what();
        }
        fs::permissions(copy, fs::perms::owner_all);
        fs::remove_all(copy);
    }
}

TEST_F(ServerDataTest, ACheckRefusesAnOrderRecordAStartRefusesAndNamesIt)
{
    // A record a start refuses, the check refuses too, and names it by its key and version, so that it can be found
    // (#591): here the newest version of order 7's record, a byte short
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        WideDataStorage::instance()->bindStorage(&writer);
        std::unique_ptr<OrderEntry> val = order(test::addInstrument("CUT"), 7, NEW_ORDSTATUS, 100);
        writer.save(*val);
        std::string record = orderRecord(*val);
        record.pop_back();
        ASSERT_EQ(1u, lmdb.update(val->orderId_, record.data(), record.size())) << "the cut record is the newest";
    }
    const std::string written = bytesOf(dir_ + "/data.mdb");
    std::string refused;
    restart();
    try
    {
        App::checkServerData(dir_, MAP_SIZE);
        ADD_FAILURE() << "the check restored a record cut short";
    }
    catch (const std::runtime_error &ex)
    {
        refused = ex.what();
    }
    EXPECT_EQ(0u, refused.find("Order 20261014.7 version 1: ")) << refused;
    EXPECT_EQ(written, bytesOf(dir_ + "/data.mdb"));

    restart();
    try
    {
        load();
        ADD_FAILURE() << "a start restored a record cut short";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_EQ(refused, ex.what()) << "the check refuses what a start refuses";
    }
}

TEST_F(ServerDataTest, ACheckNamesAReferenceRecordItRefuses)
{
    // A record the load's scan refuses names itself too (#591): an instrument's record of one byte
    {
        LMDBStorage lmdb(MAP_SIZE);
        StorageRecordDispatcher writer;
        writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
        lmdb.load(dir_, &writer);
        const u32 word = StorageRecordDispatcher::INSTRUMENT_RECORDTYPE;
        std::string record(reinterpret_cast<const char *>(&word), sizeof(word));
        record += "X";
        lmdb.save(IdT(5, 20261014), record.data(), record.size());
    }
    restart();
    try
    {
        App::checkServerData(dir_, MAP_SIZE);
        ADD_FAILURE() << "the check restored an instrument's record of one byte";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_EQ(0u, std::string(ex.what()).find("Record 20261014.5 version 0: ")) << ex.what();
    }
}

TEST_F(ServerDataTest, AStartDropsTheClosedOrdersOfBusinessDaysBeforeTheKeptOnes)
{
    // --keep-closed-days N (#597): a closed order whose business day isn't one of the last N, the current one counted,
    // isn't restored, and every version of its record is erased. Every other order is kept, whatever its day. The start
    // is at 22:00 New York on 2026-11-08, in the business day that began there at 17:00.
    const DateTimeT now = utc(2026, 11, 9, 3);
    DateTimeT days[3]; // two business days before the start's, the one before it, and the start's own
    days[2] = Proc::ExpiryScheduler::dayStartOf(now);
    days[1] = Proc::ExpiryScheduler::dayStartOf(days[2] - 1);
    days[0] = Proc::ExpiryScheduler::dayStartOf(days[1] - 1);
    ASSERT_EQ(utc(2026, 11, 8, 22), days[2]);
    ASSERT_EQ(utc(2026, 11, 7, 22), days[1]);
    ASSERT_EQ(utc(2026, 11, 6, 22), days[0]);
    for (int day = 0; day < 3; ++day)
    {
        EXPECT_EQ(days[day], Proc::ExpiryScheduler::keptDaysStart(now, static_cast<u32>(3 - day)));
    }
    EXPECT_EQ(0u, Proc::ExpiryScheduler::keptDaysStart(now, 0)) << "0 days keeps every day";

    struct Written
    {
        IdT id_;
        RawDataEntry clOrderId_;
        bool closed_;
        int day_; ///< in days, the business day it closed in, or was last live in
    };
    const OrderStatus closed[] = { FILLED_ORDSTATUS, CANCELED_ORDSTATUS, REJECTED_ORDSTATUS, EXPIRED_ORDSTATUS,
                                   REPLACED_ORDSTATUS };
    const OrderStatus open[] = { NEW_ORDSTATUS, SUSPENDED_ORDSTATUS, DFD_ORDSTATUS, PENDINGREPLACE_ORDSTATUS };
    const char *const keptFrom[] = { "", "2026-11-08T22:00:00Z", "2026-11-07T22:00:00Z", "2026-11-06T22:00:00Z" };
    for (const u32 keep : { 1u, 2u, 3u, 0u })
    {
        std::filesystem::remove_all(dir_);
        std::vector<Written> written;
        {
            LMDBStorage lmdb(MAP_SIZE);
            StorageRecordDispatcher writer;
            writer.init(WideDataStorage::instance(), nullptr, &lmdb, OrderStorage::instance());
            lmdb.load(dir_, &writer);
            WideDataStorage::instance()->bindStorage(&writer);
            const SourceIdT instrument = test::addInstrument("KEEP");
            u64 number = 0;
            const auto save = [&](OrderStatus status, DateTimeT dayStart, DateTimeT created, int day)
            {
                const bool isClosed = std::find(std::begin(closed), std::end(closed), status) != std::end(closed);
                const QuantityT leaves = (isClosed || (DFD_ORDSTATUS == status)) ? 0 : 100;
                std::unique_ptr<OrderEntry> val = order(instrument, ++number, status, leaves);
                val->dayStart_ = dayStart;
                val->creationTime_ = created;
                // Two versions, as a server leaves them (#148): a drop erases both
                writer.save(*val);
                writer.save(*val);
                written.push_back(Written{ val->orderId_, val->clOrderId_.get(), isClosed, day });
            };
            for (int day = 0; day < 3; ++day)
            {
                for (const OrderStatus status : closed)
                {
                    save(status, days[day], days[day] + 1000, day);
                }
                for (const OrderStatus status : open)
                {
                    save(status, days[day], days[day] + 1000, day);
                }
            }
            // Written before format 4 (#244), with no day: its creation's time stands for it
            save(FILLED_ORDSTATUS, 0, days[1] + 3600000, 1);
            save(CANCELED_ORDSTATUS, 0, days[2] + 3600000, 2);
            // The largest number, on an order a start that keeps one or two days drops: no new order may take it
            number = 999;
            save(FILLED_ORDSTATUS, days[0], days[0] + 1000, 0);
        }
        const auto dropped = [&](const Written &w, u32 kept)
        {
            return (0 < kept) && w.closed_ && (w.day_ < 3 - static_cast<int>(kept));
        };
        restart();
        {
            test::CapturedLog log;
            size_t expected = 0;
            {
                App::ServerData data = load(keep, now);
                for (const Written &w : written)
                {
                    const bool drop = dropped(w, keep);
                    expected += drop ? 1 : 0;
                    EXPECT_EQ(drop, nullptr == OrderStorage::instance()->locateByOrderId(w.id_))
                        << "keeping " << keep << " days, order " << w.id_.id_;
                    EXPECT_EQ(drop, nullptr == OrderStorage::instance()->locateByClOrderId(w.clOrderId_))
                        << "keeping " << keep << " days, order " << w.id_.id_ << "'s ClOrdID";
                }
                EXPECT_EQ(expected, data.droppedOrders) << "keeping " << keep << " days";
                EXPECT_EQ(1000u, data.lastOrderNumber) << "keeping " << keep << " days";
                EXPECT_GT(IdTGenerator::instance()->getId().id_, 1000u) << "keeping " << keep << " days";
            }
            const std::vector<std::string> lines = log.lines();
            const std::string note = (0 < keep) ? "Dropped " + std::to_string(expected) +
                                                      " closed orders of business days before " + keptFrom[keep] +
                                                      " (--keep-closed-days " + std::to_string(keep) + ")"
                                                : std::string("Dropped ");
            EXPECT_EQ((0 < keep) ? 1 : 0, std::count_if(lines.begin(), lines.end(),
                                                        [&](const std::string &line)
                                                        {
                                                            return std::string::npos != line.find(note);
                                                        }))
                << "keeping " << keep << " days: " << note;
        }
        // Erased: a start that keeps every order finds none of them
        restart();
        {
            App::ServerData data = load(0, now);
            EXPECT_EQ(0u, data.droppedOrders);
            for (const Written &w : written)
            {
                EXPECT_EQ(dropped(w, keep), nullptr == OrderStorage::instance()->locateByOrderId(w.id_))
                    << "after keeping " << keep << " days, order " << w.id_.id_;
            }
        }
        restart();
    }
}

TEST_F(ServerDataTest, ARunningServerDropsClosedOrdersAtEachDaysStartOnlyWithDaysToKeep)
{
    // --keep-closed-days 0 keeps every order (#597), and a running server sets no drop (#722). With days to keep, the
    // expiry scheduler fires the drop at each business day's start, the first after the server's start: here
    // 2026-11-09 17:00 New York, 22:00 UTC, in standard time.
    DateTimeT now = utc(2026, 11, 9, 15);
    ::testing::NiceMock<test::MockInQueues> queues;
    std::vector<std::pair<IdT, Queues::TimerEvent::TimerType>> fired;
    ON_CALL(queues, push(::testing::_, ::testing::An<const Queues::TimerEvent &>()))
        .WillByDefault(
            [&fired](const std::string &, const Queues::TimerEvent &evnt)
            {
                fired.emplace_back(evnt.id_, evnt.timerType_);
            });
    Proc::ExpiryScheduler expiry(&queues,
                                 [&now]
                                 {
                                     return now;
                                 });
    EXPECT_FALSE(App::scheduleClosedOrderDrops(expiry, 0, now));
    EXPECT_EQ(0u, expiry.pending()) << "keeping every day, a drop was set";

    EXPECT_TRUE(App::scheduleClosedOrderDrops(expiry, 2, now));
    ASSERT_EQ(1u, expiry.pending());
    now = utc(2026, 11, 9, 22) - 1;
    EXPECT_EQ(0u, expiry.fireDue()) << "the drop fired before the day's start";
    now += 1;
    EXPECT_EQ(1u, expiry.fireDue());
    ASSERT_EQ(1u, fired.size());
    EXPECT_FALSE(fired[0].first.isValid()) << "the drop names an order";
    EXPECT_EQ(Queues::TimerEvent::DROP_CLOSED_ORDERS, fired[0].second);
    EXPECT_EQ(1u, expiry.pending()) << "the next day's drop isn't set";
}
