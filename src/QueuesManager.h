/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "QueuesDef.h"
#include "OutgoingQueues.h"
#include "IncomingQueues.h"

namespace COP
{
namespace Queues
{

class QueuesManager
{
public:
    QueuesManager(void);
    ~QueuesManager(void);

public:
    InQueues *getInQueues();
    OutQueues *getOutQueues();

private:
    OutgoingQueues outQueues_;
    IncomingQueues inQueues_;
};

} // namespace Queues
} // namespace COP