#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "WsCredentials.h"

namespace COP
{
namespace App
{

/// Who may open a WebSocket session (#146), and what it may see and trade (#225). Left empty, anyone who reaches the
/// port may, as before #146.
struct WsAuth
{
    /// The shared token a client must offer, as the subprotocol WS_TOKEN_PREFIX + token beside WS_SUBPROTOCOL. Empty:
    /// no token is asked for, unless credentials are given.
    std::string token;
    /// Identities, each with a token of its own and the accounts it may see and trade (#225). When set, a client
    /// offers its identity's token where it would offer the shared one, and token is empty: one or the other.
    std::shared_ptr<WsCredentials> credentials;
    /// The Origins a browser may connect from. Empty: any. A client that sends no Origin isn't a browser, and the token
    /// authenticates it.
    std::vector<std::string> allowedOrigins;
};

/// The subprotocol the frontend offers, and the one the server answers with
inline constexpr std::string_view WS_SUBPROTOCOL = "oms-v1";
/// A client carries the token as a second subprotocol: this prefix, then the token. A browser can't set a WebSocket's
/// headers, but it can name its subprotocols.
inline constexpr std::string_view WS_TOKEN_PREFIX = "oms-token.";
/// The shortest token the server takes: a short one could be guessed
inline constexpr std::size_t WS_MIN_TOKEN_LENGTH = 16;

/// A token travels as part of a subprotocol name, which only some characters may make up (RFC 6455): letters, digits,
/// '.', '_' and '-' here, and at least WS_MIN_TOKEN_LENGTH of them. `openssl rand -hex 32` makes one.
inline bool isValidWsToken(std::string_view token)
{
    if (token.size() < WS_MIN_TOKEN_LENGTH)
    {
        return false;
    }
    return std::all_of(token.begin(), token.end(),
                       [](char c)
                       {
                           return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                                  ((c >= '0') && (c <= '9')) || (c == '.') || (c == '_') || (c == '-');
                       });
}

/// What the server does with an upgrade request
enum class WsUpgrade
{
    Accept,
    /// A token is asked for, and the request offers none: 401
    MissingToken,
    /// The request offers a token, and it isn't the server's: 401
    WrongToken,
    /// The request comes from a browser page whose Origin isn't allowed: 403
    ForbiddenOrigin,
};

namespace detail
{
/// Compares in time that depends only on the lengths, so the comparison doesn't tell a client how much of a guess was
/// right
inline bool sameToken(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return 0 == diff;
}

/// Calls f for each entry of a comma-separated header value, without the spaces around it
template <typename F> void forEachEntry(std::string_view list, F f)
{
    while (!list.empty())
    {
        const std::size_t comma = list.find(',');
        std::string_view entry = list.substr(0, comma);
        list = (std::string_view::npos == comma) ? std::string_view() : list.substr(comma + 1);
        while (!entry.empty() && ((entry.front() == ' ') || (entry.front() == '\t')))
        {
            entry.remove_prefix(1);
        }
        while (!entry.empty() && ((entry.back() == ' ') || (entry.back() == '\t')))
        {
            entry.remove_suffix(1);
        }
        if (!entry.empty())
        {
            f(entry);
        }
    }
}
} // namespace detail

/// Whether the client offered WS_SUBPROTOCOL. The server answers with it only then: a browser drops a connection whose
/// answer names a subprotocol it didn't offer.
inline bool offersSubprotocol(std::string_view subprotocols)
{
    bool offered = false;
    detail::forEachEntry(subprotocols,
                         [&offered](std::string_view entry)
                         {
                             offered = offered || (WS_SUBPROTOCOL == entry);
                         });
    return offered;
}

/// Whether an upgrade request may open a session. subprotocols is its Sec-WebSocket-Protocol header, comma-separated,
/// and origin its Origin header, empty when it sent none (#146). With credentials, an accepted request's identity goes
/// to identity (#225); without, identity is left as it is.
inline WsUpgrade decideUpgrade(const WsAuth &auth, std::string_view subprotocols, std::string_view origin,
                               std::shared_ptr<const WsIdentity> *identity = nullptr)
{
    if (!origin.empty() && !auth.allowedOrigins.empty() &&
        (auth.allowedOrigins.end() == std::find(auth.allowedOrigins.begin(), auth.allowedOrigins.end(), origin)))
    {
        return WsUpgrade::ForbiddenOrigin;
    }
    if (auth.credentials)
    {
        bool offered = false;
        std::shared_ptr<const WsIdentity> found;
        detail::forEachEntry(subprotocols,
                             [&](std::string_view entry)
                             {
                                 if (0 == entry.compare(0, WS_TOKEN_PREFIX.size(), WS_TOKEN_PREFIX))
                                 {
                                     offered = true;
                                     if (!found)
                                     {
                                         found = auth.credentials->find(entry.substr(WS_TOKEN_PREFIX.size()));
                                     }
                                 }
                             });
        if (!found)
        {
            return offered ? WsUpgrade::WrongToken : WsUpgrade::MissingToken;
        }
        if (nullptr != identity)
        {
            *identity = std::move(found);
        }
        return WsUpgrade::Accept;
    }
    if (auth.token.empty())
    {
        return WsUpgrade::Accept;
    }
    bool offered = false;
    bool matched = false;
    detail::forEachEntry(subprotocols,
                         [&](std::string_view entry)
                         {
                             if (0 == entry.compare(0, WS_TOKEN_PREFIX.size(), WS_TOKEN_PREFIX))
                             {
                                 offered = true;
                                 matched =
                                     matched || detail::sameToken(entry.substr(WS_TOKEN_PREFIX.size()), auth.token);
                             }
                         });
    if (matched)
    {
        return WsUpgrade::Accept;
    }
    return offered ? WsUpgrade::WrongToken : WsUpgrade::MissingToken;
}

} // namespace App
} // namespace COP
