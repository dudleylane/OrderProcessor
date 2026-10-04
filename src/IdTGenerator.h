/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"
#include "Singleton.h"
#include <atomic>
#include <mutex>

namespace COP
{

/// Makes an id limit durable: every id below it may be issued, in this run or after a restart (#81)
class IdLimitSaver
{
public:
    virtual ~IdLimitSaver() {}
    /// Returns once the limit is durable, and throws if it can't be made so
    virtual void saveIdLimit(u64 limit) = 0;
};

class IdTValueGenerator
{
public:
    IdTValueGenerator(void);
    ~IdTValueGenerator(void);

    IdT getId();
    /// Makes every id issued from now on larger than id, so that after a restart new ids don't repeat ones already
    /// stored (#58). Never moves the counter back.
    void advancePast(u64 id);

    /// From now on no id is returned until a limit above it is durable. The saver saves a limit block ids ahead, and
    /// getId() saves the next one when the counter reaches it. A restart, after a crash too, continues past the last
    /// limit saved, so ids clients have seen never repeat, execution ids included, whose reports aren't persisted (#81).
    void reserve(IdLimitSaver *saver, u64 block);
    /// Saves the exact next id as the limit, so that a clean restart continues without a gap, and stops reserving. An
    /// id drawn after this is past the saved limit, and is logged as an error. Throws if the limit can't be saved; the
    /// last block limit saved is then still on disk, and still above every id issued.
    void releaseReservation();

private:
    void extendReservation(u64 id);

    std::atomic<u64> counter_;
    /// No id at or past this is returned before a limit above it is saved. While nothing reserves, it is the largest
    /// u64, so a draw costs one more load and compare than before.
    std::atomic<u64> limit_;
    std::mutex reserveLock_; // saving a limit, and the two members below
    IdLimitSaver *saver_;
    u64 block_;
};

typedef aux::Singleton<IdTValueGenerator> IdTGenerator;
} // namespace COP