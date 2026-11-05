#include "InFlightGauge.h"

#include "Logger.h"
#include "QueuesDef.h"
#include "TransactionDef.h"

#include <string>

using namespace COP;
using namespace COP::App;

namespace
{
using Clock = std::chrono::steady_clock;

const Clock::rep REFRESH_TICKS = std::chrono::duration_cast<Clock::duration>(EngineInFlightGauge::REFRESH).count();

Clock::rep nowTicks()
{
    return Clock::now().time_since_epoch().count();
}
} // namespace

EngineInFlightGauge::EngineInFlightGauge(const Queues::InQueuesContainer *inQueues,
                                         const ACID::TransactionManager *transactMgr, std::size_t maxInFlight)
    : inQueues_(inQueues), transactMgr_(transactMgr), maxInFlight_(maxInFlight),
      readAt_(nowTicks() - REFRESH_TICKS) // so that the first question reads the count
{
}

bool EngineInFlightGauge::overBound()
{
    const Clock::rep now = nowTicks();
    Clock::rep readAt = readAt_.load(std::memory_order_relaxed);
    // The thread that moves the time on reads the count; another asking meanwhile gets the last one
    if ((now - readAt >= REFRESH_TICKS) && readAt_.compare_exchange_strong(readAt, now, std::memory_order_relaxed))
    {
        refresh();
    }
    return over_.load(std::memory_order_relaxed);
}

void EngineInFlightGauge::refresh()
{
    const std::size_t queued = inQueues_->size();
    const std::size_t pending = transactMgr_->pendingTransactions();
    const bool over = (queued + pending >= maxInFlight_);
    if (over == over_.exchange(over, std::memory_order_relaxed))
    {
        return;
    }
    const std::string counts =
        std::to_string(queued) + " events waiting and " + std::to_string(pending) + " transactions pending";
    if (over)
    {
        aux::ExchLogger::instance()->warn("InFlightGauge: the engine's work in flight reached the bound, " +
                                          std::to_string(maxInFlight_) + " (--max-in-flight), with " + counts +
                                          ": clients' messages wait to be read until it is back under");
    }
    else
    {
        aux::ExchLogger::instance()->note("InFlightGauge: the engine's work in flight is back under the bound, with " +
                                          counts + ": clients' messages are read again");
    }
}
