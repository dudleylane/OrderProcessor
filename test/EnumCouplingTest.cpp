/**
 Concurrent Order Processor library - Google Test

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

// The enums the C++ writes, as the frontend and Postgres read them (#154). Each C++ list comes from the code at run
// time: toJsonString() or toSQL() over every enumerator but INVALID. Each other list comes from its file:
// src/oms-frontend/src/types/enums.ts for the JSON, docker/init-db/001_schema.sql for Postgres. A value the C++ writes
// that a file lacks fails here, rather than in a browser that can't show it or a database that refuses the row.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "DataModelDef.h"
#include "EnumStrings.h"
#include "PGEnumStrings.h"
#include "TestAux.h"

using namespace COP;

namespace
{

using Values = std::set<std::string>;

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream f(path);
    EXPECT_TRUE(f.good()) << "can't read " << path;
    std::stringstream text;
    text << f.rdbuf();
    return text.str();
}

/// enum name -> the values of each `export enum Name { A = 'A', ... }` in a TypeScript file
std::map<std::string, Values> tsEnums(const std::string &text)
{
    std::map<std::string, Values> enums;
    const std::regex block(R"(export\s+enum\s+(\w+)\s*\{([^}]*)\})");
    const std::regex member(R"(\w+\s*=\s*'([^']*)')");
    for (std::sregex_iterator it(text.begin(), text.end(), block), end; it != end; ++it)
    {
        const std::string body = (*it)[2];
        Values &values = enums[(*it)[1]];
        for (std::sregex_iterator m(body.begin(), body.end(), member); m != end; ++m)
        {
            values.insert((*m)[1]);
        }
    }
    return enums;
}

/// type name -> the values of each `CREATE TYPE name AS ENUM ('A', ...)` in a SQL file
std::map<std::string, Values> sqlEnums(const std::string &text)
{
    std::map<std::string, Values> enums;
    const std::regex block(R"(CREATE\s+TYPE\s+(\w+)\s+AS\s+ENUM\s*\(([^)]*)\))");
    const std::regex member(R"('([^']*)')");
    for (std::sregex_iterator it(text.begin(), text.end(), block), end; it != end; ++it)
    {
        const std::string body = (*it)[2];
        Values &values = enums[(*it)[1]];
        for (std::sregex_iterator m(body.begin(), body.end(), member); m != end; ++m)
        {
            values.insert((*m)[1]);
        }
    }
    return enums;
}

/// What the JSON converter writes for each of an enum's enumerators, from 1 to the last
template <typename Enum> Values jsonValues(Enum last)
{
    Values values;
    for (int v = 1; v <= static_cast<int>(last); ++v)
    {
        values.insert(std::string(toJsonString(static_cast<Enum>(v))));
    }
    return values;
}

/// What the Postgres converter writes for each of an enum's enumerators, from 1 to the last
template <typename Enum> Values sqlValues(Enum last)
{
    Values values;
    for (int v = 1; v <= static_cast<int>(last); ++v)
    {
        const char *sql = PG::toSQL(static_cast<Enum>(v));
        values.insert((nullptr != sql) ? std::string(sql) : "<no SQL value for " + std::to_string(v) + ">");
    }
    return values;
}

void expectSame(const std::map<std::string, Values> &file, const std::string &name, const Values &cpp,
                const char *where)
{
    const auto it = file.find(name);
    ASSERT_NE(file.end(), it) << where << " has no enum " << name;
    EXPECT_EQ(cpp, it->second) << where << "'s " << name << " differs from what the C++ writes";
}

} // namespace

TEST(EnumCouplingTest, TheFrontendKnowsEveryValueTheServerWrites)
{
    const auto ts = tsEnums(readFile(test::sourceRoot() / "src/oms-frontend/src/types/enums.ts"));
    const char *where = "enums.ts";
    expectSame(ts, "Side", jsonValues(CROSS_SIDE), where);
    expectSame(ts, "OrderType", jsonValues(FXSWAP_ORDERTYPE), where);
    expectSame(ts, "TimeInForce", jsonValues(ATCLOSE_TIF), where);
    expectSame(ts, "OrderStatus", jsonValues(CANCELED_ORDSTATUS), where);
    expectSame(ts, "ExecType", jsonValues(TRIGGERED_EXECTYPE), where);
    expectSame(ts, "Currency", jsonValues(NZD_CURRENCY), where);
    expectSame(ts, "Capacity", jsonValues(AGENT_FOR_ANOTHER_MEMBER_CAPACITY), where);
    expectSame(ts, "AccountType", jsonValues(AGENCY_ACCOUNTTYPE), where);
}

TEST(EnumCouplingTest, ThePostgresSchemaHasEveryValueTheWriterWrites)
{
    // The enums PGRequestBuilder converts with toSQL(); executions aren't written, so exec_type has no C++ side
    const auto sql = sqlEnums(readFile(test::sourceRoot() / "docker/init-db/001_schema.sql"));
    const char *where = "001_schema.sql";
    expectSame(sql, "side", sqlValues(CROSS_SIDE), where);
    expectSame(sql, "order_type", sqlValues(FXSWAP_ORDERTYPE), where);
    expectSame(sql, "order_status", sqlValues(CANCELED_ORDSTATUS), where);
    expectSame(sql, "time_in_force", sqlValues(ATCLOSE_TIF), where);
    expectSame(sql, "capacity", sqlValues(AGENT_FOR_ANOTHER_MEMBER_CAPACITY), where);
    expectSame(sql, "currency", sqlValues(NZD_CURRENCY), where);
    expectSame(sql, "settl_type", sqlValues(TENOR_SETTLTYPE), where);
    expectSame(sql, "account_type", sqlValues(AGENCY_ACCOUNTTYPE), where);
}
