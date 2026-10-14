/**
 Concurrent Order Processor library - the PostgreSQL schema against the strings the write-behind sends (#151)

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "PGEnumStrings.h"

using namespace COP;

namespace
{

/// The project's root: the first directory up from the test's that has README.md and CMakeLists.txt
std::filesystem::path projectRoot()
{
    auto p = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(p / "README.md") && std::filesystem::exists(p / "CMakeLists.txt"))
        {
            return p;
        }
        if (!p.has_parent_path())
        {
            break;
        }
        p = p.parent_path();
    }
    ADD_FAILURE() << "Could not locate the project root from " << std::filesystem::current_path();
    return std::filesystem::current_path();
}

/// Each enum type docker/init-db's scripts create, and its values
std::map<std::string, std::set<std::string>> schemaEnums()
{
    std::map<std::string, std::set<std::string>> types;
    const std::filesystem::path dir = projectRoot() / "docker" / "init-db";
    for (const auto &file : std::filesystem::directory_iterator(dir))
    {
        if (".sql" != file.path().extension())
        {
            continue;
        }
        std::ifstream in(file.path());
        std::stringstream text;
        text << in.rdbuf();
        const std::string sql = text.str();
        static const std::regex type(R"(CREATE TYPE (\w+) AS ENUM \(([^;]*)\);)");
        static const std::regex value(R"('([^']+)')");
        for (auto it = std::sregex_iterator(sql.begin(), sql.end(), type); std::sregex_iterator() != it; ++it)
        {
            const std::string values = (*it)[2].str();
            auto &set = types[(*it)[1].str()];
            for (auto v = std::sregex_iterator(values.begin(), values.end(), value); std::sregex_iterator() != v; ++v)
            {
                set.insert((*v)[1].str());
            }
        }
    }
    return types;
}

/// Expects every value of E from 1 to last that PGEnumStrings maps to be one the schema's type has
template <typename E>
void expectInSchema(const std::map<std::string, std::set<std::string>> &schema, const std::string &type, E last)
{
    const auto it = schema.find(type);
    ASSERT_NE(schema.end(), it) << "docker/init-db creates no enum type " << type;
    for (int v = 1; v <= static_cast<int>(last); ++v)
    {
        const char *sql = PG::toSQL(static_cast<E>(v));
        ASSERT_NE(nullptr, sql) << type << " value " << v << " has no string in PGEnumStrings.h";
        EXPECT_EQ(1u, it->second.count(sql))
            << "the write-behind sends " << type << " '" << sql
            << "', which the schema's type doesn't have: Postgres would refuse the row";
    }
}

TEST(PGSchemaTest, EveryEnumStringIsOneTheSchemasTypeHas)
{
    // A string the schema lacks makes Postgres refuse the row: the FX branch drifted so before #154. Nothing checked them
    // against each other on master (#151).
    const auto schema = schemaEnums();
    expectInSchema(schema, "side", CROSS_SIDE);
    expectInSchema(schema, "order_type", STOPLIMIT_ORDERTYPE);
    expectInSchema(schema, "order_status", CANCELED_ORDSTATUS);
    expectInSchema(schema, "time_in_force", ATCLOSE_TIF);
    expectInSchema(schema, "capacity", AGENT_FOR_ANOTHER_MEMBER_CAPACITY);
    expectInSchema(schema, "currency", EUR_CURRENCY);
    expectInSchema(schema, "settl_type", TENOR_SETTLTYPE);
    expectInSchema(schema, "account_type", AGENCY_ACCOUNTTYPE);
}

} // namespace
