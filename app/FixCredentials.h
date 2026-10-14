#pragma once

#ifdef BUILD_FIX

#include <string>
#include <string_view>
#include <vector>

namespace COP::App
{

/// One FIX counterparty that may log on, and the accounts it may trade (#147)
struct FixCounterparty
{
    /// Its CompID: the TargetCompID of the server's session with it, and the SenderCompID of its messages
    std::string compId;
    /// The Username (553) its Logon must carry
    std::string username;
    /// The Password (554) its Logon must carry, in FixCredentials::hashPassword()'s form
    std::string passwordHash;
    /// The accounts its orders may name. An order that names none trades the first.
    std::vector<std::string> accounts;
};

/// Who may log on to the FIX acceptor, and what each may trade (#147). Before #147 a Logon was checked for nothing, so
/// anyone who reached port 5001 could log on as the CompID config/fix.cfg named, and trade any account.
///
/// The credentials file has one line per counterparty, and each makes a session:
///
///     # CompID    Username   PasswordHash                          Accounts
///     CLIENT_A    client_a   pbkdf2-sha256$600000$<salt>$<key>     TRADING-1,TRADING-2
///
/// Fields are separated by spaces or tabs. Blank lines, and lines whose first non-blank character is '#', are ignored.
/// The file holds only password hashes, so it may be readable by others: the server runs as uid 10001 in its image, and
/// a rootless container can't read a host file that only its owner may read.
class FixCredentials final
{
public:
    /// Reads the file. Throws std::runtime_error, naming the file and the line, for a file it can't read or a line it
    /// can't use: too few or too many fields, a CompID given twice, a hash it doesn't recognise, an empty account, or
    /// an account given twice.
    static FixCredentials load(const std::string &path);
    /// Parses text as load() parses a file's; source names it in errors
    static FixCredentials parse(std::string_view text, const std::string &source);

    const std::vector<FixCounterparty> &counterparties() const
    {
        return counterparties_;
    }
    /// The counterparty with this CompID, or nullptr
    const FixCounterparty *find(std::string_view compId) const;

    /// Whether a Logon's Username and Password are the counterparty's. The derived keys are compared in time that
    /// depends only on their length.
    static bool verify(const FixCounterparty &counterparty, std::string_view username, std::string_view password);
    /// Whether a counterparty may trade the account
    static bool mayTrade(const FixCounterparty &counterparty, std::string_view account);

    /// A password's stored form: "pbkdf2-sha256$<iterations>$<salt>$<key>", salt and key in hex, with PBKDF2-HMAC-SHA256
    /// over a random 16-byte salt and a 32-byte key
    static std::string hashPassword(std::string_view password, unsigned iterations = DEFAULT_ITERATIONS);
    /// The same with a given salt, which tests use for a known answer
    static std::string hashPassword(std::string_view password, unsigned iterations, std::string_view salt);
    /// The iteration count a stored hash names, or 0 when it isn't one hashPassword() makes
    static unsigned iterationsOf(std::string_view passwordHash);

    /// OWASP's 2023 count for PBKDF2-HMAC-SHA256. A logon costs about 0.6 s of its session's thread on the laptop that
    /// runs CI, and QuickFIX checks one logon per session at a time.
    static constexpr unsigned DEFAULT_ITERATIONS = 600000;
    /// Fewer is weak against a stolen file: load() accepts it, and the server warns. Tests use fewer, to stay fast.
    static constexpr unsigned MIN_RECOMMENDED_ITERATIONS = 100000;

private:
    std::vector<FixCounterparty> counterparties_;
};

} // namespace COP::App

#endif // BUILD_FIX
