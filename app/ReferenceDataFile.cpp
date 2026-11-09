#include "ReferenceDataFile.h"

#include "EnumStrings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>

using namespace COP;
using namespace COP::App;
using json = nlohmann::json;

namespace
{

const char *const INSTRUMENTS = "instruments";
const char *const ACCOUNTS = "accounts";
const char *const CLEARING_FIRMS = "clearingFirms";

/// The field each kind's entries are named by, which a file may give once
const StringT &nameOf(const InstrumentEntry &entry)
{
    return entry.symbol_;
}

const StringT &nameOf(const AccountEntry &entry)
{
    return entry.account_;
}

const StringT &nameOf(const ClearingEntry &entry)
{
    return entry.firm_;
}

/// A field every entry of its kind gives, as a JSON string. Whether it may be empty is the entry's isValid()'s to say.
bool readString(const json &item, const char *key, StringT *out, std::string *why)
{
    const json::const_iterator found = item.find(key);
    if (item.end() == found)
    {
        *why = std::string("no \"") + key + "\"";
        return false;
    }
    if (!found->is_string())
    {
        *why = std::string("\"") + key + "\" must be a string";
        return false;
    }
    *out = found->get<std::string>();
    return true;
}

/// A field an entry may leave out, which then keeps the value it has: a JSON string fromJson() knows, and values names
/// those it knows. fromJson() gives one it doesn't know as 0, each enum's INVALID.
template <typename Enum>
bool readOptionalEnum(const json &item, const char *key, Enum (*fromJson)(std::string_view), const char *values,
                      Enum *value, std::string *why)
{
    if (!item.contains(key))
    {
        return true;
    }
    StringT given;
    if (!readString(item, key, &given, why))
    {
        return false;
    }
    *value = fromJson(given);
    if (Enum() == *value)
    {
        *why = std::string("\"") + key + "\" must be " + values;
        return false;
    }
    return true;
}

const char *const CURRENCIES = "USD, EUR, GBP, JPY, CHF, AUD, CAD or NZD";

/// On this branch an instrument may also give its type, and an FX pair its currencies. One that gives no type has none,
/// as seedData's test set's equities have.
bool readInstrument(const json &item, InstrumentEntry *entry, std::string *why)
{
    if (!readString(item, "symbol", &entry->symbol_, why) ||
        !readString(item, "securityId", &entry->securityId_, why) ||
        !readString(item, "securityIdSource", &entry->securityIdSource_, why) ||
        !readOptionalEnum(item, "instrumentType", instrumentTypeFromJson, "EQUITY, FX_SPOT or FX_SWAP",
                          &entry->instrumentType_, why) ||
        !readOptionalEnum(item, "baseCurrency", currencyFromJson, CURRENCIES, &entry->baseCurrency_, why) ||
        !readOptionalEnum(item, "termCurrency", currencyFromJson, CURRENCIES, &entry->termCurrency_, why))
    {
        return false;
    }
    // isValid() checks an FX pair's currencies, and nothing would check them on any other instrument
    if (!entry->isFxPair() &&
        ((INVALID_CURRENCY != entry->baseCurrency_) || (INVALID_CURRENCY != entry->termCurrency_)))
    {
        *why =
            "\"baseCurrency\" and \"termCurrency\" are for an FX pair, whose \"instrumentType\" is FX_SPOT or FX_SWAP";
        return false;
    }
    return true;
}

bool readAccount(const json &item, AccountEntry *entry, std::string *why)
{
    StringT type;
    if (!readString(item, "account", &entry->account_, why) || !readString(item, "firm", &entry->firm_, why) ||
        !readString(item, "type", &type, why))
    {
        return false;
    }
    entry->type_ = accountTypeFromJson(type);
    if (INVALID_ACCOUNTTYPE == entry->type_)
    {
        *why = "\"type\" must be PRINCIPAL or AGENCY";
        return false;
    }
    return true;
}

bool readClearingFirm(const json &item, ClearingEntry *entry, std::string *why)
{
    return readString(item, "firm", &entry->firm_, why);
}

/// The checks the entry's isValid() makes, all but the id's: the store gives it one as it's added
template <typename Entry> bool passesItsChecks(Entry entry, std::string *why)
{
    entry.id_ = IdT(1, 1);
    return entry.isValid(why);
}

/// Reads one section's entries, which read() fills in from their objects. keys are the keys an entry may give, and
/// nameKey the one it's named by, which a message about the entry gives when it can.
template <typename Entry, typename Read>
bool readSection(const json &file, const char *section, std::initializer_list<const char *> keys, const char *nameKey,
                 Read read, std::vector<Entry> *out, std::string *error)
{
    const json::const_iterator found = file.find(section);
    if (file.end() == found)
    {
        return true;
    }
    std::map<StringT, std::size_t> named;
    for (std::size_t index = 0; index < found->size(); ++index)
    {
        const json &item = (*found)[index];
        std::string at = std::string(section) + "[" + std::to_string(index) + "]";
        std::string why;
        Entry entry;
        if (!item.is_object())
        {
            why = "must be an object";
        }
        else
        {
            const json::const_iterator name = item.find(nameKey);
            if ((item.end() != name) && name->is_string() && !name->get_ref<const std::string &>().empty())
            {
                at += " (" + name->get<std::string>() + ")";
            }
            for (json::const_iterator key = item.begin(); key != item.end(); ++key)
            {
                if (std::none_of(keys.begin(), keys.end(),
                                 [&key](const char *known)
                                 {
                                     return key.key() == known;
                                 }))
                {
                    why = "unknown key \"" + key.key() + "\"";
                    break;
                }
            }
            if (why.empty() && read(item, &entry, &why) && passesItsChecks(entry, &why))
            {
                const auto given = named.emplace(nameOf(entry), index);
                if (!given.second)
                {
                    why = std::string(section) + "[" + std::to_string(given.first->second) + "] gives the same " +
                          nameKey;
                }
            }
        }
        if (!why.empty())
        {
            *error = at + ": " + why;
            return false;
        }
        out->push_back(entry);
    }
    return true;
}

} // namespace

std::optional<ReferenceData> App::parseReferenceData(const std::string &text, std::string *error)
{
    try
    {
        const json file = json::parse(text);
        if (!file.is_object())
        {
            *error = "the file must hold a JSON object";
            return std::nullopt;
        }
        for (json::const_iterator section = file.begin(); section != file.end(); ++section)
        {
            if ((INSTRUMENTS != section.key()) && (ACCOUNTS != section.key()) && (CLEARING_FIRMS != section.key()))
            {
                *error = "unknown key \"" + section.key() + "\": a file gives instruments, accounts and clearingFirms";
                return std::nullopt;
            }
            if (!section->is_array())
            {
                *error = "\"" + section.key() + "\" must be an array";
                return std::nullopt;
            }
        }
        ReferenceData data;
        if (!readSection(
                file, INSTRUMENTS,
                { "symbol", "securityId", "securityIdSource", "instrumentType", "baseCurrency", "termCurrency" },
                "symbol", readInstrument, &data.instruments, error) ||
            !readSection(file, ACCOUNTS, { "account", "firm", "type" }, "account", readAccount, &data.accounts,
                         error) ||
            !readSection(file, CLEARING_FIRMS, { "firm" }, "firm", readClearingFirm, &data.clearingFirms, error))
        {
            return std::nullopt;
        }
        return data;
    }
    catch (const json::exception &ex)
    {
        *error = ex.what();
    }
    return std::nullopt;
}

std::optional<ReferenceData> App::readReferenceDataFile(const std::string &path, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        *error = path + ": can't open it";
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
    {
        *error = path + ": can't read it";
        return std::nullopt;
    }
    std::optional<ReferenceData> data = parseReferenceData(text, error);
    if (!data)
    {
        *error = path + ": " + *error;
    }
    return data;
}

bool App::sameFields(const InstrumentEntry &lft, const InstrumentEntry &rght)
{
    return (lft.symbol_ == rght.symbol_) && (lft.securityId_ == rght.securityId_) &&
           (lft.securityIdSource_ == rght.securityIdSource_) && (lft.instrumentType_ == rght.instrumentType_) &&
           (lft.baseCurrency_ == rght.baseCurrency_) && (lft.termCurrency_ == rght.termCurrency_);
}

bool App::sameFields(const AccountEntry &lft, const AccountEntry &rght)
{
    return (lft.account_ == rght.account_) && (lft.firm_ == rght.firm_) && (lft.type_ == rght.type_);
}

std::string App::describe(const InstrumentEntry &entry)
{
    // seedData's test set gives its equities no type, and nothing gave an instrument one before #585
    std::string text =
        "securityId " + entry.securityId_ + ", securityIdSource " + entry.securityIdSource_ + ", type " +
        std::string(INVALID_INSTRUMENTTYPE == entry.instrumentType_ ? std::string_view("none")
                                                                    : toJsonString(entry.instrumentType_));
    if ((INVALID_CURRENCY != entry.baseCurrency_) || (INVALID_CURRENCY != entry.termCurrency_))
    {
        text += ", currencies " + std::string(toJsonString(entry.baseCurrency_)) + "/" +
                std::string(toJsonString(entry.termCurrency_));
    }
    return text;
}

std::string App::describe(const AccountEntry &entry)
{
    return "firm " + entry.firm_ + ", type " + std::string(toJsonString(entry.type_));
}
