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
#include "TestAux.h"

using namespace COP;

namespace
{

/// Each enum type docker/init-db's scripts create, and its values
std::map<std::string, std::set<std::string>> schemaEnums()
{
    std::map<std::string, std::set<std::string>> types;
    const std::filesystem::path dir = test::sourceRoot() / "docker" / "init-db";
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

/// The column definitions of table in docker/init-db/001_schema.sql: each column's name, and the rest of its line. The
/// comments go first: a table's body is read up to its first ");", and a comment can hold one, as #244's in orders
/// does, which hid every column after it.
std::map<std::string, std::string> schemaColumns(const std::string &table)
{
    std::ifstream in(test::sourceRoot() / "docker" / "init-db" / "001_schema.sql");
    std::stringstream text;
    text << in.rdbuf();
    const std::string sql = std::regex_replace(text.str(), std::regex("--[^\n]*"), "");
    std::map<std::string, std::string> columns;
    const std::regex block("CREATE TABLE " + table + R"( \(([^;]*)\);)");
    std::smatch found;
    if (!std::regex_search(sql, found, block))
    {
        return columns;
    }
    const std::string body = found[1].str();
    static const std::regex column(R"(\n\s*([a-z_]+)\s+([^\n]*))");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), column); std::sregex_iterator() != it; ++it)
    {
        columns[(*it)[1].str()] = (*it)[2].str();
    }
    return columns;
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
    // A string the schema lacks makes Postgres refuse the row: the FX branch drifted so before #154. Nothing checked
    // them against each other on master (#151).
    const auto schema = schemaEnums();
    expectInSchema(schema, "side", CROSS_SIDE);
    expectInSchema(schema, "order_type", FXSWAP_ORDERTYPE); // this branch's last, with its currencies (#154)
    expectInSchema(schema, "order_status", CANCELED_ORDSTATUS);
    expectInSchema(schema, "time_in_force", ATCLOSE_TIF);
    expectInSchema(schema, "capacity", AGENT_FOR_ANOTHER_MEMBER_CAPACITY);
    expectInSchema(schema, "currency", NZD_CURRENCY);
    expectInSchema(schema, "settl_type", TENOR_SETTLTYPE);
    expectInSchema(schema, "account_type", AGENCY_ACCOUNTTYPE);
}

} // namespace

TEST(PGSchemaTest, ThePriceColumnsHoldAnyPriceTheEngineAccepts)
{
    // NUMERIC(18,8) refused a price of 1e10 or more, though the engine accepts any finite price (#538): a precision on
    // any of them brings that back
    const auto columns = schemaColumns("orders");
    ASSERT_FALSE(columns.empty()) << "001_schema.sql creates no orders table";
    for (const char *name : { "price", "stop_px", "avg_px", "day_avg_px", "far_price" })
    {
        const auto it = columns.find(name);
        ASSERT_NE(columns.end(), it) << "orders has no column " << name;
        EXPECT_EQ(0u, it->second.rfind("NUMERIC", 0)) << name << " is " << it->second;
        EXPECT_EQ(std::string::npos, it->second.find('(')) << name << " has a precision: " << it->second;
    }
}
