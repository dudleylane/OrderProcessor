/**
 Concurrent Order Processor library - orderProcessorServer argument Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "ServerArgs.h"

using COP::App::parseServerArgs;
using COP::App::ServerArgs;
using COP::App::serverUsage;

namespace
{

/// The arguments parsed, or nullopt with the reason in *error for arguments refused
std::optional<ServerArgs> parse(const std::vector<std::string> &args, std::string *error = nullptr)
{
    std::string ignored;
    return parseServerArgs(args, (nullptr != error) ? error : &ignored);
}

TEST(ServerArgsTest, NoArgumentsGiveTheDefaults)
{
    const std::optional<ServerArgs> parsed = parse({});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(8080, parsed->port);
    EXPECT_EQ("0.0.0.0", parsed->bindAddress);
    EXPECT_EQ("./data", parsed->dataDir);
    EXPECT_EQ(0, parsed->workers);
    EXPECT_EQ(-1, parsed->cpuAffinityStart);
    EXPECT_FALSE(parsed->hugePages);
    EXPECT_EQ(COP::App::DEFAULT_CLEARING_FIRM, parsed->clearingFirm);
    EXPECT_EQ(COP::App::DEFAULT_ACCOUNT, parsed->defaultAccount);
    EXPECT_FALSE(parsed->debug);
    EXPECT_EQ(COP::Store::LMDBStorage::DEFAULT_MAP_SIZE >> 20, parsed->lmdbMapMb);
    EXPECT_EQ(COP::App::DEFAULT_MAX_CONNECTIONS, parsed->wsLimits.maxConnections);
    EXPECT_EQ(COP::App::DEFAULT_MAX_SESSION_QUEUE_MB << 20, parsed->wsLimits.maxQueuedBytes);
    EXPECT_FALSE(parsed->help);
    EXPECT_TRUE(parsed->wsTokenFile.empty());
    EXPECT_TRUE(parsed->allowedOrigins.empty());
}

TEST(ServerArgsTest, EveryOptionIsRead)
{
    const std::optional<ServerArgs> parsed = parse({ "--port",
                                                     "9090",
                                                     "--data-dir",
                                                     "/data",
                                                     "--workers",
                                                     "4",
                                                     "--cpu-affinity",
                                                     "2",
                                                     "--huge-pages",
                                                     "--clearing-firm",
                                                     "FIRM",
                                                     "--default-account",
                                                     "ACCT",
                                                     "--debug",
                                                     "--lmdb-map-mb",
                                                     "512",
                                                     "--max-connections",
                                                     "64",
                                                     "--max-session-queue-mb",
                                                     "8",
                                                     "--bind",
                                                     "127.0.0.1",
                                                     "--ws-token-file",
                                                     "/run/secrets/oms_ws_token",
                                                     "--allowed-origin",
                                                     "http://a.example",
                                                     "--allowed-origin",
                                                     "https://b.example",
                                                     "--pg-connection",
                                                     "host=db dbname=oms" });
    ASSERT_TRUE(parsed);
    EXPECT_EQ(9090, parsed->port);
    EXPECT_EQ("/data", parsed->dataDir);
    EXPECT_EQ(4, parsed->workers);
    EXPECT_EQ(2, parsed->cpuAffinityStart);
    EXPECT_TRUE(parsed->hugePages);
    EXPECT_EQ("FIRM", parsed->clearingFirm);
    EXPECT_EQ("ACCT", parsed->defaultAccount);
    EXPECT_TRUE(parsed->debug);
    EXPECT_EQ(512u, parsed->lmdbMapMb);
    EXPECT_EQ(64u, parsed->wsLimits.maxConnections);
    EXPECT_EQ(8u << 20, parsed->wsLimits.maxQueuedBytes);
    EXPECT_EQ("127.0.0.1", parsed->bindAddress);
    EXPECT_EQ("/run/secrets/oms_ws_token", parsed->wsTokenFile);
    EXPECT_EQ((std::vector<std::string>{ "http://a.example", "https://b.example" }), parsed->allowedOrigins);
    EXPECT_EQ("host=db dbname=oms", parsed->pgConnection);
}

TEST(ServerArgsTest, ThePostgresConnectionComesFromItsFlagOrTheEnvironmentButNotBoth)
{
    // #151: the server mirrors to Postgres only when given a connection string
    ServerArgs neither;
    std::string connection = "unchanged";
    std::string error;
    ASSERT_TRUE(resolvePgConnection(neither, nullptr, &connection, &error)) << error;
    EXPECT_TRUE(connection.empty()); // LMDB only, as before
    EXPECT_FALSE(resolvePgConnection(neither, "", &connection, &error));
    EXPECT_NE(std::string::npos, error.find("set, but empty")) << error;
    ServerArgs flag;
    flag.pgConnection = "host=db";
    EXPECT_FALSE(resolvePgConnection(flag, "host=other", &connection, &error));
    EXPECT_NE(std::string::npos, error.find("given twice")) << error;
#ifdef BUILD_PG
    ASSERT_TRUE(resolvePgConnection(flag, nullptr, &connection, &error)) << error;
    EXPECT_EQ("host=db", connection);
    ASSERT_TRUE(resolvePgConnection(neither, "host=env", &connection, &error)) << error;
    EXPECT_EQ("host=env", connection);
#else
    // A build that can't use it says so, rather than run without the mirror it was asked for
    EXPECT_FALSE(resolvePgConnection(flag, nullptr, &connection, &error));
    EXPECT_NE(std::string::npos, error.find("without BUILD_PG")) << error;
#endif
}

TEST(ServerArgsTest, BindTakesAnIpAddress)
{
    // #146: the address the server listens on, which used to be 0.0.0.0 always
    std::string error;
    ASSERT_TRUE(parse({ "--bind", "::1" }, &error)) << error;
    EXPECT_FALSE(parse({ "--bind", "localhost" }, &error));
    EXPECT_EQ("--bind needs an IP address, not 'localhost'", error);
    EXPECT_FALSE(parse({ "--bind", "" }, &error));
    EXPECT_EQ("--bind needs a value", error);
}

TEST(ServerArgsTest, AnUnknownOptionIsRefused)
{
    // The issue's example: the server used to run with its defaults (#173)
    std::string error;
    EXPECT_FALSE(parse({ "--workres", "4" }, &error));
    EXPECT_EQ("unknown option: --workres", error);
    EXPECT_FALSE(parse({ "4" }, &error));
    EXPECT_EQ("unknown option: 4", error);
}

TEST(ServerArgsTest, AnOptionWithoutItsValueIsRefused)
{
    std::string error;
    EXPECT_FALSE(parse({ "--port" }, &error));
    EXPECT_EQ("--port needs a value", error);
    EXPECT_FALSE(parse({ "--data-dir", "" }, &error));
    EXPECT_EQ("--data-dir needs a value", error);
    EXPECT_FALSE(parse({ "--clearing-firm", "" }, &error));
    EXPECT_EQ("--clearing-firm needs a value", error);
}

TEST(ServerArgsTest, ANumberOutOfItsRangeIsRefused)
{
    // Each was read before #173: 70000 wrapped to port 4464, a negative worker count ran 2 processor pairs, a map of
    // 0 MiB became 1, and "abc" threw from std::stoi.
    const std::vector<std::vector<std::string>> refused = {
        { "--port", "70000" },
        { "--port", "-1" },
        { "--port", "abc" },
        { "--port", "80x" },
        { "--port", " 80" },
        { "--workers", "-1" },
        { "--cpu-affinity", "-2" },
        { "--lmdb-map-mb", "0" },
        { "--max-connections", "0" },
        { "--max-session-queue-mb", "0" },
        { "--max-session-queue-mb", "99999999999999999999" },
    };
    for (const std::vector<std::string> &args : refused)
    {
        std::string error;
        EXPECT_FALSE(parse(args, &error)) << args[0] << " " << args[1];
        EXPECT_EQ(0u, error.find(args[0] + " needs a whole number ")) << error;
        EXPECT_NE(std::string::npos, error.find("'" + args[1] + "'")) << error;
    }
}

TEST(ServerArgsTest, PortZeroLetsTheSystemChoose)
{
    const std::optional<ServerArgs> parsed = parse({ "--port", "0" });
    ASSERT_TRUE(parsed);
    EXPECT_EQ(0, parsed->port);
}

TEST(ServerArgsTest, HelpIsAsked)
{
    for (const char *help : { "--help", "-h" })
    {
        const std::optional<ServerArgs> parsed = parse({ help });
        ASSERT_TRUE(parsed) << help;
        EXPECT_TRUE(parsed->help) << help;
    }
}

TEST(ServerArgsTest, TheUsageNamesEveryOption)
{
    const std::string usage = serverUsage();
    const std::vector<std::vector<std::string>> options = {
        { "--port", "1" },
        { "--data-dir", "d" },
        { "--workers", "1" },
        { "--cpu-affinity", "0" },
        { "--huge-pages" },
        { "--clearing-firm", "f" },
        { "--default-account", "a" },
        { "--debug" },
        { "--lmdb-map-mb", "1" },
        { "--max-connections", "1" },
        { "--bind", "0.0.0.0" },
        { "--ws-token-file", "f" },
        { "--allowed-origin", "o" },
        { "--pg-connection", "s" },
        { "--max-session-queue-mb", "1" },
        { "--help" },
    };
    for (const std::vector<std::string> &option : options)
    {
        EXPECT_TRUE(parse(option)) << option[0];
        EXPECT_NE(std::string::npos, usage.find(" " + option[0] + " ")) << option[0] << " isn't in the usage";
    }
}

} // namespace
