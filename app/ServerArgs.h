#pragma once

#include <charconv>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <boost/asio/ip/address.hpp>

#include "LMDBStorage.h"
#include "ServerDefaults.h"
#include "TypesDef.h"
#include "WsAuth.h"

namespace COP
{
namespace App
{

/// What orderProcessorServer is asked to do
struct ServerArgs
{
    unsigned short port = 8080;          // 0: a port the system picks
    std::string bindAddress = "0.0.0.0"; // the address the server listens on (#146)
    std::string dataDir = "./data";
    int workers = 0;
    int cpuAffinityStart = -1; // -1 = disabled, >= 0 = pin starting from this core
    bool hugePages = false;
    std::string fixCfg;                               // QuickFIX's settings file (empty: no FIX gateway)
    std::string clearingFirm = DEFAULT_CLEARING_FIRM; // the clearing firm on every order (#34)
    std::string defaultAccount = DEFAULT_ACCOUNT;     // the account of an order that names none (#34)
    bool debug = false;                               // debug logging, off unless asked for (#76)
    // LMDB's starting map, in MiB: a write that finds the map full doubles it (#148)
    u64 lmdbMapMb = Store::LMDBStorage::DEFAULT_MAP_SIZE >> 20;
    WsLimits wsLimits; // what WebSocket clients may hold of the server (#159)
    // Who may open a WebSocket session (#146): the file the access token is in, and the Origins a browser may come from.
    // The token may come from OMS_WS_TOKEN instead, and the Origins from OMS_ALLOWED_ORIGINS, which main() reads.
    std::string wsTokenFile;
    std::vector<std::string> allowedOrigins;
    bool help = false; // --help: print the usage and exit
};

/// The options orderProcessorServer takes, for its usage message
inline std::string serverUsage()
{
    return "usage: orderProcessorServer [OPTION]...\n"
           "  --port N                  WebSocket port, 0 to 65535 (default 8080; 0: one the system picks)\n"
           "  --bind ADDRESS            the WebSocket server's IP address (default 0.0.0.0, every interface)\n"
           "  --data-dir DIR            LMDB data directory (default ./data)\n"
           "  --workers N               TBB worker threads, and as many event and transaction processor pairs\n"
           "                            (default 0: TBB's default, and 2 pairs)\n"
           "  --cpu-affinity CORE       pin the main thread to CORE (default -1: no pinning)\n"
           "  --huge-pages              report whether huge pages are available\n"
#ifdef BUILD_FIX
           "  --fix-cfg FILE            QuickFIX's settings file, which starts the FIX gateway (default: none)\n"
#endif
           "  --clearing-firm FIRM      the clearing firm on every order (default " +
           std::string(DEFAULT_CLEARING_FIRM) +
           ")\n"
           "  --default-account ACCOUNT the account of an order that names none (default " +
           std::string(DEFAULT_ACCOUNT) +
           ")\n"
           "  --debug                   debug logging\n"
           "  --lmdb-map-mb N           LMDB's starting map, in MiB (default " +
           std::to_string(Store::LMDBStorage::DEFAULT_MAP_SIZE >> 20) +
           ")\n"
           "  --max-connections N       WebSocket clients at once (default " +
           std::to_string(DEFAULT_MAX_CONNECTIONS) +
           ")\n"
           "  --max-session-queue-mb N  MiB of messages that may wait for one client (default " +
           std::to_string(DEFAULT_MAX_SESSION_QUEUE_MB) +
           ")\n"
           "  --ws-token-file FILE      the access token every WebSocket client must offer, from FILE; or set\n"
           "                            OMS_WS_TOKEN. Without one, any client that reaches the port may trade\n"
           "  --allowed-origin ORIGIN   a browser Origin that may connect; repeat for more, or set\n"
           "                            OMS_ALLOWED_ORIGINS, comma-separated (default: any)\n"
           "  -h, --help                print this and exit\n";
}

namespace detail
{
/// Reads all of value as a decimal number from min to max into *out
template <typename T> bool readNumber(const std::string &value, T min, T max, T *out)
{
    T number{};
    const char *const end = value.data() + value.size();
    const std::from_chars_result result = std::from_chars(value.data(), end, number);
    if ((std::errc() != result.ec) || (end != result.ptr) || (number < min) || (max < number))
    {
        return false;
    }
    *out = number;
    return true;
}
} // namespace detail

/// Reads orderProcessorServer's arguments. An option it doesn't know, an option without its value, and a value that
/// isn't a number in the option's range return nullopt, with the reason in *error. Until #173 the server ignored what
/// it didn't know, so a typo such as `--workres 4` ran it with the defaults, and a number out of range wrapped or
/// threw.
inline std::optional<ServerArgs> parseServerArgs(const std::vector<std::string> &args, std::string *error)
{
    ServerArgs parsed;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        if (("--help" == arg) || ("-h" == arg))
        {
            parsed.help = true;
            continue;
        }
        if ("--huge-pages" == arg)
        {
            parsed.hugePages = true;
            continue;
        }
        if ("--debug" == arg)
        {
            parsed.debug = true;
            continue;
        }
        static const std::vector<std::string> withValues = {
            "--port",         "--data-dir",        "--workers",
            "--cpu-affinity", "--clearing-firm",   "--default-account",
            "--lmdb-map-mb",  "--max-connections", "--max-session-queue-mb",
            "--bind",         "--ws-token-file",   "--allowed-origin",
            "--fix-cfg"
        };
        bool takesValue = false;
        for (const std::string &option : withValues)
        {
            takesValue = takesValue || (option == arg);
        }
        if (!takesValue)
        {
            *error = "unknown option: " + arg;
            return std::nullopt;
        }
        if (i + 1 >= args.size())
        {
            *error = arg + " needs a value";
            return std::nullopt;
        }
        const std::string &value = args[++i];
        // The range a number must be in, for the message if it isn't
        std::string range;
        bool read = true;
        if ("--port" == arg)
        {
            range = "from 0 to 65535";
            read =
                detail::readNumber<unsigned short>(value, 0, std::numeric_limits<unsigned short>::max(), &parsed.port);
        }
        else if ("--workers" == arg)
        {
            range = "from 0";
            read = detail::readNumber<int>(value, 0, std::numeric_limits<int>::max(), &parsed.workers);
        }
        else if ("--cpu-affinity" == arg)
        {
            range = "from -1";
            read = detail::readNumber<int>(value, -1, std::numeric_limits<int>::max(), &parsed.cpuAffinityStart);
        }
        else if ("--lmdb-map-mb" == arg)
        {
            range = "from 1";
            read = detail::readNumber<u64>(value, 1, std::numeric_limits<size_t>::max() >> 20, &parsed.lmdbMapMb);
        }
        else if ("--max-connections" == arg)
        {
            range = "from 1";
            read = detail::readNumber<size_t>(value, 1, std::numeric_limits<size_t>::max(),
                                              &parsed.wsLimits.maxConnections);
        }
        else if ("--max-session-queue-mb" == arg)
        {
            range = "from 1";
            size_t mb = 0;
            read = detail::readNumber<size_t>(value, 1, std::numeric_limits<size_t>::max() >> 20, &mb);
            parsed.wsLimits.maxQueuedBytes = mb << 20;
        }
        else if (value.empty())
        {
            *error = arg + " needs a value";
            return std::nullopt;
        }
        else if ("--data-dir" == arg)
        {
            parsed.dataDir = value;
        }
        else if ("--bind" == arg)
        {
            boost::system::error_code ec;
            boost::asio::ip::make_address(value, ec);
            if (ec)
            {
                *error = "--bind needs an IP address, not '" + value + "'";
                return std::nullopt;
            }
            parsed.bindAddress = value;
        }
        else if ("--ws-token-file" == arg)
        {
            parsed.wsTokenFile = value;
        }
        else if ("--allowed-origin" == arg)
        {
            parsed.allowedOrigins.push_back(value);
        }
        else if ("--clearing-firm" == arg)
        {
            parsed.clearingFirm = value;
        }
        else if ("--fix-cfg" == arg)
        {
#ifdef BUILD_FIX
            parsed.fixCfg = value;
#else
            // A server built without the gateway would run without it, though asked for it (#173)
            *error = "--fix-cfg needs a server built with BUILD_FIX=ON";
            return std::nullopt;
#endif
        }
        else
        {
            parsed.defaultAccount = value;
        }
        if (!read)
        {
            *error = arg + " needs a whole number " + range + ", not '" + value + "'";
            return std::nullopt;
        }
    }
    return parsed;
}

/// Who may open a WebSocket session (#146), from the arguments and the environment: the access token from
/// --ws-token-file or OMS_WS_TOKEN, not both, and the Origins from --allowed-origin and OMS_ALLOWED_ORIGINS,
/// comma-separated. Returns false, with the reason in *error, for a token file it can't read, two tokens, or a token
/// isValidWsToken() refuses: a usage error, as a flag it can't read is.
inline bool resolveWsAuth(const ServerArgs &args, const char *envToken, const char *envOrigins, WsAuth *auth,
                          std::string *error)
{
    WsAuth resolved;
    if (!args.wsTokenFile.empty())
    {
        std::ifstream file(args.wsTokenFile);
        if (!file)
        {
            *error = "can't read the token file " + args.wsTokenFile;
            return false;
        }
        // The first line; a CR or spaces at its end, as an editor may leave them, aren't part of the token
        std::getline(file, resolved.token);
        while (!resolved.token.empty() &&
               (('\r' == resolved.token.back()) || (' ' == resolved.token.back()) || ('\t' == resolved.token.back())))
        {
            resolved.token.pop_back();
        }
        // A secret that failed to render would otherwise start a server anyone may use
        if (resolved.token.empty())
        {
            *error = "the token file " + args.wsTokenFile + " has no token on its first line";
            return false;
        }
    }
    if (nullptr != envToken)
    {
        if ('\0' == *envToken)
        {
            *error = "OMS_WS_TOKEN is set, but empty: unset it to run without a token";
            return false;
        }
        if (!resolved.token.empty())
        {
            *error = "the WebSocket token is given twice, by --ws-token-file and by OMS_WS_TOKEN";
            return false;
        }
        resolved.token = envToken;
    }
    if (!resolved.token.empty() && !isValidWsToken(resolved.token))
    {
        *error = "the WebSocket token needs at least " + std::to_string(WS_MIN_TOKEN_LENGTH) +
                 " letters, digits, '.', '_' or '-' (`openssl rand -hex 32` makes one)";
        return false;
    }
    // An Origin is scheme://host[:port], which a browser sends without a trailing '/': one written with it would match
    // no page
    const auto addOrigin = [&resolved](std::string_view origin)
    {
        while (!origin.empty() && ('/' == origin.back()))
        {
            origin.remove_suffix(1);
        }
        if (!origin.empty())
        {
            resolved.allowedOrigins.emplace_back(origin);
        }
    };
    for (const std::string &origin : args.allowedOrigins)
    {
        addOrigin(origin);
    }
    if (nullptr != envOrigins)
    {
        detail::forEachEntry(envOrigins, addOrigin);
    }
    *auth = std::move(resolved);
    return true;
}

} // namespace App
} // namespace COP
