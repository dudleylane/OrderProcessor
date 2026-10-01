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

namespace
{

std::optional<std::string> parse(const std::vector<std::string> &args, std::string *error = nullptr)
{
    std::string ignored;
    return parseSeedDataArgs(args, (nullptr != error) ? error : &ignored);
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

} // namespace
