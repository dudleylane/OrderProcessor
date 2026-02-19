/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <tbb/concurrent_queue.h>
#include "PGWriteRequest.h"

namespace COP::PG {

class PGWriteBehind final {
public:
    explicit PGWriteBehind(const std::string& connString);
    ~PGWriteBehind();

    void enqueue(PGWriteRequest&& req);
    void shutdown();

    u64 totalEnqueued() const { return totalEnqueued_.load(std::memory_order_relaxed); }
    u64 totalWritten() const { return totalWritten_.load(std::memory_order_relaxed); }
    u64 totalErrors() const { return totalErrors_.load(std::memory_order_relaxed); }

private:
    void run();

    std::string connString_;
    tbb::concurrent_queue<PGWriteRequest> queue_;
    std::thread thread_;
    std::atomic<bool> shutdown_{false};

    std::atomic<u64> totalEnqueued_{0};
    std::atomic<u64> totalWritten_{0};
    std::atomic<u64> totalErrors_{0};
};

} // namespace COP::PG
