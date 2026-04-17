#pragma once

#ifdef BUILD_FIX

#include <memory>
#include <string>
#include "QueuesDef.h"
#include "DataModelDef.h"

namespace COP {

namespace Store {
    class WideParamsDataStorage;
    class OrderDataStorage;
}

namespace App {

/// Opaque wrapper around FixGateway + ThreadedSocketAcceptor.
/// No QuickFIX headers leak into callers — isolates the boost::flat_map
/// polyfill from TUs that use GCC's std::flat_map.
class FixServer {
public:
    FixServer(Queues::InQueues* inQueues,
              Store::WideParamsDataStorage* wideData,
              Store::OrderDataStorage* orderStorage,
              SourceIdT defaultClearingId);
    ~FixServer();

    /// Start the FIX acceptor (ThreadedSocketAcceptor).
    void start(const std::string& configFile);

    /// Stop the FIX acceptor gracefully.
    void stop();

    /// Get the OutQueues implementation for routing execution reports to FIX.
    Queues::OutQueues* outQueues();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}} // namespace COP::App

#endif // BUILD_FIX
