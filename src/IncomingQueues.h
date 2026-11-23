/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <optional>
#include <variant>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/spin_mutex.h>

#include "QueuesDef.h"
#include "CacheAlignedAtomic.h"

namespace COP
{
namespace Queues
{

class IncomingQueues : public InQueues, public InQueuesPublisher, public InQueuesContainer
{
public:
    IncomingQueues(void);
    ~IncomingQueues(void);

public:
    /// reimplemented from InQueues
    void push(const std::string &source, const OrderEvent &evnt) override;
    void push(const std::string &source, const OrderCancelEvent &evnt) override;
    void push(const std::string &source, const OrderReplaceEvent &evnt) override;
    void push(const std::string &source, const OrderChangeStateEvent &evnt) override;
    void push(const std::string &source, const ProcessEvent &evnt) override;
    void push(const std::string &source, const TimerEvent &evnt) override;
    InQueuesObserver *attach(InQueuesObserver *obs) override;
    InQueuesObserver *detach() override;

public:
    /// reimplemented from InQueuesPublisher
    InQueueProcessor *attach(InQueueProcessor *obs) override;
    InQueueProcessor *detachProcessor() override;

public:
    /// reimplemented from InQueuesContainer
    u32 size() const override;
    bool top(InQueueProcessor *obs) override;
    bool pop() override;
    bool pop(InQueueProcessor *obs) override;

private:
    std::atomic<InQueueProcessor *> processor_;
    std::atomic<InQueuesObserver *> observer_;

    /// Unified event type using std::variant for lock-free queue
    using EventVariant =
        std::variant<OrderEvent, OrderCancelEvent, OrderReplaceEvent, OrderChangeStateEvent, ProcessEvent, TimerEvent>;

    /// Queued event containing source and the event data
    struct QueuedEvent
    {
        std::string source_;
        EventVariant event_;

        QueuedEvent() = default;
        QueuedEvent(const std::string &src, EventVariant evt) : source_(src), event_(std::move(evt)) {}
    };

    /// Lock-free concurrent queue (MPMC safe)
    oneapi::tbb::concurrent_queue<QueuedEvent> eventQueue_;

    /// Atomic size counter for O(1) size() queries - cache aligned
    CacheAlignedAtomic<u32> queueSize_;

    /// Pending event for top() without pop() - protected by pendingLock_
    mutable oneapi::tbb::spin_mutex pendingLock_;
    std::optional<QueuedEvent> pendingEvent_;

private:
    void clear();
    void dispatchEvent(InQueueProcessor *obs, const std::string &source, const EventVariant &event);
};

} // namespace Queues
} // namespace COP