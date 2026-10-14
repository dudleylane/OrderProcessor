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
    EXPECT_EQ("./data", parsed->dataDir);
    EXPECT_EQ(0, parsed->workers);
    EXPECT_EQ(-1, parsed->cpuAffinityStart);
    EXPECT_FALSE(parsed->hugePages);
    EXPECT_TRUE(parsed->fixCfg.empty()) << "no FIX gateway unless asked for";
    EXPECT_EQ(COP::App::DEFAULT_CLEARING_FIRM, parsed->clearingFirm);
    EXPECT_EQ(COP::App::DEFAULT_ACCOUNT, parsed->defaultAccount);
    EXPECT_FALSE(parsed->debug);
    EXPECT_EQ(COP::Store::LMDBStorage::DEFAULT_MAP_SIZE >> 20, parsed->lmdbMapMb);
    EXPECT_EQ(COP::App::DEFAULT_MAX_CONNECTIONS, parsed->wsLimits.maxConnections);
    EXPECT_EQ(COP::App::DEFAULT_MAX_SESSION_QUEUE_MB << 20, parsed->wsLimits.maxQueuedBytes);
    EXPECT_FALSE(parsed->help);
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
                                                     "8" });
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
        { "--max-session-queue-mb", "1" },
        { "--help" },
    };
    for (const std::vector<std::string> &option : options)
    {
        EXPECT_TRUE(parse(option)) << option[0];
        EXPECT_NE(std::string::npos, usage.find(" " + option[0] + " ")) << option[0] << " isn't in the usage";
    }
}

TEST(ServerArgsTest, TheFixGatewaysSettingsNeedAFixBuild)
{
    // On this branch --fix-cfg starts the FIX gateway. A build without it refuses the flag rather than run without the
    // gateway it was asked for, as the server refuses any value it can't use (#173).
    std::string error;
#ifdef BUILD_FIX
    const std::optional<ServerArgs> parsed = parse({ "--fix-cfg", "config/fix.cfg" }, &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ("config/fix.cfg", parsed->fixCfg);
    EXPECT_NE(std::string::npos, serverUsage().find(" --fix-cfg ")) << "--fix-cfg isn't in the usage";
#else
    EXPECT_FALSE(parse({ "--fix-cfg", "config/fix.cfg" }, &error));
    EXPECT_EQ("--fix-cfg needs a server built with BUILD_FIX=ON", error);
    EXPECT_EQ(std::string::npos, serverUsage().find("--fix-cfg")) << "the usage offers a flag this build refuses";
#endif
    EXPECT_FALSE(parse({ "--fix-cfg", "" }, &error));
    EXPECT_EQ("--fix-cfg needs a value", error);
}

} // namespace
