/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "PGWriteRequest.h"
#include "DataModelDef.h"

namespace COP::PG
{

class PGRequestBuilder final
{
public:
    static InstrumentWrite fromInstrument(const InstrumentEntry &val);
    static AccountWrite fromAccount(const AccountEntry &val);
    static ClearingWrite fromClearing(const ClearingEntry &val);
    static OrderWrite fromOrder(const OrderEntry &val);
};

} // namespace COP::PG
