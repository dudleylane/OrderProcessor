/**
 Concurrent Order Processor library - WebSocket credentials Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ServerArgs.h"
#include "TestAux.h"
#include "WsAuth.h"
#include "WsCredentials.h"

using namespace COP;
using namespace COP::App;

namespace
{

const std::string TOKEN_A = "aaaaaaaaaaaaaaaa0123456789abcdef";
const std::string TOKEN_B = "bbbbbbbbbbbbbbbb0123456789abcdef";

/// A credentials file's line for the identity
std::string line(const std::string &name, const std::string &token, const std::string &accounts)
{
    return name + " " + WsCredentials::hashToken(token) + " " + accounts + "\n";
}

/// What parsing the text throws, or empty when it doesn't
std::string parseError(const std::string &text)
{
    try
    {
        WsCredentials::parse(text, "creds");
    }
    catch (const std::runtime_error &ex)
    {
        return ex.what();
    }
    return std::string();
}

/// Resolves account names from the map, as the server does from its data directory
std::function<SourceIdT(const std::string &)> lookupIn(const std::map<std::string, SourceIdT> &accounts)
{
    return [&accounts](const std::string &name)
    {
        const auto it = accounts.find(name);
        return (accounts.end() == it) ? SourceIdT() : it->second;
    };
}

} // namespace

TEST(WsCredentialsTest, ATokensHashIsItsSha256InLowercaseHex)
{
    // FIPS 180-2's first example
    EXPECT_EQ("sha256$ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              WsCredentials::hashToken("abc"));
}

TEST(WsCredentialsTest, EachIdentityIsFoundByItsOwnToken)
{
    const WsCredentials credentials = WsCredentials::parse("# Identity  TokenHash  Accounts\r\n\r\n" +
                                                               line("desk-a", TOKEN_A, "TRADING-1,TRADING-2") +
                                                               "   \t\n" + line("desk-b", TOKEN_B, "CLIENT-A"),
                                                           "creds");
    ASSERT_EQ(2u, credentials.size());
    const std::shared_ptr<const WsIdentity> a = credentials.find(TOKEN_A);
    ASSERT_NE(nullptr, a);
    EXPECT_EQ("desk-a", a->name);
    EXPECT_EQ((std::vector<std::string>{ "TRADING-1", "TRADING-2" }), a->accountNames);
    const std::shared_ptr<const WsIdentity> b = credentials.find(TOKEN_B);
    ASSERT_NE(nullptr, b);
    EXPECT_EQ("desk-b", b->name);
    EXPECT_EQ(nullptr, credentials.find(TOKEN_A + "x"));
    EXPECT_EQ(nullptr, credentials.find(""));
}

TEST(WsCredentialsTest, ALineItCantUseIsRefusedWithItsNumber)
{
    const std::string hash = WsCredentials::hashToken(TOKEN_A);
    const struct
    {
        std::string text;
        std::string says;
    } cases[] = {
        { "desk-a " + hash + "\n", "creds:1: an identity takes 3 fields" },
        { "desk-a " + hash + " A B\n", "creds:1: an identity takes 3 fields" },
        { line("desk-a", TOKEN_A, "A") + line("desk-a", TOKEN_B, "B"), "creds:2: the name desk-a is given twice" },
        { line("desk-a", TOKEN_A, "A") + line("desk-b", TOKEN_A, "B"),
          "creds:2: the token hash of desk-b is another identity's too" },
        { "desk/a " + hash + " A\n", "the name desk/a isn't" },
        { "desk-a sha256$" + std::string(64, 'A') + " A\n", "the token hash isn't" },
        { "desk-a sha256$abc A\n", "the token hash isn't" },
        { "desk-a pbkdf2-sha256$1$aa$bb A\n", "the token hash isn't" },
        { "desk-a " + hash + " A,,B\n", "an account in \"A,,B\" is empty" },
        { "desk-a " + hash + " A,\n", "is empty" },
        { "desk-a " + hash + " A,B,A\n", "the account A is given twice" },
    };
    for (const auto &c : cases)
    {
        const std::string error = parseError(c.text);
        EXPECT_NE(std::string::npos, error.find(c.says)) << "text: " << c.text << "\nerror: " << error;
    }
}

TEST(WsCredentialsTest, AccountsResolveToIdsAndTheFirstNamedIsTheDefault)
{
    const std::map<std::string, SourceIdT> accounts = { { "ALPHA", SourceIdT(5, 1) }, { "ZED", SourceIdT(9, 1) } };
    WsCredentials credentials = WsCredentials::parse(line("desk-a", TOKEN_A, "ZED,ALPHA"), "creds");
    credentials.resolveAccounts(lookupIn(accounts));
    const std::shared_ptr<const WsIdentity> a = credentials.find(TOKEN_A);
    ASSERT_NE(nullptr, a);
    EXPECT_EQ(SourceIdT(9, 1), a->defaultAccount) << "the first account the file names, not the lowest id";
    EXPECT_TRUE(a->mayUse(SourceIdT(5, 1)));
    EXPECT_TRUE(a->mayUse(SourceIdT(9, 1)));
    EXPECT_FALSE(a->mayUse(SourceIdT(7, 1)));
    EXPECT_FALSE(a->mayUse(SourceIdT()));

    WsCredentials unknown = WsCredentials::parse(line("desk-b", TOKEN_B, "ALPHA,GONE"), "creds");
    try
    {
        unknown.resolveAccounts(lookupIn(accounts));
        FAIL() << "an account the data directory lacks was taken";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_STREQ("the WebSocket identity desk-b names the account GONE, which the data directory doesn't have",
                     ex.what());
    }
}

TEST(WsCredentialsTest, AnUpgradeOfferingAnIdentitysTokenIsAcceptedAsThatIdentity)
{
    WsAuth auth;
    auth.credentials = std::make_shared<WsCredentials>(
        WsCredentials::parse(line("desk-a", TOKEN_A, "A") + line("desk-b", TOKEN_B, "B"), "creds"));
    std::shared_ptr<const WsIdentity> identity;
    EXPECT_EQ(WsUpgrade::Accept, decideUpgrade(auth, "oms-v1, oms-token." + TOKEN_B, "", &identity));
    ASSERT_NE(nullptr, identity);
    EXPECT_EQ("desk-b", identity->name);

    identity.reset();
    EXPECT_EQ(WsUpgrade::WrongToken, decideUpgrade(auth, "oms-v1, oms-token." + std::string(32, 'c'), "", &identity));
    EXPECT_EQ(nullptr, identity);
    EXPECT_EQ(WsUpgrade::MissingToken, decideUpgrade(auth, "oms-v1", "", &identity));
    EXPECT_EQ(nullptr, identity);

    // A wrong token offered beside a right one still opens the right identity's session
    EXPECT_EQ(WsUpgrade::Accept,
              decideUpgrade(auth, "oms-token.wrongwrongwrongwrong, oms-token." + TOKEN_A, "", &identity));
    ASSERT_NE(nullptr, identity);
    EXPECT_EQ("desk-a", identity->name);
    identity.reset();
    EXPECT_EQ(WsUpgrade::Accept,
              decideUpgrade(auth, "oms-token." + TOKEN_B + ", oms-token.wrongwrongwrongwrong", "", &identity));
    ASSERT_NE(nullptr, identity);
    EXPECT_EQ("desk-b", identity->name);

    // The Origin list still applies
    auth.allowedOrigins = { "https://oms.example" };
    EXPECT_EQ(WsUpgrade::ForbiddenOrigin,
              decideUpgrade(auth, "oms-token." + TOKEN_A, "https://elsewhere.example", &identity));
}

TEST(WsCredentialsTest, CredentialsComeFromTheirFileAndNeverBesideASharedToken)
{
    const std::string path = test::uniqueTestPath("ws_credentials");
    {
        std::ofstream file(path);
        file << line("desk-a", TOKEN_A, "TRADING-1");
    }
    ServerArgs args;
    args.wsCredentialsFile = path;
    WsAuth auth;
    std::string error;
    ASSERT_TRUE(resolveWsAuth(args, nullptr, nullptr, &auth, &error)) << error;
    ASSERT_NE(nullptr, auth.credentials);
    EXPECT_TRUE(auth.token.empty());
    EXPECT_NE(nullptr, auth.credentials->find(TOKEN_A));

    // Each identity's token, or one that everybody shares: not both
    EXPECT_FALSE(resolveWsAuth(args, TOKEN_B.c_str(), nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("can't both be given")) << error;

    // A file that names nobody would let nobody in: a mistake, said at startup
    {
        std::ofstream file(path);
        file << "# nobody yet\n";
    }
    EXPECT_FALSE(resolveWsAuth(args, nullptr, nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("names no identity")) << error;

    ServerArgs missing;
    missing.wsCredentialsFile = path + ".absent";
    EXPECT_FALSE(resolveWsAuth(missing, nullptr, nullptr, &auth, &error));
    EXPECT_NE(std::string::npos, error.find("can't read the WebSocket credentials file")) << error;
    std::remove(path.c_str());
}

TEST(WsCredentialsTest, TheServerTakesTheCredentialsFlags)
{
    std::string error;
    const std::optional<ServerArgs> parsed =
        parseServerArgs({ "--ws-credentials", "/etc/orderprocessor/ws-credentials", "--hash-ws-token" }, &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ("/etc/orderprocessor/ws-credentials", parsed->wsCredentialsFile);
    EXPECT_TRUE(parsed->hashWsToken);
    EXPECT_FALSE(parseServerArgs({ "--ws-credentials" }, &error));
}
