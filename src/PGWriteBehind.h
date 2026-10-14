/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include "PGWriteRequest.h"

namespace COP::PG
{

/// Mirrors what the server persists in LMDB to PostgreSQL, behind it: on a thread of its own, in the order it is given,
/// so a later write of an order lands after an earlier one (#151). LMDB stays the server's record; Postgres is a copy
/// for queries and reports, and nothing waits for it.
class PGWriteBehind final
{
public:
    /// Starts the writer's thread, which connects to connString, a libpq connection string. It doesn't log the string,
    /// which may carry a password.
    explicit PGWriteBehind(const std::string &connString);
    ~PGWriteBehind();

    void enqueue(PGWriteRequest &&req);
    /// Writes what is queued, then stops the thread. If the database can't be reached by then, what is left is dropped,
    /// counted and logged: shutdown doesn't wait for it.
    void shutdown();

    u64 totalEnqueued() const
    {
        return totalEnqueued_.load(std::memory_order_relaxed);
    }
    u64 totalWritten() const
    {
        return totalWritten_.load(std::memory_order_relaxed);
    }
    /// Requests Postgres refused, such as a value its column can't hold: each is logged, and the rest of its batch lands
    u64 totalErrors() const
    {
        return totalErrors_.load(std::memory_order_relaxed);
    }
    /// Requests left unwritten at shutdown, because the database couldn't be reached
    u64 totalDropped() const
    {
        return totalDropped_.load(std::memory_order_relaxed);
    }

    /// Requests written in one transaction: a commit per request made a startup's sync of every order hold up live
    /// writes behind thousands of commits
    static constexpr std::size_t BATCH_SIZE = 256;
    /// The connection string with connect_timeout=5 when it sets none: libpq waits for a connection forever by default,
    /// and the writer connects at shutdown too
    static std::string withConnectTimeout(const std::string &connString);

private:
    void run();

    const std::string connString_;
    std::mutex mutex_; // guards queue_ and stopping_
    std::condition_variable wake_;
    std::deque<PGWriteRequest> queue_;
    bool stopping_ = false;
    std::thread thread_;

    std::atomic<u64> totalEnqueued_{ 0 };
    std::atomic<u64> totalWritten_{ 0 };
    std::atomic<u64> totalErrors_{ 0 };
    std::atomic<u64> totalDropped_{ 0 };
};

} // namespace COP::PG
