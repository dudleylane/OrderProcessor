#pragma once

#ifdef BUILD_FIX

#include <vector>
#include "QueuesDef.h"

namespace COP
{
namespace App
{

class MultiOutQueues : public Queues::OutQueues
{
public:
    void addDelegate(Queues::OutQueues *delegate);

    void push(const Queues::ExecReportEvent &evnt, const std::string &target) override;
    void push(const Queues::CancelRejectEvent &evnt, const std::string &target) override;
    void push(const Queues::BusinessRejectEvent &evnt, const std::string &target) override;

private:
    std::vector<Queues::OutQueues *> delegates_;
};

} // namespace App
} // namespace COP

#endif // BUILD_FIX
