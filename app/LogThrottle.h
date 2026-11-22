#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "TypesDef.h"

namespace COP
{
namespace App
{

/// At most one log line a period, counting the rest (#547). A client could make its session contain an exception for
/// every message it sent, and each was an error line, which the log writes out at once (#65).
class LogThrottle
{
public:
    explicit LogThrottle(std::chrono::steady_clock::duration period) : period_(period) {}

    /// Whether to log a line at now. If so, *skipped gives how many lines it refused since the last one it allowed;
    /// if not, it counts this one.
    bool allow(std::chrono::steady_clock::time_point now, u64 *skipped)
    {
        if (logged_ && (now - last_ < period_))
        {
            ++skipped_;
            return false;
        }
        *skipped = skipped_;
        skipped_ = 0;
        logged_ = true;
        last_ = now;
        return true;
    }

private:
    std::chrono::steady_clock::duration period_;
    std::chrono::steady_clock::time_point last_{};
    bool logged_ = false;
    u64 skipped_ = 0;
};

/// A client's bytes as the log may hold them (#547): printable ASCII as it is, with a backslash doubled, and every
/// other byte as \xHH, so that a line carries no control character, and no byte that isn't UTF-8, into the log
inline std::string escapeForLog(std::string_view bytes)
{
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(bytes.size());
    for (const char c : bytes)
    {
        const auto byte = static_cast<unsigned char>(c);
        if ('\\' == c)
        {
            escaped += "\\\\";
        }
        else if ((0x20 <= byte) && (byte < 0x7F))
        {
            escaped += c;
        }
        else
        {
            escaped += "\\x";
            escaped += HEX[byte >> 4];
            escaped += HEX[byte & 0x0F];
        }
    }
    return escaped;
}

/// The line a contained exception is logged with (#547): its number, where it was caught, the exception's text and
/// the request, both escaped (escapeForLog()), the request cut at maxRequest bytes, and, when the throttle left any
/// out, how many. Pure, so that a test checks the line without reading a log a server thread writes to.
inline std::string containedExceptionLine(const char *who, u64 number, std::string_view where, std::string_view what,
                                          std::string_view request, size_t maxRequest, u64 skipped)
{
    std::string line = std::string(who) + ": contained exception #" + std::to_string(number) + " in " +
                       std::string(where) + ": " + escapeForLog(what);
    if (0 < skipped)
    {
        line +=
            " (" + std::to_string(skipped) + " more in this session since the last logged weren't: one a second is)";
    }
    if (!request.empty())
    {
        line += "; request: " + escapeForLog(request.substr(0, maxRequest));
    }
    return line;
}

} // namespace App
} // namespace COP
