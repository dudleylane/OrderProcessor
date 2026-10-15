/**
 Concurrent Order Processor library - WebSocket authorization Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "IncomingQueues.h"
#include "MockQueues.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "SessionManager.h"
#include "TestAux.h"
#include "WideDataStorage.h"
#include "WsAuth.h"
#include "WsCredentials.h"
#include "WsOutQueues.h"
#include "WsServer.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace COP;
using namespace COP::App;
using namespace COP::Store;

namespace
{

const std::string TOKEN_A = "aaaaaaaaaaaaaaaa0123456789abcdef";
const std::string TOKEN_B = "bbbbbbbbbbbbbbbb0123456789abcdef";
const std::chrono::milliseconds REPLY_TIMEOUT(5000);

/// A real WsServer on loopback with two identities (#225): desk-a may use TRADING-1 and SHARED, and desk-b CLIENT-A and
/// SHARED. Engine output reaches the sessions through a WsOutQueues, as the transaction workers' does.
class WsAuthorizationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        WideDataStorage::create();
        IdTGenerator::create();
        OrderStorage::create();
        auto *instr = new InstrumentEntry();
        instr->symbol_ = "AAPL";
        instr->securityId_ = "US0378331005";
        instr->securityIdSource_ = "ISIN";
        instrumentId_ = WideDataStorage::instance()->add(instr);
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(instrumentId_);
        orderBook_.init(instruments);
        tradingId_ = test::addAccount("TRADING-1");
        clientId_ = test::addAccount("CLIENT-A");
        sharedId_ = test::addAccount("SHARED");
        serverDefaultId_ = test::addAccount("HOUSE-DEFAULT"); // the server's --default-account, which no identity has
        clearingId_ = test::addClearing("HOUSE-CLEARING");

        credentials_ = std::make_shared<WsCredentials>(
            WsCredentials::parse("desk-a " + WsCredentials::hashToken(TOKEN_A) + " TRADING-1,SHARED\n" + "desk-b " +
                                     WsCredentials::hashToken(TOKEN_B) + " CLIENT-A,SHARED\n",
                                 "creds"));
        credentials_->resolveAccounts(
            [](const std::string &name)
            {
                return WideDataStorage::instance()->findAccountByName(name);
            });
        out_ = std::make_unique<WsOutQueues>(&sessionMgr_, WideDataStorage::instance(), OrderStorage::instance(),
                                             &orderBook_);
    }

    void TearDown() override
    {
        ioc_.stop();
        if (serverThread_.joinable())
        {
            serverThread_.join();
        }
        server_.reset();
        out_.reset();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    /// Starts the server, with the identities or, for the regressions, without any
    void start(bool withIdentities = true)
    {
        WsAuth auth;
        if (withIdentities)
        {
            auth.credentials = credentials_;
        }
        server_ = std::make_shared<WsServer>(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), &sessionMgr_,
                                             WideDataStorage::instance(), OrderStorage::instance(), &inQueues_,
                                             IdTGenerator::instance(), &orderBook_, clearingId_, serverDefaultId_,
                                             WsLimits(), auth);
        endpoint_ = server_->localEndpoint();
        server_->run();
        serverThread_ = std::thread(
            [this]
            {
                ioc_.run();
            });
    }

    struct Client
    {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ ioc };
        beast::flat_buffer buffer;
        nlohmann::json accounts; // the account_list it got on connecting
        nlohmann::json snapshot; // and the order_snapshot
    };

    /// Reads the client's next message, within the timeout; nullopt when none comes
    static std::optional<nlohmann::json> next(Client &client)
    {
        std::optional<std::string> text;
        client.buffer.clear();
        client.ws.async_read(client.buffer,
                             [&](beast::error_code ec, std::size_t)
                             {
                                 if (!ec)
                                 {
                                     text = beast::buffers_to_string(client.buffer.data());
                                 }
                             });
        client.ioc.restart();
        client.ioc.run_for(REPLY_TIMEOUT);
        if (!text)
        {
            return std::nullopt;
        }
        return nlohmann::json::parse(*text);
    }

    /// Reads until a message of the type, which it returns; nullopt when none comes
    static std::optional<nlohmann::json> readUntil(Client &client, const std::string &type)
    {
        for (int i = 0; i < 1000; ++i)
        {
            std::optional<nlohmann::json> msg = next(client);
            if (!msg || (msg->value("type", "") == type))
            {
                return msg;
            }
        }
        return std::nullopt;
    }

    /// Connects offering the token, or none when it is empty, and takes the state the server sends first
    std::unique_ptr<Client> connect(const std::string &token)
    {
        auto client = std::make_unique<Client>();
        client->ws.next_layer().connect(endpoint_);
        const std::string subprotocols = token.empty() ? "oms-v1" : "oms-v1, oms-token." + token;
        client->ws.set_option(websocket::stream_base::decorator(
            [subprotocols](websocket::request_type &req)
            {
                req.set(http::field::sec_websocket_protocol, subprotocols);
            }));
        client->ws.handshake("127.0.0.1", "/ws");
        std::optional<nlohmann::json> accounts = readUntil(*client, "account_list");
        std::optional<nlohmann::json> snapshot = readUntil(*client, "order_snapshot");
        EXPECT_TRUE(accounts && snapshot) << "no initial state";
        client->accounts = accounts.value_or(nlohmann::json());
        client->snapshot = snapshot.value_or(nlohmann::json());
        return client;
    }

    /// The types of the messages the client gets before a marker broadcast now: a session's messages arrive in the
    /// order they were sent, so anything sent to it earlier comes first. Every session gets each marker, so each is
    /// numbered, and one an earlier call left in this client's queue is skipped.
    std::vector<std::string> typesBeforeMarker(Client &client)
    {
        const int marker = ++markers_;
        sessionMgr_.broadcast(R"({"type":"marker","n":)" + std::to_string(marker) + "}");
        std::vector<std::string> types;
        for (int i = 0; i < 1000; ++i)
        {
            std::optional<nlohmann::json> msg = next(client);
            if (!msg)
            {
                ADD_FAILURE() << "the marker never came";
                return types;
            }
            const std::string type = msg->value("type", "");
            if ("marker" == type)
            {
                if (marker == msg->value("n", 0))
                {
                    return types;
                }
                continue;
            }
            types.push_back(type);
        }
        return types;
    }

    /// An order of the account in storage, as one restored from the data directory
    OrderEntry *storeOrder(u64 number, const SourceIdT &account, const std::string &clOrdId)
    {
        const SourceIdT clOrderId = WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size())));
        auto *order = new OrderEntry(SourceIdT(), SourceIdT(), clOrderId, SourceIdT(), instrumentId_, account,
                                     clearingId_, SourceIdT());
        order->orderId_ = IdT(number, 20261015);
        order->side_ = BUY_SIDE;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->price_ = 150.0;
        order->orderQty_ = 100;
        order->leavesQty_ = 100;
        order->status_ = NEW_ORDSTATUS;
        OrderStorage::instance()->restore(order);
        return order;
    }

    /// An execution report about the order, pushed as a transaction worker pushes one, to the order's source
    void report(const OrderEntry &order, const std::string &target)
    {
        ExecutionEntry exec;
        exec.type_ = NEW_EXECTYPE;
        exec.orderId_ = order.orderId_;
        exec.orderStatus_ = NEW_ORDSTATUS;
        exec.transactTime_ = 1;
        exec.market_ = INTERNAL_EXECUTION;
        out_->push(Queues::ExecReportEvent(&exec), target);
    }

    /// The order ids an order_snapshot holds
    static std::set<u64> idsIn(const nlohmann::json &snapshot)
    {
        std::set<u64> ids;
        for (const nlohmann::json &order : snapshot["data"])
        {
            ids.insert(order["orderId"].get<u64>());
        }
        return ids;
    }

    /// The account names an account_list holds
    static std::set<std::string> namesIn(const nlohmann::json &accounts)
    {
        std::set<std::string> names;
        for (const nlohmann::json &account : accounts["data"])
        {
            names.insert(account["account"].get<std::string>());
        }
        return names;
    }

    /// Takes what the session queued for the engine, with the source it went under, polling as WsSessionTest does. A
    /// new order is a copy: the queue deletes the event's order once it has dispatched it. timeout 0 polls once.
    struct Queued
    {
        std::string source;
        std::unique_ptr<OrderEntry> order;
        std::optional<Queues::OrderCancelEvent> cancel;
        bool replace = false;
    };
    std::optional<Queued> takeQueued(std::chrono::milliseconds timeout = REPLY_TIMEOUT)
    {
        std::optional<Queued> taken;
        testing::NiceMock<test::MockInQueueProcessor> processor;
        ON_CALL(processor, onEvent(testing::_, testing::An<const Queues::OrderEvent &>()))
            .WillByDefault(
                [&taken](const std::string &source, const Queues::OrderEvent &evnt)
                {
                    taken.emplace();
                    taken->source = source;
                    taken->order.reset(evnt.order_->clone());
                });
        ON_CALL(processor, onEvent(testing::_, testing::An<const Queues::OrderCancelEvent &>()))
            .WillByDefault(
                [&taken](const std::string &source, const Queues::OrderCancelEvent &evnt)
                {
                    taken.emplace();
                    taken->source = source;
                    taken->cancel = evnt;
                });
        ON_CALL(processor, onEvent(testing::_, testing::An<const Queues::OrderReplaceEvent &>()))
            .WillByDefault(
                [&taken](const std::string &source, const Queues::OrderReplaceEvent &)
                {
                    taken.emplace();
                    taken->source = source;
                    taken->replace = true;
                });
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!inQueues_.pop(&processor) && (std::chrono::steady_clock::now() < deadline))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return taken;
    }

    net::io_context ioc_;
    std::thread serverThread_;
    std::shared_ptr<WsServer> server_;
    SessionManager sessionMgr_;
    Queues::IncomingQueues inQueues_;
    OrderBookImpl orderBook_;
    std::unique_ptr<WsOutQueues> out_;
    std::shared_ptr<WsCredentials> credentials_;
    tcp::endpoint endpoint_;
    int markers_ = 0;
    SourceIdT instrumentId_;
    SourceIdT tradingId_;
    SourceIdT clientId_;
    SourceIdT sharedId_;
    SourceIdT serverDefaultId_;
    SourceIdT clearingId_;
};

} // namespace

TEST_F(WsAuthorizationTest, ASessionGetsOnlyItsIdentitysAccountsAndTheirOrders)
{
    storeOrder(1, tradingId_, "A-1");
    storeOrder(2, clientId_, "B-1");
    storeOrder(3, sharedId_, "S-1");
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);
    std::unique_ptr<Client> b = connect(TOKEN_B);

    EXPECT_EQ((std::set<std::string>{ "TRADING-1", "SHARED" }), namesIn(a->accounts));
    EXPECT_EQ((std::set<u64>{ 1, 3 }), idsIn(a->snapshot));
    EXPECT_EQ((std::set<std::string>{ "CLIENT-A", "SHARED" }), namesIn(b->accounts));
    EXPECT_EQ((std::set<u64>{ 2, 3 }), idsIn(b->snapshot)) << "two identities that share an account both see it";
}

TEST_F(WsAuthorizationTest, AnOrdersReportsReachOnlyTheSessionsThatMaySeeItsAccount)
{
    // Both sides of one trade: each side's report and order update go to its own identity alone
    const OrderEntry *buy = storeOrder(1, tradingId_, "A-1");
    const OrderEntry *sell = storeOrder(2, clientId_, "B-1");
    const OrderEntry *shared = storeOrder(3, sharedId_, "S-1");
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);
    std::unique_ptr<Client> b = connect(TOKEN_B);

    report(*buy, "desk-a");
    EXPECT_EQ((std::vector<std::string>{ "execution_report", "order_update" }), typesBeforeMarker(*a));
    EXPECT_TRUE(typesBeforeMarker(*b).empty()) << "desk-b got a report of desk-a's order";

    report(*sell, "desk-b");
    EXPECT_TRUE(typesBeforeMarker(*a).empty()) << "desk-a got a report of desk-b's order";
    EXPECT_EQ((std::vector<std::string>{ "execution_report", "order_update" }), typesBeforeMarker(*b));

    report(*shared, "desk-a");
    EXPECT_EQ((std::vector<std::string>{ "execution_report", "order_update" }), typesBeforeMarker(*a));
    EXPECT_EQ((std::vector<std::string>{ "execution_report", "order_update" }), typesBeforeMarker(*b));
}

TEST_F(WsAuthorizationTest, AReportOfAnOrderTheStorageLacksGoesOnlyToItsSource)
{
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);
    std::unique_ptr<Client> b = connect(TOKEN_B);
    OrderEntry ghost(SourceIdT(), SourceIdT(), SourceIdT(), SourceIdT(), instrumentId_, clientId_, clearingId_,
                     SourceIdT());
    ghost.orderId_ = IdT(99, 20261015);
    report(ghost, "desk-a");
    EXPECT_EQ(std::vector<std::string>{ "execution_report" }, typesBeforeMarker(*a));
    EXPECT_TRUE(typesBeforeMarker(*b).empty()) << "a report of an unknown order was broadcast";
}

TEST_F(WsAuthorizationTest, ACancelRejectGoesToTheOrdersAccountOrForAnUnknownOrderToTheRequester)
{
    const OrderEntry *order = storeOrder(1, tradingId_, "A-1");
    const OrderEntry *shared = storeOrder(3, sharedId_, "S-1");
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);
    std::unique_ptr<Client> b = connect(TOKEN_B);

    Queues::CancelRejectEvent tooLate;
    tooLate.id_ = order->orderId_;
    tooLate.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    out_->push(tooLate, "desk-a");
    EXPECT_EQ(std::vector<std::string>{ "cancel_reject" }, typesBeforeMarker(*a));
    EXPECT_TRUE(typesBeforeMarker(*b).empty());

    // An order in an account both identities have: both, whoever asked
    Queues::CancelRejectEvent sharedTooLate;
    sharedTooLate.id_ = shared->orderId_;
    sharedTooLate.reason_ = Queues::CancelRejectEvent::TOO_LATE;
    out_->push(sharedTooLate, "desk-a");
    EXPECT_EQ(std::vector<std::string>{ "cancel_reject" }, typesBeforeMarker(*a));
    EXPECT_EQ(std::vector<std::string>{ "cancel_reject" }, typesBeforeMarker(*b));

    Queues::CancelRejectEvent unknown;
    unknown.id_ = IdT(1234, 0);
    out_->push(unknown, "desk-b");
    EXPECT_TRUE(typesBeforeMarker(*a).empty()) << "desk-a got the answer to desk-b's cancel";
    EXPECT_EQ(std::vector<std::string>{ "cancel_reject" }, typesBeforeMarker(*b));
}

TEST_F(WsAuthorizationTest, ARefusedUnstoredOrderIsReportedOnlyToItsSender)
{
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);
    std::unique_ptr<Client> b = connect(TOKEN_B);
    Queues::OrderRejectEvent reject;
    reject.reason_ = "ClOrdID in use";
    out_->push(reject, "desk-b");
    EXPECT_TRUE(typesBeforeMarker(*a).empty());
    EXPECT_EQ(std::vector<std::string>{ "execution_report" }, typesBeforeMarker(*b));
}

TEST_F(WsAuthorizationTest, AnOrderTradesOnlyTheIdentitysAccountsAndIsRecordedAsItsIdentitys)
{
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);

    // Naming none: the identity's first account, not the server's default
    a->ws.write(net::buffer(std::string(
        R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","tif":"DAY","currency":"USD","capacity":"AGENCY","price":150.0,"orderQty":10}})")));
    std::optional<Queued> queued = takeQueued();
    ASSERT_TRUE(queued && queued->order);
    EXPECT_EQ("desk-a", queued->source) << "the order went to the engine under another source";
    EXPECT_EQ(tradingId_, queued->order->account_.getId());
    EXPECT_EQ("desk-a", queued->order->source_.get());

    // Another identity's account is refused, in the words an account that doesn't exist gets
    a->ws.write(net::buffer(std::string(
        R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","tif":"DAY","currency":"USD","capacity":"AGENCY","price":150.0,"orderQty":10,"account":"CLIENT-A"}})")));
    std::optional<nlohmann::json> refusal = readUntil(*a, "error");
    ASSERT_TRUE(refusal);
    EXPECT_EQ("Order refused: account CLIENT-A isn't one this connection may trade", refusal->value("message", ""));
    a->ws.write(net::buffer(std::string(
        R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT","tif":"DAY","currency":"USD","capacity":"AGENCY","price":150.0,"orderQty":10,"account":"NO-SUCH"}})")));
    refusal = readUntil(*a, "error");
    ASSERT_TRUE(refusal);
    EXPECT_EQ("Order refused: account NO-SUCH isn't one this connection may trade", refusal->value("message", ""));
    EXPECT_FALSE(takeQueued(std::chrono::milliseconds(0))) << "a refused order reached the engine";
}

TEST_F(WsAuthorizationTest, AnotherIdentitysOrderCantBeCanceledOrReplaced)
{
    const OrderEntry *theirs = storeOrder(7, clientId_, "B-7");
    start();
    std::unique_ptr<Client> a = connect(TOKEN_A);

    // A cancel of it reaches the engine as a cancel of an unknown order, by its number or by its ClOrdID
    a->ws.write(net::buffer(std::string(R"({"type":"cancel_order","data":{"orderId":7}})")));
    std::optional<Queued> queued = takeQueued();
    ASSERT_TRUE(queued && queued->cancel);
    EXPECT_EQ("desk-a", queued->source);
    EXPECT_EQ(IdT(7, 0), queued->cancel->id_) << "another identity's order was named to the engine";
    EXPECT_NE(theirs->orderId_, queued->cancel->id_);
    a->ws.write(net::buffer(std::string(R"({"type":"cancel_order","data":{"orderId":7,"clOrderId":"B-7"}})")));
    queued = takeQueued();
    ASSERT_TRUE(queued && queued->cancel);
    EXPECT_EQ(IdT(7, 0), queued->cancel->id_);

    // A replace of it is refused as for an order that doesn't exist
    a->ws.write(net::buffer(std::string(R"({"type":"replace_order","data":{"orderId":7,"price":151.0}})")));
    std::optional<nlohmann::json> refusal = readUntil(*a, "error");
    ASSERT_TRUE(refusal);
    EXPECT_EQ("Order not found for replace: 7", refusal->value("message", ""));
    EXPECT_FALSE(takeQueued(std::chrono::milliseconds(0)))
        << "a replace of another identity's order reached the engine";
}

TEST_F(WsAuthorizationTest, WithoutIdentitiesEverySessionSeesEverythingAsBefore)
{
    const OrderEntry *buy = storeOrder(1, tradingId_, "A-1");
    storeOrder(2, clientId_, "B-1");
    start(false);
    std::unique_ptr<Client> one = connect("");
    std::unique_ptr<Client> two = connect("");
    EXPECT_EQ((std::set<std::string>{ "TRADING-1", "CLIENT-A", "SHARED", "HOUSE-DEFAULT" }), namesIn(one->accounts));
    EXPECT_EQ((std::set<u64>{ 1, 2 }), idsIn(one->snapshot));

    report(*buy, "WebSocket");
    EXPECT_EQ((std::vector<std::string>{ "execution_report", "order_update" }), typesBeforeMarker(*one));
    EXPECT_EQ((std::vector<std::string>{ "execution_report", "order_update" }), typesBeforeMarker(*two));

    Queues::OrderRejectEvent reject;
    out_->push(reject, "WebSocket");
    EXPECT_EQ(std::vector<std::string>{ "execution_report" }, typesBeforeMarker(*one));
    EXPECT_EQ(std::vector<std::string>{ "execution_report" }, typesBeforeMarker(*two));
}
