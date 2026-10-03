/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <ctime>
#include "IdTGenerator.h"

using namespace COP;

IdTValueGenerator::IdTValueGenerator(void)
{
    counter_.store(1);
}

IdTValueGenerator::~IdTValueGenerator(void) {}

IdT IdTValueGenerator::getId()
{
    std::time_t ltime = std::time(nullptr);
    // Use relaxed ordering - only need atomicity for counter uniqueness,
    // not memory synchronization. The ID value itself provides ordering context.
    return IdT(counter_.fetch_add(1, std::memory_order_relaxed), static_cast<u32>(ltime));
}

void IdTValueGenerator::advancePast(u64 id)
{
    const u64 next = id + 1;
    u64 current = counter_.load(std::memory_order_relaxed);
    while ((current < next) && !counter_.compare_exchange_weak(current, next, std::memory_order_relaxed))
    {
    }
}
