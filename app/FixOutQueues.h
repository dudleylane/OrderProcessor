#pragma once

#ifdef BUILD_FIX

#include "QueuesDef.h"

namespace COP {

namespace Store { class OrderDataStorage; }

namespace App {

class FixGateway;

class FixOutQueues : public Queues::OutQueues {
public:
    FixOutQueues(FixGateway* gateway, Store::OrderDataStorage* orderStorage);

    void push(const Queues::ExecReportEvent& evnt, const std::string& target) override;
    void push(const Queues::CancelRejectEvent& evnt, const std::string& target) override;
    void push(const Queues::BusinessRejectEvent& evnt, const std::string& target) override;

private:
    FixGateway* gateway_;
    Store::OrderDataStorage* orderStorage_;
};

}} // namespace COP::App

#endif // BUILD_FIX
