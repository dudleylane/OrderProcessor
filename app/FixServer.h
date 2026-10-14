#pragma once

#ifdef BUILD_FIX

#include <memory>
#include <string>
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

/// Opaque wrapper around FixGateway + ThreadedSocketAcceptor.
/// No QuickFIX headers leak into callers — isolates the boost::flat_map
/// polyfill from TUs that use GCC's std::flat_map.
class FixServer
{
public:
    FixServer(Queues::InQueues *inQueues, Store::WideParamsDataStorage *wideData, Store::OrderDataStorage *orderStorage,
              SourceIdT defaultClearingId, SourceIdT defaultAccountId);
    ~FixServer();

    /// Start the FIX acceptor (ThreadedSocketAcceptor), with QuickFIX's settings from configFile and the counterparties
    /// from credentialsFile (#147). Each counterparty gets a session, with BeginString and SenderCompID from the
    /// settings' [DEFAULT], unless the settings already have one; a session the credentials don't name is refused, since
    /// nobody could log on to it. Throws if either file can't be used, or names an account the data directory doesn't
    /// have.
    void start(const std::string &configFile, const std::string &credentialsFile);

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
