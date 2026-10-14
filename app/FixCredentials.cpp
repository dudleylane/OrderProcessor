#ifdef BUILD_FIX

#include "FixCredentials.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace COP::App;

namespace
{
constexpr std::string_view SCHEME = "pbkdf2-sha256";
constexpr std::size_t SALT_BYTES = 16;
constexpr std::size_t KEY_BYTES = 32; // SHA-256's output
/// More would make each logon take seconds: a mistake, not a choice
constexpr unsigned MAX_ITERATIONS = 10000000;

std::string toHex(std::string_view bytes)
{
    static const char DIGITS[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(2 * bytes.size());
    for (const char byte : bytes)
    {
        const auto value = static_cast<unsigned char>(byte);
        hex.push_back(DIGITS[value >> 4]);
        hex.push_back(DIGITS[value & 0x0f]);
    }
    return hex;
}

int hexDigit(char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f'))
    {
        return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F'))
    {
        return c - 'A' + 10;
    }
    return -1;
}

bool fromHex(std::string_view hex, std::string *bytes)
{
    if (hex.empty() || (0 != (hex.size() % 2)))
    {
        return false;
    }
    bytes->clear();
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int high = hexDigit(hex[i]);
        const int low = hexDigit(hex[i + 1]);
        if ((high < 0) || (low < 0))
        {
            return false;
        }
        bytes->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

/// A stored hash's fields: "pbkdf2-sha256$<iterations>$<salt>$<key>"
struct StoredHash
{
    unsigned iterations = 0;
    std::string salt;
    std::string key;
};

bool parseHash(std::string_view text, StoredHash *hash)
{
    std::string_view parts[4];
    for (std::size_t i = 0; i < 4; ++i)
    {
        const std::size_t dollar = text.find('$');
        if ((3 == i) != (std::string_view::npos == dollar))
        {
            return false; // fewer or more than four fields
        }
        parts[i] = text.substr(0, dollar);
        text = (std::string_view::npos == dollar) ? std::string_view() : text.substr(dollar + 1);
    }
    if (SCHEME != parts[0])
    {
        return false;
    }
    const char *end = parts[1].data() + parts[1].size();
    const auto [rest, error] = std::from_chars(parts[1].data(), end, hash->iterations);
    if ((std::errc() != error) || (end != rest) || (0 == hash->iterations) || (hash->iterations > MAX_ITERATIONS))
    {
        return false;
    }
    return fromHex(parts[2], &hash->salt) && fromHex(parts[3], &hash->key) && (KEY_BYTES == hash->key.size());
}

std::string derive(std::string_view password, std::string_view salt, unsigned iterations)
{
    std::string key(KEY_BYTES, '\0');
    if (1 != PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                               reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
                               static_cast<int>(iterations), EVP_sha256(), static_cast<int>(key.size()),
                               reinterpret_cast<unsigned char *>(key.data())))
    {
        throw std::runtime_error("PBKDF2 failed");
    }
    return key;
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

FixCredentials FixCredentials::load(const std::string &path)
{
    std::ifstream file(path);
    std::stringstream text;
    if (!file || !(text << file.rdbuf()))
    {
        throw std::runtime_error("can't read the FIX credentials file " + path +
                                 " (the server must be able to read it: it holds only password hashes)");
    }
    return parse(text.str(), path);
}

FixCredentials FixCredentials::parse(std::string_view text, const std::string &source)
{
    FixCredentials credentials;
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
        if (4 != fields.size())
        {
            fail("a counterparty takes 4 fields, its CompID, Username, PasswordHash and Accounts, not " +
                 std::to_string(fields.size()));
        }
        FixCounterparty counterparty{ std::string(fields[0]), std::string(fields[1]), std::string(fields[2]), {} };
        if (nullptr != credentials.find(counterparty.compId))
        {
            fail("the CompID " + counterparty.compId + " is given twice");
        }
        StoredHash hash;
        if (!parseHash(counterparty.passwordHash, &hash))
        {
            fail("the password hash isn't " + std::string(SCHEME) +
                 "$<iterations>$<salt>$<key>; orderProcessorServer --hash-fix-password makes one");
        }
        std::string_view accounts = fields[3];
        while (true)
        {
            const std::size_t comma = accounts.find(',');
            const std::string_view account = accounts.substr(0, comma);
            if (account.empty())
            {
                fail("an account in \"" + std::string(fields[3]) + "\" is empty");
            }
            if (mayTrade(counterparty, account))
            {
                fail("the account " + std::string(account) + " is given twice");
            }
            counterparty.accounts.emplace_back(account);
            if (std::string_view::npos == comma)
            {
                break;
            }
            accounts.remove_prefix(comma + 1);
        }
        credentials.counterparties_.push_back(std::move(counterparty));
    }
    return credentials;
}

const FixCounterparty *FixCredentials::find(std::string_view compId) const
{
    const auto it = std::find_if(counterparties_.begin(), counterparties_.end(),
                                 [compId](const FixCounterparty &counterparty)
                                 {
                                     return counterparty.compId == compId;
                                 });
    return (counterparties_.end() == it) ? nullptr : &*it;
}

bool FixCredentials::verify(const FixCounterparty &counterparty, std::string_view username, std::string_view password)
{
    StoredHash hash;
    if (!parseHash(counterparty.passwordHash, &hash))
    {
        return false;
    }
    // Derived whatever the username, so a wrong one takes as long as a wrong password
    const std::string key = derive(password, hash.salt, hash.iterations);
    const bool keyMatches = (0 == CRYPTO_memcmp(key.data(), hash.key.data(), KEY_BYTES));
    return keyMatches && (counterparty.username == username);
}

bool FixCredentials::mayTrade(const FixCounterparty &counterparty, std::string_view account)
{
    return counterparty.accounts.end() !=
           std::find(counterparty.accounts.begin(), counterparty.accounts.end(), account);
}

std::string FixCredentials::hashPassword(std::string_view password, unsigned iterations)
{
    std::string salt(SALT_BYTES, '\0');
    if (1 != RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), static_cast<int>(salt.size())))
    {
        throw std::runtime_error("no random bytes for a salt");
    }
    return hashPassword(password, iterations, salt);
}

std::string FixCredentials::hashPassword(std::string_view password, unsigned iterations, std::string_view salt)
{
    if ((0 == iterations) || (iterations > MAX_ITERATIONS) || salt.empty())
    {
        throw std::invalid_argument("hashPassword needs 1 to " + std::to_string(MAX_ITERATIONS) +
                                    " iterations and a salt");
    }
    return std::string(SCHEME) + "$" + std::to_string(iterations) + "$" + toHex(salt) + "$" +
           toHex(derive(password, salt, iterations));
}

unsigned FixCredentials::iterationsOf(std::string_view passwordHash)
{
    StoredHash hash;
    return parseHash(passwordHash, &hash) ? hash.iterations : 0;
}

#endif // BUILD_FIX
