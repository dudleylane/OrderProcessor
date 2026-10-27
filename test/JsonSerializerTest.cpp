/**
 Concurrent Order Processor library - WebSocket JSON serializer Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "TestFixtures.h"
#include "TestAux.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "JsonSerializer.h"
#include "OrderStorage.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::Store;

namespace
{

class JsonSerializerTest : public test::OrderStorageFixture
{
protected:
    void SetUp() override
    {
        test::OrderStorageFixture::SetUp();
        instrument_ = test::addInstrument("AAPL");
        source_ = WideDataStorage::instance()->add(new StringT("WebSocket"));
        destination_ = WideDataStorage::instance()->add(new StringT("Internal"));
    }

    static SourceIdT addRawData(const char *text)
    {
        return WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, text, static_cast<u32>(std::strlen(text))));
    }

    /// An order as WsSession builds one when the client gives no account: clOrderId, the instrument, the source
    /// and the destination are set, and origClOrderId, the account, the clearing and the executions are not.
    std::unique_ptr<OrderEntry> newOrder(const char *clOrderId, const IdT &orderId)
    {
        std::unique_ptr<OrderEntry> order(new OrderEntry(source_, destination_, addRawData(clOrderId), SourceIdT(),
                                                         instrument_, SourceIdT(), SourceIdT(), SourceIdT()));
        order->orderId_ = orderId;
        order->side_ = BUY_SIDE;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        return order;
    }

    SourceIdT instrument_;
    SourceIdT source_;
    SourceIdT destination_;
};

TEST_F(JsonSerializerTest, CancelReportCarriesNoTradeCancelFields)
{
    // The engine reports an order cancel as a plain ExecutionEntry. The serializer used to pick the class from type_
    // and read a CANCEL report as a TradeCancelExecEntry, past the end of the object; the blank copies OrderStorage
    // kept until #56 hid that.
    std::unique_ptr<ExecutionEntry> report(new ExecutionEntry());
    report->type_ = CANCEL_EXECTYPE;
    report->orderStatus_ = CANCELED_ORDSTATUS;
    report->orderId_ = IdT(7, 1);
    report->execId_ = IdT(8, 1);
    report->market_ = INTERNAL_EXECUTION;

    nlohmann::json msg = nlohmann::json::parse(App::serializeExecReport(report.get()));
    EXPECT_EQ("CANCEL", msg["data"].value("type", ""));
    EXPECT_EQ(7u, msg["data"].value("orderId", 0u));
    EXPECT_EQ(INTERNAL_EXECUTION, msg["data"].value("market", ""));
    EXPECT_FALSE(msg["data"].contains("execRefId"));
}

TEST_F(JsonSerializerTest, TriggeredReportIsAPlainReport)
{
    // A stop's release (#236): a plain ExecutionEntry, the order NEW, and no trade fields
    std::unique_ptr<ExecutionEntry> report(new ExecutionEntry());
    report->type_ = TRIGGERED_EXECTYPE;
    report->orderStatus_ = NEW_ORDSTATUS;
    report->orderId_ = IdT(7, 1);
    report->execId_ = IdT(8, 1);
    report->market_ = INTERNAL_EXECUTION;

    nlohmann::json msg = nlohmann::json::parse(App::serializeExecReport(report.get()));
    EXPECT_EQ("TRIGGERED", msg["data"].value("type", ""));
    EXPECT_EQ("NEW", msg["data"].value("orderStatus", ""));
    EXPECT_EQ(7u, msg["data"].value("orderId", 0u));
    EXPECT_FALSE(msg["data"].contains("lastQty"));
}

TEST_F(JsonSerializerTest, RejectedReplaceReportCarriesItsReason)
{
    // A rejected replace is a RejectExecEntry with type REPLACE. Read as a ReplaceExecEntry, its reason's bytes became
    // an origOrderId.
    std::unique_ptr<RejectExecEntry> report(new RejectExecEntry());
    report->type_ = REPLACE_EXECTYPE;
    report->orderStatus_ = REJECTED_ORDSTATUS;
    report->orderId_ = IdT(7, 1);
    report->rejectReason_ = "too late to replace";

    nlohmann::json msg = nlohmann::json::parse(App::serializeExecReport(report.get()));
    EXPECT_EQ("REPLACE", msg["data"].value("type", ""));
    EXPECT_EQ("too late to replace", msg["data"].value("rejectReason", ""));
    EXPECT_FALSE(msg["data"].contains("origOrderId"));
}

TEST_F(JsonSerializerTest, CancelRejectSaysWhy)
{
    // Each reason the engine gives for refusing a cancel reads differently in cancel_reject (#73)
    Queues::CancelRejectEvent reject;
    reject.reason_ = Queues::CancelRejectEvent::UNKNOWN_ORDER;
    EXPECT_EQ("Cancel rejected: unknown order", App::cancelRejectReason(reject));
    reject.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    reject.ordStatus_ = FILLED_ORDSTATUS;
    EXPECT_EQ("Cancel rejected: too late, the order is FILLED", App::cancelRejectReason(reject));
    reject.reason_ = Queues::CancelRejectEvent::PENDING;
    EXPECT_EQ("Cancel rejected: a replace of the order is pending", App::cancelRejectReason(reject));
    reject.reason_ = Queues::CancelRejectEvent::OTHER;
    EXPECT_EQ("Cancel rejected", App::cancelRejectReason(reject));
}

TEST_F(JsonSerializerTest, OrderWithUnsetReferencesSerializes)
{
    // The bug this covers: orderToJson resolved every reference, and a new order's origClOrderId is unset, so
    // serializing any new order threw "rawData not found" (#35). An account the client left out is unset too.
    std::unique_ptr<OrderEntry> order = newOrder("CL-1", IdT(1, 1));

    std::string text;
    ASSERT_NO_THROW(text = App::serializeOrderUpdate(*order));
    nlohmann::json data = nlohmann::json::parse(text)["data"];
    EXPECT_EQ("CL-1", data.value("clOrderId", "?"));
    EXPECT_EQ("", data.value("origClOrderId", "?"));
    EXPECT_EQ("", data.value("account", "?"));
    EXPECT_EQ("AAPL", data.value("symbol", "?"));
    EXPECT_EQ("WebSocket", data.value("source", "?"));
    EXPECT_EQ("Internal", data.value("destination", "?"));
}

TEST_F(JsonSerializerTest, AnOrderCarriesItsDayFigures)
{
    // FIX's DayOrderQty, DayCumQty and DayAvgPx, and the business day they are for (#244): a RESTATED report's order
    // update is how a client hears its figures began again
    std::unique_ptr<OrderEntry> order = newOrder("CL-1", IdT(1, 1));
    order->dayOrderQty_ = 70;
    order->dayCumQty_ = 20;
    order->dayAvgPx_ = 10.25;
    order->dayStart_ = 1792098000000ULL;

    nlohmann::json data = nlohmann::json::parse(App::serializeOrderUpdate(*order))["data"];
    EXPECT_EQ(70u, data.value("dayOrderQty", 0u));
    EXPECT_EQ(20u, data.value("dayCumQty", 0u));
    EXPECT_DOUBLE_EQ(10.25, data.value("dayAvgPx", 0.0));
    EXPECT_EQ(1792098000000ULL, data.value("dayStart", 0ULL));
}

TEST_F(JsonSerializerTest, AnOrderUpdateGivesTheFiguresItsReportCarries)
{
    // A fill's report can go out after a later fill of the order was applied (#411). The order update sent with it
    // gives what the fill left the order with, which the report carries, and not what the order has by then.
    std::unique_ptr<OrderEntry> order = newOrder("CL-1", IdT(1, 1));
    order->orderQty_ = 100;
    order->status_ = PARTFILL_ORDSTATUS;
    order->cumQty_ = 30;
    order->leavesQty_ = 70;
    order->avgPx_ = 10.0;
    order->dayOrderQty_ = 100;
    order->dayCumQty_ = 30;
    order->dayAvgPx_ = 10.0;
    order->dayStart_ = 1792098000000ULL;
    const OrderFigures firstFill(*order);
    // The second fill, which takes the rest, applied before the first fill's report goes out
    order->status_ = FILLED_ORDSTATUS;
    order->cumQty_ = 100;
    order->leavesQty_ = 0;
    order->avgPx_ = 10.35;
    order->dayCumQty_ = 100;
    order->dayAvgPx_ = 10.35;

    nlohmann::json data = nlohmann::json::parse(App::serializeOrderUpdate(*order, firstFill))["data"];
    EXPECT_EQ("PARTIAL_FILL", data.value("status", "?"));
    EXPECT_EQ(30u, data.value("cumQty", 0u));
    EXPECT_EQ(70u, data.value("leavesQty", 0u));
    EXPECT_DOUBLE_EQ(10.0, data.value("avgPx", 0.0));
    EXPECT_EQ(100u, data.value("dayOrderQty", 0u));
    EXPECT_EQ(30u, data.value("dayCumQty", 0u));
    EXPECT_DOUBLE_EQ(10.0, data.value("dayAvgPx", 0.0));
    EXPECT_EQ(1792098000000ULL, data.value("dayStart", 0ULL));
    EXPECT_EQ(100u, data.value("orderQty", 0u)) << "what no decision changes comes from the order";
    EXPECT_EQ("CL-1", data.value("clOrderId", "?"));

    // Without a report's figures, the order as it is
    data = nlohmann::json::parse(App::serializeOrderUpdate(*order))["data"];
    EXPECT_EQ("FILLED", data.value("status", "?"));
    EXPECT_EQ(100u, data.value("cumQty", 0u));
    EXPECT_EQ(0u, data.value("leavesQty", 1u));
}

TEST_F(JsonSerializerTest, SetReferencesAreSerialized)
{
    // A replacement has an origClOrderId, and an order may have an account: both still come through.
    auto *acct = new AccountEntry();
    acct->account_ = "TRADING-1";
    acct->firm_ = "Apex Capital";
    acct->type_ = PRINCIPAL_ACCOUNTTYPE;
    SourceIdT account = WideDataStorage::instance()->add(acct);
    OrderEntry replacement(source_, destination_, addRawData("CL-2"), addRawData("CL-1"), instrument_, account,
                           SourceIdT(), SourceIdT());

    nlohmann::json data = nlohmann::json::parse(App::serializeOrderUpdate(replacement))["data"];
    EXPECT_EQ("CL-2", data.value("clOrderId", "?"));
    EXPECT_EQ("CL-1", data.value("origClOrderId", "?"));
    EXPECT_EQ("TRADING-1", data.value("account", "?"));
}

TEST_F(JsonSerializerTest, ASnapshotGivesEachOrderAsItsLastReportLeftIt)
{
    // A fill is applied to its orders when it is decided, and persisted and reported after (#404). The snapshot gives
    // each order as its last report left it: a restored order as it was persisted, whatever has been decided about it
    // since. An order whose first report isn't out is left out, since its report's order update reaches the session
    // after the snapshot.
    std::unique_ptr<OrderEntry> restoredOrder = newOrder("CL-1", IdT(1, 1));
    restoredOrder->status_ = NEW_ORDSTATUS;
    restoredOrder->orderQty_ = 100;
    restoredOrder->leavesQty_ = 100;
    OrderEntry *restored = restoredOrder.get();
    OrderStorage::instance()->restore(restoredOrder.release());
    {
        // A fill of 30 decided, not yet reported
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(restored->entryMutex_, true);
        restored->status_ = PARTFILL_ORDSTATUS;
        restored->cumQty_ = 30;
        restored->leavesQty_ = 70;
        restored->avgPx_ = 10.0;
    }
    std::unique_ptr<OrderEntry> received = newOrder("CL-2", IdT());
    received->orderQty_ = 50;
    received->leavesQty_ = 50;
    OrderEntry *stored = OrderStorage::instance()->save(*received, IdTGenerator::instance());
    ASSERT_NE(nullptr, stored);

    nlohmann::json msg = nlohmann::json::parse(App::serializeOrderSnapshot(OrderStorage::instance()));
    ASSERT_EQ(1u, msg["data"].size()) << "an order whose first report isn't out is in the snapshot";
    EXPECT_EQ("CL-1", msg["data"][0].value("clOrderId", "?"));
    EXPECT_EQ("NEW", msg["data"][0].value("status", "?")) << "the snapshot gives a fill that isn't reported";
    EXPECT_EQ(0u, msg["data"][0].value("cumQty", 1u));
    EXPECT_EQ(100u, msg["data"][0].value("leavesQty", 0u));
    EXPECT_DOUBLE_EQ(0.0, msg["data"][0].value("avgPx", 1.0));

    {
        // Each order's next report goes out, as processEvent_GenerateExecution publishes one: the fill's, and the new
        // order's acknowledgement
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(restored->entryMutex_, true);
        restored->published_ = OrderFigures(*restored);
    }
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(stored->entryMutex_, true);
        stored->status_ = NEW_ORDSTATUS;
        stored->published_ = OrderFigures(*stored);
    }
    msg = nlohmann::json::parse(App::serializeOrderSnapshot(OrderStorage::instance()));
    ASSERT_EQ(2u, msg["data"].size());
    for (const nlohmann::json &order : msg["data"])
    {
        if ("CL-1" == order.value("clOrderId", "?"))
        {
            EXPECT_EQ("PARTIAL_FILL", order.value("status", "?"));
            EXPECT_EQ(30u, order.value("cumQty", 0u));
            EXPECT_EQ(70u, order.value("leavesQty", 0u));
            EXPECT_DOUBLE_EQ(10.0, order.value("avgPx", 0.0));
        }
        else
        {
            EXPECT_EQ("CL-2", order.value("clOrderId", "?"));
            EXPECT_EQ("NEW", order.value("status", "?"));
            EXPECT_EQ(0u, order.value("cumQty", 1u));
            EXPECT_EQ(50u, order.value("leavesQty", 0u));
        }
    }
}

TEST_F(JsonSerializerTest, ASnapshotWaitingForAnOrdersLockLeavesTheStoreFree)
{
    // A replace holds its original's write lock while it stores the replacement, and the store takes its own write
    // lock to insert it (Processor::process(onReplace)). A snapshot that took an order's lock while it held the store's
    // would wait there for the original, and the replacement's save for the store: a deadlock (#404). The snapshot
    // reads each order under its lock once it has released the store's, so the save goes ahead while it waits.
    std::unique_ptr<OrderEntry> originalOrder = newOrder("CL-1", IdT(1, 1));
    originalOrder->status_ = NEW_ORDSTATUS;
    originalOrder->orderQty_ = 100;
    originalOrder->leavesQty_ = 100;
    OrderEntry *original = originalOrder.get();
    OrderStorage::instance()->restore(originalOrder.release());
    const std::unique_ptr<OrderEntry> replacement = newOrder("CL-2", IdT());

    std::string snapshot;
    std::atomic<bool> saved{ false };
    bool savedWhileHeld = false;
    {
        // The replace's hold on its original
        oneapi::tbb::spin_rw_mutex::scoped_lock hold(original->entryMutex_, true);
        std::thread reader(
            [&snapshot]
            {
                snapshot = App::serializeOrderSnapshot(OrderStorage::instance());
            });
        // Time for the snapshot to reach the original's lock, which it waits for until the hold ends
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::thread replacer(
            [&saved, &replacement]
            {
                OrderStorage::instance()->save(*replacement, IdTGenerator::instance());
                saved = true;
            });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!saved && (std::chrono::steady_clock::now() < deadline))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        savedWhileHeld = saved;
        // Released before the threads are joined, so that a snapshot holding the store's lock finishes, and the save
        // after it
        hold.release();
        reader.join();
        replacer.join();
    }
    EXPECT_TRUE(savedWhileHeld) << "the snapshot held the store's lock while it waited for an order's";
    nlohmann::json msg = nlohmann::json::parse(snapshot);
    ASSERT_EQ(1u, msg["data"].size()) << "the original, published when restored; the replacement has no report yet";
    EXPECT_EQ("CL-1", msg["data"][0].value("clOrderId", "?"));
}

TEST_F(JsonSerializerTest, SnapshotIncludesAnOrderWithUnsetReferences)
{
    OrderStorage::instance()->restore(newOrder("CL-3", IdT(3, 1)).release());

    std::string text;
    ASSERT_NO_THROW(text = App::serializeOrderSnapshot(OrderStorage::instance()));
    nlohmann::json msg = nlohmann::json::parse(text);
    ASSERT_EQ(1u, msg["data"].size());
    EXPECT_EQ("CL-3", msg["data"][0].value("clOrderId", "?"));
}

// The metrics carry LMDB's map, which used to fill unseen and then fail every write (#148)
TEST(JsonSerializerMetricsTest, MetricsUpdateCarriesLmdbsMap)
{
    App::SystemMetrics metrics{};
    metrics.storageMapSize = 512UL * 1024 * 1024;
    metrics.storageMapUsed = 300UL * 1024 * 1024;

    nlohmann::json msg = nlohmann::json::parse(App::serializeMetricsUpdate(metrics));
    EXPECT_EQ("metrics_update", msg.value("type", "?"));
    EXPECT_EQ(512UL * 1024 * 1024, msg["data"].value("storageMapSize", size_t(0)));
    EXPECT_EQ(300UL * 1024 * 1024, msg["data"].value("storageMapUsed", size_t(0)));
}

// A swap's report names its leg, and a plain fill's names none (#153)
TEST(JsonSerializerLegTest, ASwapLegIsNamedOnTheWire)
{
    TradeExecEntry trade;
    trade.type_ = TRADE_EXECTYPE;
    trade.orderStatus_ = PARTFILL_ORDSTATUS;
    trade.orderId_ = IdT(7, 1);
    trade.execId_ = IdT(8, 1);
    trade.lastQty_ = 100;
    trade.lastPx_ = 1.0850;
    trade.currency_ = EUR_CURRENCY;

    trade.execLegType_ = NEAR_LEG;
    EXPECT_EQ("NEAR", nlohmann::json::parse(App::serializeExecReport(&trade))["data"].value("execLegType", "?"));
    trade.execLegType_ = FAR_LEG;
    EXPECT_EQ("FAR", nlohmann::json::parse(App::serializeExecReport(&trade))["data"].value("execLegType", "?"));
    trade.execLegType_ = SINGLE_LEG;
    EXPECT_FALSE(nlohmann::json::parse(App::serializeExecReport(&trade))["data"].contains("execLegType"));
}

} // namespace
