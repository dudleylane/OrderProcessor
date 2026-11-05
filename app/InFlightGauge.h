#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

namespace COP
{

namespace Queues
{
class InQueuesContainer;
}
namespace ACID
{
class TransactionManager;
}

namespace App
{

/// The engine's work in flight, against the server's bound on it, --max-in-flight (#519): the events not yet decided
/// and the transactions not yet run. A WebSocket session reads its next message only while the work is under the
/// bound, so a client that sends faster than the engine decides and LMDB commits waits in its own TCP window, where its
/// orders had waited in the engine's memory without limit.
class InFlightGauge
{
public:
    virtual ~InFlightGauge() = default;
    /// Whether the work in flight has reached the bound
    virtual bool overBound() = 0;
};

/// The gauge on the engine's incoming queue and its transaction manager. Counting the transactions takes the manager's
/// lock, which its workers take for every transaction, so the count is read at most once a REFRESH, and overBound()
/// answers from the last count in between: the bound holds to within that long's reads. Each time the work reaches the
/// bound, the gauge logs a warning, and each time it is back under, a note.
class EngineInFlightGauge final : public InFlightGauge
{
public:
    static constexpr std::chrono::milliseconds REFRESH{ 1 };

    EngineInFlightGauge(const Queues::InQueuesContainer *inQueues, const ACID::TransactionManager *transactMgr,
                        std::size_t maxInFlight);

    bool overBound() override;

private:
    void refresh();

    const Queues::InQueuesContainer *inQueues_;
    const ACID::TransactionManager *transactMgr_;
    const std::size_t maxInFlight_;
    /// When the count was last read, in steady_clock's ticks, and whether it had reached the bound. Atomic, so that
    /// more than one thread may ask: the one that moves readAt_ on reads the count.
    std::atomic<std::chrono::steady_clock::rep> readAt_;
    std::atomic<bool> over_{ false };
};

} // namespace App
} // namespace COP
