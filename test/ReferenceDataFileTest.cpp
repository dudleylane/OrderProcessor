/**
 Concurrent Order Processor library - seedData's reference data file Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

#include "ReferenceDataFile.h"
#include "TestAux.h"

using namespace COP;
using COP::App::parseReferenceData;
using COP::App::ReferenceData;

namespace
{

/// Why parseReferenceData() refuses text, or "" when it reads it
std::string refusal(const std::string &text)
{
    std::string error;
    const std::optional<ReferenceData> data = parseReferenceData(text, &error);
    return data ? std::string() : error;
}

const char *const EXAMPLE = R"({
  "instruments": [
    { "symbol": "AAPL", "securityId": "US0378331005", "securityIdSource": "ISIN" },
    { "symbol": "MSFT", "securityId": "US5949181045", "securityIdSource": "ISIN" }
  ],
  "accounts": [
    { "account": "TRADING-1", "firm": "Apex Capital", "type": "PRINCIPAL" },
    { "account": "CLIENT-A", "firm": "Summit Partners", "type": "AGENCY" }
  ],
  "clearingFirms": [
    { "firm": "HOUSE-CLEARING" }
  ]
})";

TEST(ReferenceDataFileTest, ReadsEveryField)
{
    std::string error;
    const std::optional<ReferenceData> data = parseReferenceData(EXAMPLE, &error);
    ASSERT_TRUE(data) << error;
    ASSERT_EQ(2u, data->instruments.size());
    EXPECT_EQ("AAPL", data->instruments[0].symbol_);
    EXPECT_EQ("US0378331005", data->instruments[0].securityId_);
    EXPECT_EQ("ISIN", data->instruments[0].securityIdSource_);
    EXPECT_EQ("MSFT", data->instruments[1].symbol_);
    ASSERT_EQ(2u, data->accounts.size());
    EXPECT_EQ("TRADING-1", data->accounts[0].account_);
    EXPECT_EQ("Apex Capital", data->accounts[0].firm_);
    EXPECT_EQ(PRINCIPAL_ACCOUNTTYPE, data->accounts[0].type_);
    EXPECT_EQ("CLIENT-A", data->accounts[1].account_);
    EXPECT_EQ(AGENCY_ACCOUNTTYPE, data->accounts[1].type_);
    ASSERT_EQ(1u, data->clearingFirms.size());
    EXPECT_EQ("HOUSE-CLEARING", data->clearingFirms[0].firm_);
    // The store gives each its id as it's added
    EXPECT_FALSE(data->instruments[0].id_.isValid());
    EXPECT_FALSE(data->accounts[0].id_.isValid());
    EXPECT_FALSE(data->clearingFirms[0].id_.isValid());
}

TEST(ReferenceDataFileTest, AnySectionMayBeLeftOut)
{
    std::string error;
    std::optional<ReferenceData> data = parseReferenceData("{}", &error);
    ASSERT_TRUE(data) << error;
    EXPECT_TRUE(data->instruments.empty());
    EXPECT_TRUE(data->accounts.empty());
    EXPECT_TRUE(data->clearingFirms.empty());

    data = parseReferenceData(R"({ "clearingFirms": [ { "firm": "HOUSE-CLEARING" } ] })", &error);
    ASSERT_TRUE(data) << error;
    EXPECT_TRUE(data->instruments.empty());
    EXPECT_TRUE(data->accounts.empty());
    EXPECT_EQ(1u, data->clearingFirms.size());
}

TEST(ReferenceDataFileTest, RefusesWhatIsntAJsonObject)
{
    EXPECT_NE(std::string::npos, refusal("instruments: AAPL").find("parse error"));
    EXPECT_NE(std::string::npos, refusal("").find("parse error"));
    EXPECT_EQ("the file must hold a JSON object", refusal(R"([ { "symbol": "AAPL" } ])"));
}

TEST(ReferenceDataFileTest, RefusesASectionItDoesntKnow)
{
    // A misspelt section would load nothing of what it holds, without a word
    EXPECT_EQ("unknown key \"instrument\": a file gives instruments, accounts and clearingFirms",
              refusal(R"({ "instrument": [] })"));
    EXPECT_EQ("\"accounts\" must be an array", refusal(R"({ "accounts": { "account": "TRADING-1" } })"));
}

TEST(ReferenceDataFileTest, RefusesAnEntryThatIsntAnObject)
{
    EXPECT_EQ("instruments[0]: must be an object", refusal(R"({ "instruments": [ "AAPL" ] })"));
}

TEST(ReferenceDataFileTest, RefusesAKeyItDoesntKnow)
{
    // A misspelt key would drop its field without a word
    EXPECT_EQ("instruments[1] (MSFT): unknown key \"securityID\"", refusal(R"({ "instruments": [
                  { "symbol": "AAPL", "securityId": "US0378331005", "securityIdSource": "ISIN" },
                  { "symbol": "MSFT", "securityID": "US5949181045", "securityIdSource": "ISIN" } ] })"));
    EXPECT_EQ("accounts[0] (TRADING-1): unknown key \"id\"",
              refusal(R"({ "accounts": [ { "account": "TRADING-1", "firm": "Apex Capital", "type": "PRINCIPAL",
                                           "id": 1 } ] })"));
    EXPECT_EQ("clearingFirms[0] (HOUSE-CLEARING): unknown key \"name\"",
              refusal(R"({ "clearingFirms": [ { "firm": "HOUSE-CLEARING", "name": "House" } ] })"));
}

TEST(ReferenceDataFileTest, RefusesAMissingField)
{
    EXPECT_EQ("instruments[0] (AAPL): no \"securityIdSource\"",
              refusal(R"({ "instruments": [ { "symbol": "AAPL", "securityId": "US0378331005" } ] })"));
    EXPECT_EQ("accounts[0] (TRADING-1): no \"type\"",
              refusal(R"({ "accounts": [ { "account": "TRADING-1", "firm": "Apex Capital" } ] })"));
    EXPECT_EQ("clearingFirms[0]: no \"firm\"", refusal(R"({ "clearingFirms": [ {} ] })"));
}

TEST(ReferenceDataFileTest, RefusesAFieldThatIsntAString)
{
    EXPECT_EQ(
        "instruments[0] (AAPL): \"securityId\" must be a string",
        refusal(R"({ "instruments": [ { "symbol": "AAPL", "securityId": 378331005, "securityIdSource": "ISIN" } ] })"));
    // An entry whose name isn't a string is named by its place alone
    EXPECT_EQ("clearingFirms[0]: \"firm\" must be a string", refusal(R"({ "clearingFirms": [ { "firm": 7 } ] })"));
}

TEST(ReferenceDataFileTest, RefusesWhatTheEntrysOwnChecksRefuse)
{
    // Each entry's isValid(), all but the id's: here an empty field
    EXPECT_EQ("instruments[0] (AAPL): Invalid value of the instrument's securityId!",
              refusal(R"({ "instruments": [ { "symbol": "AAPL", "securityId": "", "securityIdSource": "ISIN" } ] })"));
    EXPECT_EQ(
        "instruments[0]: Invalid value of the instrument's symbol!",
        refusal(
            R"({ "instruments": [ { "symbol": "", "securityId": "US0378331005", "securityIdSource": "ISIN" } ] })"));
    EXPECT_EQ("accounts[0] (TRADING-1): Invalid value of the account's firm!",
              refusal(R"({ "accounts": [ { "account": "TRADING-1", "firm": "", "type": "PRINCIPAL" } ] })"));
    EXPECT_EQ("clearingFirms[0]: Invalid value of the clearing firm!",
              refusal(R"({ "clearingFirms": [ { "firm": "" } ] })"));
}

TEST(ReferenceDataFileTest, RefusesAnAccountTypeItDoesntKnow)
{
    for (const char *type : { "principal", "INVALID", "PROPRIETARY", "" })
    {
        EXPECT_EQ(
            "accounts[0] (TRADING-1): \"type\" must be PRINCIPAL or AGENCY",
            refusal(std::string(R"({ "accounts": [ { "account": "TRADING-1", "firm": "Apex Capital", "type": ")") +
                    type + "\" } ] }"))
            << type;
    }
}

TEST(ReferenceDataFileTest, RefusesANameGivenTwice)
{
    EXPECT_EQ("instruments[2] (AAPL): instruments[0] gives the same symbol", refusal(R"({ "instruments": [
                  { "symbol": "AAPL", "securityId": "US0378331005", "securityIdSource": "ISIN" },
                  { "symbol": "MSFT", "securityId": "US5949181045", "securityIdSource": "ISIN" },
                  { "symbol": "AAPL", "securityId": "US0378331005", "securityIdSource": "ISIN" } ] })"));
    EXPECT_EQ("accounts[1] (TRADING-1): accounts[0] gives the same account", refusal(R"({ "accounts": [
                  { "account": "TRADING-1", "firm": "Apex Capital", "type": "PRINCIPAL" },
                  { "account": "TRADING-1", "firm": "Summit Partners", "type": "AGENCY" } ] })"));
    EXPECT_EQ("clearingFirms[1] (HOUSE-CLEARING): clearingFirms[0] gives the same firm",
              refusal(R"({ "clearingFirms": [ { "firm": "HOUSE-CLEARING" }, { "firm": "HOUSE-CLEARING" } ] })"));
}

TEST(ReferenceDataFileTest, ReadsAFileAndNamesOneItRefuses)
{
    const std::string path = test::uniqueTestPath("reference.json");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << EXAMPLE;
    }
    std::string error;
    const std::optional<ReferenceData> data = COP::App::readReferenceDataFile(path, &error);
    ASSERT_TRUE(data) << error;
    EXPECT_EQ(2u, data->instruments.size());
    EXPECT_EQ(2u, data->accounts.size());
    EXPECT_EQ(1u, data->clearingFirms.size());

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({ "instrument": [] })";
    }
    EXPECT_FALSE(COP::App::readReferenceDataFile(path, &error));
    EXPECT_EQ(path + ": unknown key \"instrument\": a file gives instruments, accounts and clearingFirms", error);

    std::remove(path.c_str());
    EXPECT_FALSE(COP::App::readReferenceDataFile(path, &error));
    EXPECT_EQ(path + ": can't open it", error);
}

TEST(ReferenceDataFileTest, SameFieldsComparesAllButTheId)
{
    InstrumentEntry stored;
    stored.symbol_ = "AAPL";
    stored.securityId_ = "US0378331005";
    stored.securityIdSource_ = "ISIN";
    stored.id_ = IdT(5, 1);
    InstrumentEntry given = stored;
    given.id_ = IdT();
    EXPECT_TRUE(COP::App::sameFields(stored, given));
    given.securityId_ = "US5949181045";
    EXPECT_FALSE(COP::App::sameFields(stored, given));
    given = stored;
    given.securityIdSource_ = "CUSIP";
    EXPECT_FALSE(COP::App::sameFields(stored, given));
    EXPECT_EQ("securityId US0378331005, securityIdSource ISIN", COP::App::describe(stored));

    AccountEntry account;
    account.account_ = "TRADING-1";
    account.firm_ = "Apex Capital";
    account.type_ = PRINCIPAL_ACCOUNTTYPE;
    account.id_ = IdT(6, 1);
    AccountEntry other = account;
    other.id_ = IdT();
    EXPECT_TRUE(COP::App::sameFields(account, other));
    other.firm_ = "Summit Partners";
    EXPECT_FALSE(COP::App::sameFields(account, other));
    other = account;
    other.type_ = AGENCY_ACCOUNTTYPE;
    EXPECT_FALSE(COP::App::sameFields(account, other));
    EXPECT_EQ("firm Apex Capital, type PRINCIPAL", COP::App::describe(account));
}

} // namespace
