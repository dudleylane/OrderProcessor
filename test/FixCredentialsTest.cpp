#ifdef BUILD_FIX

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include "FixCredentials.h"
#include "TestAux.h"

using namespace COP::App;

namespace
{

/// A hash with few iterations, so the tests stay fast under the sanitizers
std::string quickHash(const std::string &password)
{
    return FixCredentials::hashPassword(password, 1000);
}

/// Expects parse() to refuse text, with an error that contains what
void expectRefused(const std::string &text, const std::string &what)
{
    try
    {
        FixCredentials::parse(text, "creds");
        ADD_FAILURE() << "accepted: " << text;
    }
    catch (const std::runtime_error &e)
    {
        EXPECT_NE(std::string::npos, std::string(e.what()).find(what)) << e.what();
    }
}

TEST(FixCredentialsTest, HashesWithPbkdf2HmacSha256)
{
    // RFC 7914, section 11's vectors for PBKDF2-HMAC-SHA256, to 32 bytes
    EXPECT_EQ("pbkdf2-sha256$1$73616c74$55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc",
              FixCredentials::hashPassword("passwd", 1, "salt"));
    EXPECT_EQ("pbkdf2-sha256$80000$4e61436c$4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56",
              FixCredentials::hashPassword("Password", 80000, "NaCl"));
}

TEST(FixCredentialsTest, EachHashHasItsOwnSalt)
{
    const std::string first = quickHash("secret");
    const std::string second = quickHash("secret");
    EXPECT_NE(first, second);
    EXPECT_EQ(1000u, FixCredentials::iterationsOf(first));
    EXPECT_EQ(FixCredentials::DEFAULT_ITERATIONS,
              FixCredentials::iterationsOf("pbkdf2-sha256$600000$00$" + std::string(64, '0')));
    EXPECT_EQ(0u, FixCredentials::iterationsOf("secret"));
}

TEST(FixCredentialsTest, ALogonNeedsTheUsernameAndThePassword)
{
    const FixCounterparty counterparty{ "CLIENT_A", "client_a", quickHash("secret"), { "TRADING-1" } };
    EXPECT_TRUE(FixCredentials::verify(counterparty, "client_a", "secret"));
    EXPECT_FALSE(FixCredentials::verify(counterparty, "client_a", "Secret"));
    EXPECT_FALSE(FixCredentials::verify(counterparty, "client_a", "secret "));
    EXPECT_FALSE(FixCredentials::verify(counterparty, "client_a", ""));
    EXPECT_FALSE(FixCredentials::verify(counterparty, "client_b", "secret"));
    EXPECT_FALSE(FixCredentials::verify(counterparty, "", "secret"));

    const FixCounterparty unusable{ "CLIENT_A", "client_a", "secret", { "TRADING-1" } };
    EXPECT_FALSE(FixCredentials::verify(unusable, "client_a", "secret")); // a password, not a hash
}

TEST(FixCredentialsTest, AFileNamesEachCounterpartyItsUserAndItsAccounts)
{
    const std::string hashA = quickHash("a");
    const std::string hashB = quickHash("b");
    const FixCredentials credentials = FixCredentials::parse("# CompID  Username  PasswordHash  Accounts\r\n"
                                                             "\n"
                                                             "CLIENT_A client_a " +
                                                                 hashA +
                                                                 " TRADING-1,CLIENT-A\r\n"
                                                                 "   # indented comment\n"
                                                                 "\tCLIENT_B\tclient_b\t" +
                                                                 hashB + "\tTRADING-1",
                                                             "creds");
    ASSERT_EQ(2u, credentials.counterparties().size());
    const FixCounterparty *a = credentials.find("CLIENT_A");
    ASSERT_NE(nullptr, a);
    EXPECT_EQ("client_a", a->username);
    EXPECT_EQ(hashA, a->passwordHash);
    EXPECT_EQ((std::vector<std::string>{ "TRADING-1", "CLIENT-A" }), a->accounts);
    EXPECT_TRUE(FixCredentials::mayTrade(*a, "CLIENT-A"));
    EXPECT_FALSE(FixCredentials::mayTrade(*a, "CLIENT-B"));
    EXPECT_FALSE(FixCredentials::mayTrade(*a, "trading-1"));
    const FixCounterparty *b = credentials.find("CLIENT_B");
    ASSERT_NE(nullptr, b);
    EXPECT_EQ((std::vector<std::string>{ "TRADING-1" }), b->accounts);
    EXPECT_EQ(nullptr, credentials.find("CLIENT_C"));
    EXPECT_TRUE(FixCredentials::parse("# nobody\n", "creds").counterparties().empty());
}

TEST(FixCredentialsTest, ALineThatCantBeUsedIsRefusedWithItsNumber)
{
    const std::string hash = quickHash("a");
    expectRefused("CLIENT_A client_a " + hash, "creds:1: a counterparty takes 4 fields");
    expectRefused("\nCLIENT_A client_a " + hash + " TRADING-1 extra", "creds:2: a counterparty takes 4 fields");
    expectRefused("CLIENT_A a " + hash + " T\nCLIENT_A b " + hash + " T",
                  "creds:2: the CompID CLIENT_A is given twice");
    expectRefused("CLIENT_A client_a secret TRADING-1", "creds:1: the password hash isn't pbkdf2-sha256$");
    expectRefused("CLIENT_A client_a pbkdf2-sha1$1000$00$00 TRADING-1", "the password hash isn't");
    expectRefused("CLIENT_A client_a pbkdf2-sha256$0$00$" + std::string(64, '0') + " TRADING-1", "the password hash");
    expectRefused("CLIENT_A client_a pbkdf2-sha256$-1$00$" + std::string(64, '0') + " TRADING-1", "the password hash");
    expectRefused("CLIENT_A client_a pbkdf2-sha256$1000$zz$" + std::string(64, '0') + " TRADING-1",
                  "the password hash");
    expectRefused("CLIENT_A client_a pbkdf2-sha256$1000$00$" + std::string(62, '0') + " TRADING-1",
                  "the password hash");
    expectRefused("CLIENT_A client_a " + hash + "$00 TRADING-1", "the password hash");
    expectRefused("CLIENT_A client_a " + hash + " TRADING-1,,CLIENT-A",
                  "an account in \"TRADING-1,,CLIENT-A\" is empty");
    expectRefused("CLIENT_A client_a " + hash + " TRADING-1,", "is empty");
    expectRefused("CLIENT_A client_a " + hash + " TRADING-1,TRADING-1", "the account TRADING-1 is given twice");
}

TEST(FixCredentialsTest, AFileItCantReadIsRefused)
{
    const std::string path = test::uniqueTestPath("fix_credentials");
    {
        std::ofstream file(path);
        file << "CLIENT_A client_a " << quickHash("a") << " TRADING-1\n";
    }
    EXPECT_EQ(1u, FixCredentials::load(path).counterparties().size());
    std::remove(path.c_str());
    try
    {
        FixCredentials::load(path);
        ADD_FAILURE() << "loaded a missing file";
    }
    catch (const std::runtime_error &e)
    {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("can't read the FIX credentials file " + path))
            << e.what();
    }
}

} // namespace

#endif // BUILD_FIX
