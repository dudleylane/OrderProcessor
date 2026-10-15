/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "TestAux.h"

#include "ServerData.h"
#include "ExpiryScheduler.h"
#include "MockQueues.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"

using namespace COP;
using namespace COP::Store;

namespace
{
const size_t MAP_SIZE = 16 << 20;
const u64 ID_BLOCK = 100;

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
    // lastIssued = max(the largest order number restored, the saved limit - 1): no new order may take a restored order's
    // number (#58), and no id may repeat one issued below the limit (#81)
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
            writer.save(*val);
        };
        save(1, GTD_TIF, NEW_ORDSTATUS, now - hour, now + hour);        // later
        save(2, GTD_TIF, NEW_ORDSTATUS, now - 2 * hour, now - hour);    // passed while down
        save(3, DAY_TIF, NEW_ORDSTATUS, now - 24 * hour, 0);            // yesterday's: ended at 21:00 UTC
        save(4, DAY_TIF, NEW_ORDSTATUS, now - hour, 0);                 // today's: ends at 21:00 UTC
        save(5, GTD_TIF, FILLED_ORDSTATUS, now - 2 * hour, now - hour); // done: no timer
        save(6, GTC_TIF, NEW_ORDSTATUS, now - hour, 0);                 // no end
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
    EXPECT_EQ(4u, App::scheduleRestoredExpiries(*OrderStorage::instance(), &expiry));
    EXPECT_EQ(2u, expiry.fireDue());
    std::sort(fired.begin(), fired.end(),
              [](const IdT &a, const IdT &b)
              {
                  return a.id_ < b.id_;
              });
    EXPECT_EQ((std::vector<IdT>{ IdT(2, 20261014), IdT(3, 20261014) }), fired);
    EXPECT_EQ(2u, expiry.pending());
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
