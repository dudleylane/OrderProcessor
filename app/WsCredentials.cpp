#include "WsCredentials.h"

#include <openssl/evp.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace COP;
using namespace COP::App;

namespace
{
constexpr std::string_view SCHEME = "sha256$";
constexpr std::size_t DIGEST_HEX_DIGITS = 64; // SHA-256's 32 bytes
/// Longer names are a mistake: each is stored with every order the identity sends
constexpr std::size_t MAX_NAME_LENGTH = 64;

bool isNameCharacter(char c)
{
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '.') ||
           (c == '_') || (c == '-');
}

bool isValidName(std::string_view name)
{
    return !name.empty() && (name.size() <= MAX_NAME_LENGTH) && std::all_of(name.begin(), name.end(), isNameCharacter);
}

bool isValidHash(std::string_view hash)
{
    if ((hash.size() != SCHEME.size() + DIGEST_HEX_DIGITS) || (0 != hash.compare(0, SCHEME.size(), SCHEME)))
    {
        return false;
    }
    return std::all_of(hash.begin() + SCHEME.size(), hash.end(),
                       [](char c)
                       {
                           return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f'));
                       });
}

/// The line's fields, split on spaces and tabs
std::vector<std::string_view> fieldsOf(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t at = 0;
    while (true)
    {
        at = line.find_first_not_of(" \t", at);
        if (std::string_view::npos == at)
        {
            return fields;
        }
        const std::size_t end = line.find_first_of(" \t", at);
        fields.push_back(line.substr(at, end - at));
        if (std::string_view::npos == end)
        {
            return fields;
        }
        at = end;
    }
}
} // namespace

WsCredentials WsCredentials::load(const std::string &path)
{
    std::ifstream file(path);
    std::stringstream text;
    if (!file || !(text << file.rdbuf()))
    {
        throw std::runtime_error("can't read the WebSocket credentials file " + path +
                                 " (the server must be able to read it: it holds only token hashes)");
    }
    return parse(text.str(), path);
}

WsCredentials WsCredentials::parse(std::string_view text, const std::string &source)
{
    WsCredentials credentials;
    std::size_t lineNumber = 0;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = (std::string_view::npos == newline) ? std::string_view() : text.substr(newline + 1);
        ++lineNumber;
        if (!line.empty() && ('\r' == line.back()))
        {
            line.remove_suffix(1);
        }
        const std::vector<std::string_view> fields = fieldsOf(line);
        if (fields.empty() || ('#' == fields.front().front()))
        {
            continue;
        }
        const auto fail = [&](const std::string &what)
        {
            throw std::runtime_error(source + ":" + std::to_string(lineNumber) + ": " + what);
        };
        if (3 != fields.size())
        {
            fail("an identity takes 3 fields, its name, TokenHash and Accounts, not " + std::to_string(fields.size()));
        }
        auto identity = std::make_shared<WsIdentity>();
        identity->name = std::string(fields[0]);
        identity->tokenHash = std::string(fields[1]);
        if (!isValidName(identity->name))
        {
            fail("the name " + identity->name + " isn't 1 to " + std::to_string(MAX_NAME_LENGTH) +
                 " letters, digits, '.', '_' or '-'");
        }
        for (const std::shared_ptr<WsIdentity> &earlier : credentials.identities_)
        {
            if (earlier->name == identity->name)
            {
                fail("the name " + identity->name + " is given twice");
            }
        }
        if (!isValidHash(identity->tokenHash))
        {
            fail("the token hash isn't sha256$ and 64 lowercase hex digits; orderProcessorServer --hash-ws-token makes "
                 "one");
        }
        if (credentials.byHash_.count(identity->tokenHash))
        {
            fail("the token hash of " + identity->name + " is another identity's too: one token, one identity");
        }
        std::string_view accounts = fields[2];
        while (true)
        {
            const std::size_t comma = accounts.find(',');
            const std::string_view account = accounts.substr(0, comma);
            if (account.empty())
            {
                fail("an account in \"" + std::string(fields[2]) + "\" is empty");
            }
            if (identity->accountNames.end() !=
                std::find(identity->accountNames.begin(), identity->accountNames.end(), account))
            {
                fail("the account " + std::string(account) + " is given twice");
            }
            identity->accountNames.emplace_back(account);
            if (std::string_view::npos == comma)
            {
                break;
            }
            accounts.remove_prefix(comma + 1);
        }
        credentials.byHash_.emplace(identity->tokenHash, identity);
        credentials.identities_.push_back(std::move(identity));
    }
    return credentials;
}

void WsCredentials::resolveAccounts(const std::function<SourceIdT(const std::string &)> &lookup)
{
    for (const std::shared_ptr<WsIdentity> &identity : identities_)
    {
        identity->accounts.clear();
        for (const std::string &name : identity->accountNames)
        {
            const SourceIdT id = lookup(name);
            if (!id.isValid())
            {
                throw std::runtime_error("the WebSocket identity " + identity->name + " names the account " + name +
                                         ", which the data directory doesn't have");
            }
            identity->accounts.push_back(id);
        }
        identity->defaultAccount = identity->accounts.front();
        std::sort(identity->accounts.begin(), identity->accounts.end());
    }
}

std::shared_ptr<const WsIdentity> WsCredentials::find(std::string_view token) const
{
    const auto it = byHash_.find(hashToken(token));
    return (byHash_.end() == it) ? nullptr : it->second;
}

std::string WsCredentials::hashToken(std::string_view token)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (1 != EVP_Digest(token.data(), token.size(), digest, &length, EVP_sha256(), nullptr))
    {
        throw std::runtime_error("SHA-256 failed");
    }
    static const char DIGITS[] = "0123456789abcdef";
    std::string hash(SCHEME);
    hash.reserve(SCHEME.size() + 2 * length);
    for (unsigned int i = 0; i < length; ++i)
    {
        hash.push_back(DIGITS[digest[i] >> 4]);
        hash.push_back(DIGITS[digest[i] & 0x0f]);
    }
    return hash;
}
