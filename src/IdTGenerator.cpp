/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include <ctime>
#include <limits>
#include <string>
#include "IdTGenerator.h"
#include "Logger.h"

using namespace COP;

IdTValueGenerator::IdTValueGenerator(void) : saver_(nullptr), block_(0)
{
    counter_.store(1);
    limit_.store(std::numeric_limits<u64>::max());
}

IdTValueGenerator::~IdTValueGenerator(void) {}

IdT IdTValueGenerator::getId()
{
    std::time_t ltime = std::time(nullptr);
    // Use relaxed ordering - only need atomicity for counter uniqueness,
    // not memory synchronization. The ID value itself provides ordering context.
    const u64 id = counter_.fetch_add(1, std::memory_order_relaxed);
    // A limit is stored only after it is saved, so an id below the one loaded here is covered (#81)
    if (id >= limit_.load(std::memory_order_acquire)) [[unlikely]]
    {
        extendReservation(id);
    }
    return IdT(id, static_cast<u32>(ltime));
}

void IdTValueGenerator::extendReservation(u64 id)
{
    std::lock_guard<std::mutex> lock(reserveLock_);
    if (id < limit_.load(std::memory_order_relaxed))
    {
        return; // another draw saved a limit past this id while this one waited
    }
    if (nullptr == saver_)
    {
        // Only a draw after releaseReservation() gets here: nothing will save a limit above this id
        aux::ExchLogger::instance()->error("IdTValueGenerator: id " + std::to_string(id) +
                                           " was drawn after the id reservation ended, so it may repeat after a "
                                           "restart");
        return;
    }
    // From this id, not from the old limit: draws that overshot it while this one waited are covered too
    const u64 limit = id + block_;
    saver_->saveIdLimit(limit); // throws if it can't, and then this id is not returned
    limit_.store(limit, std::memory_order_release);
}

void IdTValueGenerator::reserve(IdLimitSaver *saver, u64 block)
{
    assert(nullptr != saver);
    assert(0 < block);
    std::lock_guard<std::mutex> lock(reserveLock_);
    saver_ = saver;
    block_ = block;
    const u64 limit = counter_.load(std::memory_order_relaxed) + block;
    saver->saveIdLimit(limit);
    limit_.store(limit, std::memory_order_release);
}

void IdTValueGenerator::releaseReservation()
{
    std::lock_guard<std::mutex> lock(reserveLock_);
    if (nullptr == saver_)
    {
        return;
    }
    IdLimitSaver *saver = saver_;
    saver_ = nullptr;
    const u64 next = counter_.load(std::memory_order_relaxed);
    limit_.store(next, std::memory_order_release); // a later draw is past it, and is logged
    saver->saveIdLimit(next);
}

void IdTValueGenerator::advancePast(u64 id)
{
    const u64 next = id + 1;
    u64 current = counter_.load(std::memory_order_relaxed);
    while ((current < next) && !counter_.compare_exchange_weak(current, next, std::memory_order_relaxed))
    {
    }
}
