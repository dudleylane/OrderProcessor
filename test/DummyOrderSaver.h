/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "DataModelDef.h"

namespace test
{

/**
 * No-op implementation of OrderSaver for tests and benchmarks that don't need persistence.
 *
 * Not in TestAux.h, which includes GoogleTest: the benchmarks use it too, and they don't build with GoogleTest (#530).
 */
class DummyOrderSaver : public COP::OrderSaver
{
public:
    DummyOrderSaver() = default;
    ~DummyOrderSaver() = default;

    COP::u32 save(const COP::OrderEntry &, const COP::OrderFigures &) override
    {
        return 0;
    }
    void erase(const COP::IdT &, COP::u32) override {}
};

} // namespace test
