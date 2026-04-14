#ifdef BUILD_FIX

#include "MultiOutQueues.h"

using namespace COP;
using namespace COP::App;
using namespace COP::Queues;

void MultiOutQueues::addDelegate(OutQueues* delegate) {
    delegates_.push_back(delegate);
}

void MultiOutQueues::push(const ExecReportEvent& evnt, const std::string& target) {
    for (auto* d : delegates_) d->push(evnt, target);
}

void MultiOutQueues::push(const CancelRejectEvent& evnt, const std::string& target) {
    for (auto* d : delegates_) d->push(evnt, target);
}

void MultiOutQueues::push(const BusinessRejectEvent& evnt, const std::string& target) {
    for (auto* d : delegates_) d->push(evnt, target);
}

#endif // BUILD_FIX
