#pragma once

#ifdef BUILD_FIX

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include "ExpiryScheduler.h"
#include "QueuesDef.h"
#include "DataModelDef.h"

namespace COP
{

namespace Store
{
class WideParamsDataStorage;
class OrderDataStorage;
} // namespace Store

namespace App
{

/// The certificate and private key the FIX acceptor serves TLS with (#228), as PEM files: the certificate, followed by
/// any intermediate ones, and its key, not encrypted. Both empty: plain TCP.
struct FixTlsFiles
{
    std::string certificate;
    std::string key;

    bool enabled() const
    {
        return !certificate.empty() || !key.empty();
    }
};

/// Keeps the process's local time New York's (#240). QuickFIX 22.1 reads a session's StartTime and EndTime in local
/// time when UseLocalTime is set, and has no zone setting of its own, so config/fix.cfg's sessions roll at 17:00 New
/// York, where the engine's business day ends, only in a process whose zone is New York's. Call it before any thread
/// starts: setenv() and tzset() aren't thread-safe. The server's own clocks are UTC, and don't change. Inline, as
/// FixTls.h's functions are, so the tests need no FixServer.cpp.
inline void useBusinessDayLocalTime()
{
    ::setenv("TZ", Proc::ExpiryScheduler::DAY_ZONE, 1);
    ::tzset();
}

/// Opaque wrapper around FixGateway + ThreadedSocketAcceptor, or ThreadedSSLSocketAcceptor with TLS.
/// No QuickFIX headers leak into callers — isolates the boost::flat_map
/// polyfill from TUs that use GCC's std::flat_map.
class FixServer
{
public:
    FixServer(Queues::InQueues *inQueues, Store::WideParamsDataStorage *wideData, Store::OrderDataStorage *orderStorage,
              SourceIdT defaultClearingId, SourceIdT defaultAccountId);
    ~FixServer();

    /// Start the FIX acceptor, with QuickFIX's settings from configFile and the counterparties from credentialsFile
    /// (#147). Each counterparty gets a session, with BeginString and SenderCompID from the settings' [DEFAULT], unless
    /// the settings already have one; a session the credentials don't name is refused, since nobody could log on to it.
    /// With tls's files the acceptor is ThreadedSSLSocketAcceptor, and serves TLS only, with the protocols FixTls.h
    /// says (#228); without them it is ThreadedSocketAcceptor, plain TCP, and it warns. Throws if a file can't be used,
    /// or the credentials name an account the data directory doesn't have.
    void start(const std::string &configFile, const std::string &credentialsFile, const FixTlsFiles &tls = {});

    /// Stop the FIX acceptor gracefully.
    void stop();

    /// Get the OutQueues implementation for routing execution reports to FIX.
    Queues::OutQueues *outQueues();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace App
} // namespace COP

#endif // BUILD_FIX
