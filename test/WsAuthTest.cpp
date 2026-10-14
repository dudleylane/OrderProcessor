/**
 Concurrent Order Processor library - WebSocket authentication Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "IncomingQueues.h"
#include "OrderBookImpl.h"
#include "OrderStorage.h"
#include "ServerArgs.h"
#include "SessionManager.h"
#include "TestAux.h"
#include "WideDataStorage.h"
#include "WsAuth.h"
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

const std::string TOKEN = "0123456789abcdef0123456789abcdef";

// --- What decides an upgrade, by itself ---

TEST(WsAuthTest, ATokenIsLongEnoughAndMadeOfSubprotocolCharacters)
{
    EXPECT_TRUE(isValidWsToken(TOKEN));
    EXPECT_TRUE(isValidWsToken("abcdefghij.ABCDEFGHIJ_0123-xyz"));
    EXPECT_FALSE(isValidWsToken("short"));
    EXPECT_FALSE(isValidWsToken("0123456789abcde")); // 15
    EXPECT_FALSE(isValidWsToken("0123456789abcdef/xyz"));
    EXPECT_FALSE(isValidWsToken("0123456789abcdef=xyz"));
    EXPECT_FALSE(isValidWsToken("0123456789abcdef xyz"));
    EXPECT_FALSE(isValidWsToken("0123456789abcdef,xyz"));
}

TEST(WsAuthTest, WithoutATokenEveryClientIsAccepted)
{
    const WsAuth open;
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(open, "", ""));
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(open, "oms-v1", "http://anywhere.example"));
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(open, "oms-v1, oms-token.whatever", ""));
}

TEST(WsAuthTest, WithATokenOnlyAClientOfferingItIsAccepted)
{
    WsAuth auth;
    auth.token = TOKEN;
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, "oms-v1, oms-token." + TOKEN, ""));
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, "oms-token." + TOKEN, ""));
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, " oms-v1 ,oms-token." + TOKEN + " ", ""));
    EXPECT_EQ(WsUpgrade::MissingToken, decideUpgrade(auth, "", ""));
    EXPECT_EQ(WsUpgrade::MissingToken, decideUpgrade(auth, "oms-v1", ""));
    EXPECT_EQ(WsUpgrade::WrongToken, decideUpgrade(auth, "oms-v1, oms-token.0123456789abcdef0123456789abcdee", ""));
    EXPECT_EQ(WsUpgrade::WrongToken, decideUpgrade(auth, "oms-token." + TOKEN + "x", ""));
    EXPECT_EQ(WsUpgrade::WrongToken, decideUpgrade(auth, "oms-token.", ""));
    // The token as a subprotocol of its own, without the prefix, isn't the token
    EXPECT_EQ(WsUpgrade::MissingToken, decideUpgrade(auth, TOKEN, ""));
}

TEST(WsAuthTest, AnOriginOffTheListIsForbiddenAndAMissingOneIsNot)
{
    WsAuth auth;
    auth.token = TOKEN;
    auth.allowedOrigins = { "http://localhost:3000", "https://oms.example" };
    const std::string offer = "oms-v1, oms-token." + TOKEN;
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, offer, "http://localhost:3000"));
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, offer, "https://oms.example"));
    EXPECT_EQ(WsUpgrade::ForbiddenOrigin, decideUpgrade(auth, offer, "https://evil.example"));
    EXPECT_EQ(WsUpgrade::ForbiddenOrigin, decideUpgrade(auth, offer, "http://localhost:3001"));
    // A client that sends no Origin isn't a browser, and the token authenticates it
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, offer, ""));
    // The Origin is checked first: a page that may not connect learns nothing about the token
    EXPECT_EQ(WsUpgrade::ForbiddenOrigin, decideUpgrade(auth, "", "https://evil.example"));
}

TEST(WsAuthTest, TheServerAnswersWithItsSubprotocolOnlyWhenOffered)
{
    EXPECT_TRUE(offersSubprotocol("oms-v1"));
    EXPECT_TRUE(offersSubprotocol("oms-token." + TOKEN + ", oms-v1"));
    EXPECT_FALSE(offersSubprotocol(""));
    EXPECT_FALSE(offersSubprotocol("oms-token." + TOKEN));
    EXPECT_FALSE(offersSubprotocol("oms-v2"));
}

// --- Where the token and the Origins come from ---

TEST(WsAuthTest, TheTokenComesFromItsFileOrTheEnvironmentButNotBoth)
{
    const std::string path = test::uniqueTestPath("ws_token");
    {
        std::ofstream file(path);
        file << TOKEN << "\r\n";
    }
    ServerArgs args;
    args.wsTokenFile = path;
    WsAuth auth;
    std::string error;
    ASSERT_TRUE(resolveWsAuth(args, nullptr, nullptr, &auth, &error)) << error;
    EXPECT_EQ(TOKEN, auth.token);

    EXPECT_FALSE(resolveWsAuth(args, TOKEN.c_str(), nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("given twice")) << error;

    ServerArgs fromEnv;
    ASSERT_TRUE(resolveWsAuth(fromEnv, TOKEN.c_str(), nullptr, &auth, &error)) << error;
    EXPECT_EQ(TOKEN, auth.token);
    // Set but empty, or a file with nothing on its first line, is a mistake that would leave the server open
    EXPECT_FALSE(resolveWsAuth(fromEnv, "", nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("set, but empty")) << error;
    {
        std::ofstream file(path);
        file << "\r\n" << TOKEN << "\n";
    }
    EXPECT_FALSE(resolveWsAuth(args, nullptr, nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("has no token")) << error;

    ServerArgs missing;
    missing.wsTokenFile = path + ".absent";
    EXPECT_FALSE(resolveWsAuth(missing, nullptr, nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("can't read the token file")) << error;
    std::remove(path.c_str());
}

TEST(WsAuthTest, ATokenThatCannotTravelInASubprotocolIsRefusedAtStartup)
{
    ServerArgs args;
    WsAuth auth;
    std::string error;
    EXPECT_FALSE(resolveWsAuth(args, "too-short", nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("at least 16")) << error;
    EXPECT_FALSE(resolveWsAuth(args, "0123456789abcdef/with/slashes", nullptr, &auth, &error));
}

TEST(WsAuthTest, OriginsComeFromTheFlagsAndTheEnvironment)
{
    ServerArgs args;
    // A trailing '/' is dropped: a browser's Origin never has one
    args.allowedOrigins = { "http://a.example/" };
    WsAuth auth;
    std::string error;
    ASSERT_TRUE(resolveWsAuth(args, nullptr, "http://b.example, https://c.example/", &auth, &error)) << error;
    EXPECT_EQ((std::vector<std::string>{ "http://a.example", "http://b.example", "https://c.example" }),
              auth.allowedOrigins);
}

// --- A real server ---

const std::chrono::milliseconds REPLY_TIMEOUT(5000);

/// A real WsServer on loopback with the access settings a test gives it (#146)
class WsServerAuthTest : public ::testing::Test
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
        OrderBookImpl::InstrumentsT instruments;
        instruments.insert(WideDataStorage::instance()->add(instr));
        orderBook_.init(instruments);
    }

    void TearDown() override
    {
        ioc_.stop();
        if (serverThread_.joinable())
        {
            serverThread_.join();
        }
        server_.reset();
        OrderStorage::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    void start(const WsAuth &auth)
    {
        server_ = std::make_shared<WsServer>(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), &sessionMgr_,
                                             WideDataStorage::instance(), OrderStorage::instance(), &inQueues_,
                                             IdTGenerator::instance(), &orderBook_, SourceIdT(), SourceIdT(),
                                             WsLimits(), auth);
        endpoint_ = server_->localEndpoint();
        server_->run();
        serverThread_ = std::thread(
            [this]
            {
                ioc_.run();
            });
    }

    struct Upgrade
    {
        beast::error_code ec;
        websocket::response_type response;
        std::string firstMessage; // what the server sent first, once it accepted
    };

    /// A WebSocket handshake to /ws offering the subprotocols and sending the Origin given, each left out when empty
    Upgrade upgrade(const std::string &subprotocols, const std::string &origin)
    {
        Upgrade result;
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws(ioc);
        beast::get_lowest_layer(ws).expires_after(REPLY_TIMEOUT);
        beast::get_lowest_layer(ws).connect(endpoint_, result.ec);
        if (result.ec)
        {
            return result;
        }
        ws.set_option(websocket::stream_base::decorator(
            [subprotocols, origin](websocket::request_type &req)
            {
                if (!subprotocols.empty())
                {
                    req.set(http::field::sec_websocket_protocol, subprotocols);
                }
                if (!origin.empty())
                {
                    req.set(http::field::origin, origin);
                }
            }));
        ws.handshake(result.response, "127.0.0.1", "/ws", result.ec);
        if (!result.ec)
        {
            beast::flat_buffer buffer;
            ws.read(buffer, result.ec);
            result.firstMessage = beast::buffers_to_string(buffer.data());
        }
        return result;
    }

    boost::asio::io_context ioc_;
    std::thread serverThread_;
    std::shared_ptr<WsServer> server_;
    SessionManager sessionMgr_;
    Queues::IncomingQueues inQueues_;
    OrderBookImpl orderBook_;
    tcp::endpoint endpoint_;
};

TEST_F(WsServerAuthTest, WithoutATokenEveryClientConnectsAsBefore)
{
    start(WsAuth());
    const Upgrade bare = upgrade("", "");
    ASSERT_FALSE(bare.ec) << bare.ec.message();
    EXPECT_NE(std::string::npos, bare.firstMessage.find("\"connected\"")) << bare.firstMessage;
    // Nothing offered, so nothing named in the answer
    EXPECT_EQ(0u, bare.response.count(http::field::sec_websocket_protocol));

    const Upgrade offered = upgrade("oms-v1", "http://anywhere.example");
    ASSERT_FALSE(offered.ec) << offered.ec.message();
    EXPECT_EQ("oms-v1", std::string(offered.response[http::field::sec_websocket_protocol]));
}

TEST_F(WsServerAuthTest, WithATokenOnlyAClientOfferingItConnects)
{
    WsAuth auth;
    auth.token = TOKEN;
    start(auth);

    const Upgrade right = upgrade("oms-v1, oms-token." + TOKEN, "");
    ASSERT_FALSE(right.ec) << right.ec.message();
    EXPECT_NE(std::string::npos, right.firstMessage.find("\"connected\"")) << right.firstMessage;
    // The answer names our subprotocol, never the entry that carries the token
    EXPECT_EQ("oms-v1", std::string(right.response[http::field::sec_websocket_protocol]));

    for (const std::string &offer : { std::string(), std::string("oms-v1"), "oms-v1, oms-token.x" + TOKEN })
    {
        const Upgrade refused = upgrade(offer, "");
        EXPECT_EQ(websocket::error::upgrade_declined, refused.ec) << "'" << offer << "': " << refused.ec.message();
        EXPECT_EQ(http::status::unauthorized, refused.response.result()) << "'" << offer << "'";
    }
}

TEST_F(WsServerAuthTest, ABrowserFromAnOriginOffTheListIsForbidden)
{
    WsAuth auth;
    auth.token = TOKEN;
    auth.allowedOrigins = { "http://localhost:3000" };
    start(auth);
    const std::string offer = "oms-v1, oms-token." + TOKEN;

    const Upgrade allowed = upgrade(offer, "http://localhost:3000");
    EXPECT_FALSE(allowed.ec) << allowed.ec.message();
    const Upgrade noOrigin = upgrade(offer, "");
    EXPECT_FALSE(noOrigin.ec) << noOrigin.ec.message();

    const Upgrade forbidden = upgrade(offer, "https://evil.example");
    EXPECT_EQ(websocket::error::upgrade_declined, forbidden.ec) << forbidden.ec.message();
    EXPECT_EQ(http::status::forbidden, forbidden.response.result());
}

TEST_F(WsServerAuthTest, APlainHttpRequestIsAnswered)
{
    // The image's health check sends a plain GET and needs an answer, whatever it is (#158)
    WsAuth auth;
    auth.token = TOKEN;
    start(auth);
    net::io_context ioc;
    beast::tcp_stream stream(ioc);
    stream.expires_after(REPLY_TIMEOUT);
    stream.connect(endpoint_);
    http::request<http::empty_body> request(http::verb::get, "/", 11);
    request.set(http::field::host, "127.0.0.1");
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    EXPECT_EQ(http::status::upgrade_required, response.result());
    EXPECT_NE(std::string::npos, response.body().find("WebSocket")) << response.body();
}

} // namespace
