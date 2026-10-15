#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "TypesDef.h"

namespace COP
{
namespace App
{

/// One identity that may open a WebSocket session, and what it may see and trade (#225)
struct WsIdentity
{
    /// Recorded on each order the identity sends, where every WebSocket order said "WebSocket" before
    std::string name;
    /// Its token, in WsCredentials::hashToken()'s form
    std::string tokenHash;
    /// The accounts it may see and trade, as the file names them. An order that names none trades the first.
    std::vector<std::string> accountNames;
    /// The same accounts' ids, sorted, and the first one's: set by WsCredentials::resolveAccounts()
    std::vector<SourceIdT> accounts;
    SourceIdT defaultAccount;

    /// Whether the identity may see and trade the account
    bool mayUse(const SourceIdT &account) const
    {
        return std::binary_search(accounts.begin(), accounts.end(), account);
    }
};

/// Who may open a WebSocket session, and what each may see and trade (#225). Before it, every holder of the one shared
/// token (#146) saw every account's orders and could trade, cancel and replace on any of them.
///
/// The credentials file has one line per identity:
///
///     # Identity   TokenHash                        Accounts
///     desk-a       sha256$<64 lowercase hex digits> TRADING-1,TRADING-2
///
/// Fields are separated by spaces or tabs. Blank lines, and lines whose first non-blank character is '#', are ignored.
/// An identity's name is letters, digits, '.', '_' and '-'.
///
/// A client offers only its token, so the server finds the identity by the token's hash, a plain SHA-256. That is
/// enough because a token is random and long (WS_MIN_TOKEN_LENGTH characters at least; `openssl rand -hex 32` makes
/// one). A salted, slow hash such as FIX's passwords have (#147) would have to be tried against every identity on every
/// connection attempt, on the server's only IO thread.
class WsCredentials final
{
public:
    /// Reads the file. Throws std::runtime_error, naming the file and the line, for a file it can't read or a line it
    /// can't use: not three fields, a name or hash given twice, a name or hash it doesn't take, an empty account, or an
    /// account given twice.
    static WsCredentials load(const std::string &path);
    /// Parses text as load() parses a file's; source names it in errors
    static WsCredentials parse(std::string_view text, const std::string &source);

    /// Resolves each identity's account names to ids with lookup, which gives an invalid id for a name the data
    /// directory lacks. Call it once, after the data directory's load and before the server takes connections. Throws
    /// std::runtime_error naming the identity and the account.
    void resolveAccounts(const std::function<SourceIdT(const std::string &)> &lookup);

    /// The identity whose token this is, or null
    std::shared_ptr<const WsIdentity> find(std::string_view token) const;
    std::size_t size() const
    {
        return identities_.size();
    }

    /// A token's stored form: "sha256$" and its SHA-256 in lowercase hex
    static std::string hashToken(std::string_view token);

private:
    std::vector<std::shared_ptr<WsIdentity>> identities_;
    std::unordered_map<std::string, std::shared_ptr<const WsIdentity>> byHash_;
};

} // namespace App
} // namespace COP
