/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "QueuesManager.h"
#include "Logger.h"

using namespace std;
using namespace COP::Queues;

QueuesManager::QueuesManager(void)
{
	aux::ExchLogger::instance()->note("QueuesManager created");
}

QueuesManager::~QueuesManager(void)
{
	aux::ExchLogger::instance()->note("QueuesManager destroyed");
}

InQueues *QueuesManager::getInQueues()
{
	return &inQueues_;
}

OutQueues *QueuesManager::getOutQueues()
{
	return &outQueues_;
}

