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

namespace COP
{

class IdTValueGenerator
{
public:
    IdTValueGenerator(void);
    ~IdTValueGenerator(void);

    IdT getId();
    /// Makes every id issued from now on larger than id, so that after a restart new ids don't repeat ones already
    /// stored (#58). Never moves the counter back.
    void advancePast(u64 id);

private:
    std::atomic<u64> counter_;
};

typedef aux::Singleton<IdTValueGenerator> IdTGenerator;
} // namespace COP