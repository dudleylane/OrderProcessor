#pragma once

#include <chrono>
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

/// The default for orderProcessorServer's --max-in-flight: the engine's work in flight, the events not yet decided and
/// the transactions not yet run, at which the WebSocket sessions stop reading their clients' messages (#519). A backlog
/// of that size drains in about 20 s at LMDB's ~2 ms a commit, and holds tens of MB.
inline constexpr std::size_t DEFAULT_MAX_IN_FLIGHT = 10'000;

/// What WebSocket clients may hold of the server (#159). The defaults bound what slow clients can make the server keep:
/// 256 x 16 MiB, at most, and less when they wait on the same broadcasts, which share one copy. Each also holds the state
/// it got as it connected, its order snapshot included, until it has read it (#512).
struct WsLimits
{
    /// Clients connected at once. One more is closed as soon as it is accepted.
    std::size_t maxConnections = DEFAULT_MAX_CONNECTIONS;
    /// Bytes waiting to be sent to one client after the state it gets as it connects, which isn't counted: its order
    /// snapshot alone can be larger (#512). A client past this, one that doesn't read, is dropped. A message is taken
    /// whatever its size when nothing waits after that state.
    std::size_t maxQueuedBytes = DEFAULT_MAX_SESSION_QUEUE_MB << 20;
    /// The largest message a client may send. A larger one ends its session.
    std::size_t maxMessageBytes = MAX_CLIENT_MESSAGE_BYTES;
    /// How long a client may send nothing before its session ends: Beast pings it at half the time, and ends the
    /// session when nothing, the answer included, has come by the end. Beast's suggested 300 s. A session that waits
    /// for the engine's work in flight to drain (#519) reads nothing, so the time doesn't run while it waits.
    std::chrono::seconds idleTimeout{ 300 };
};

} // namespace App
} // namespace COP
