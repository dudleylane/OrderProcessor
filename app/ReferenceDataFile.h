#pragma once

#include <optional>
#include <string>
#include <vector>

#include "DataModelDef.h"

namespace COP
{
namespace App
{

/// The reference data `seedData --file` loads (#585), each entry without the id the store gives it as it's added
struct ReferenceData
{
    std::vector<InstrumentEntry> instruments;
    std::vector<AccountEntry> accounts;
    std::vector<ClearingEntry> clearingFirms;
};

/// Reads reference data from a JSON object that gives any of "instruments", "accounts" and "clearingFirms", each an array
/// of entries named as instrument_list and account_list name their fields:
///
///     { "instruments": [ { "symbol": "AAPL", "securityId": "US0378331005", "securityIdSource": "ISIN" } ],
///       "accounts": [ { "account": "TRADING-1", "firm": "Apex Capital", "type": "PRINCIPAL" } ],
///       "clearingFirms": [ { "firm": "HOUSE-CLEARING" } ] }
///
/// Every entry is checked before anything is returned: each field is a string, each entry passes the checks its
/// isValid() makes, all but the id's, each symbol, account and clearing firm is given once, and a key it doesn't know is
/// refused, since a misspelt one would drop its field without a word. Anything wrong returns nullopt with the reason in
/// *error, naming the entry.
std::optional<ReferenceData> parseReferenceData(const std::string &text, std::string *error);

/// Reads the file at path with parseReferenceData(). A file it can't read is refused too, and *error names the file.
std::optional<ReferenceData> readReferenceDataFile(const std::string &path, std::string *error);

/// Whether a stored entry has the fields the file gives it: all but the id. A clearing firm has only its name.
bool sameFields(const InstrumentEntry &lft, const InstrumentEntry &rght);
bool sameFields(const AccountEntry &lft, const AccountEntry &rght);

/// An entry's fields other than its name, as a message gives them
std::string describe(const InstrumentEntry &entry);
std::string describe(const AccountEntry &entry);

} // namespace App
} // namespace COP
