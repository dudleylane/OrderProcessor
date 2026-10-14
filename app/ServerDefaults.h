#pragma once

#include <cstddef>

namespace COP
{
namespace App
{

/// The clearing firm seedData seeds, and the default for orderProcessorServer's --clearing-firm: the server puts it
/// on every order (#34).
inline constexpr const char *DEFAULT_CLEARING_FIRM = "HOUSE-CLEARING";

/// The default for orderProcessorServer's --default-account, used for an order that names no account. It is one of
/// the accounts seedData seeds (#34).
inline constexpr const char *DEFAULT_ACCOUNT = "TRADING-1";

/// The default for orderProcessorServer's --max-connections (#159)
inline constexpr std::size_t DEFAULT_MAX_CONNECTIONS = 256;

/// The default for orderProcessorServer's --max-session-queue-mb (#159)
inline constexpr std::size_t DEFAULT_MAX_SESSION_QUEUE_MB = 16;

/// The largest message a WebSocket client may send. The largest the protocol has, an order, is under 1 KiB (#159).
inline constexpr std::size_t MAX_CLIENT_MESSAGE_BYTES = 64 * 1024;

/// What WebSocket clients may hold of the server (#159). The defaults bound what slow clients can make the server keep:
/// 256 x 16 MiB, at most, and less when they wait on the same broadcasts, which share one copy.
struct WsLimits
{
    /// Clients connected at once. One more is closed as soon as it is accepted.
    std::size_t maxConnections = DEFAULT_MAX_CONNECTIONS;
    /// Bytes waiting to be sent to one client. A client past this, one that doesn't read, is dropped. A single message
    /// is always taken, however large: the order snapshot a client gets when it connects can be.
    std::size_t maxQueuedBytes = DEFAULT_MAX_SESSION_QUEUE_MB << 20;
    /// The largest message a client may send. A larger one ends its session.
    std::size_t maxMessageBytes = MAX_CLIENT_MESSAGE_BYTES;
};

} // namespace App
} // namespace COP
