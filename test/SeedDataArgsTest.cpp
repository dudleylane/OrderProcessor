/**
 Concurrent Order Processor library - seedData argument Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "SeedDataArgs.h"

using COP::App::parseSeedDataArgs;
using COP::App::SeedDataArgs;

namespace
{

/// The data directory parsed, or nullopt for arguments refused
std::optional<std::string> parse(const std::vector<std::string> &args, std::string *error = nullptr)
{
    std::string ignored;
    const std::optional<SeedDataArgs> parsed = parseSeedDataArgs(args, (nullptr != error) ? error : &ignored);
    return parsed ? std::optional<std::string>(parsed->dataDir) : std::nullopt;
}

TEST(SeedDataArgsTest, DefaultsToDotData)
{
    EXPECT_EQ("./data", parse({}));
}

TEST(SeedDataArgsTest, AcceptsTheServersDataDirOption)
{
    // The bug this covers: the docker guide's `seedData --data-dir /data` seeded a directory named "--data-dir" (#42).
    EXPECT_EQ("/data", parse({ "--data-dir", "/data" }));
}

TEST(SeedDataArgsTest, AcceptsALoneDirectory)
{
    EXPECT_EQ("/data", parse({ "/data" }));
}

TEST(SeedDataArgsTest, RejectsAnUnknownOption)
{
    std::string error;
    EXPECT_FALSE(parse({ "--datadir", "/data" }, &error));
    EXPECT_EQ("unknown option: --datadir", error);
}

TEST(SeedDataArgsTest, RejectsDataDirWithoutADirectory)
{
    std::string error;
    EXPECT_FALSE(parse({ "--data-dir" }, &error));
    EXPECT_EQ("--data-dir needs a directory", error);
}

TEST(SeedDataArgsTest, RejectsASecondDirectory)
{
    std::string error;
    EXPECT_FALSE(parse({ "--data-dir", "/data", "/other" }, &error));
    EXPECT_EQ("the data directory is given more than once", error);
}

TEST(SeedDataArgsTest, IfEmptyIsOffUnlessAskedFor)
{
    // compose's oms-seed asks for it, so a volume in use gets no test data (#161)
    std::string error;
    EXPECT_FALSE(parseSeedDataArgs({ "--data-dir", "/data" }, &error)->ifEmpty);
    const std::optional<SeedDataArgs> before = parseSeedDataArgs({ "--if-empty", "--data-dir", "/data" }, &error);
    ASSERT_TRUE(before.has_value()) << error;
    EXPECT_TRUE(before->ifEmpty);
    EXPECT_EQ("/data", before->dataDir);
    const std::optional<SeedDataArgs> after = parseSeedDataArgs({ "/data", "--if-empty" }, &error);
    ASSERT_TRUE(after.has_value()) << error;
    EXPECT_TRUE(after->ifEmpty);
    EXPECT_EQ("/data", after->dataDir);
}

} // namespace
