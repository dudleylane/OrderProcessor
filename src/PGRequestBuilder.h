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

/// The version of the Postgres schema the server's statements are written for (#590). docker/init-db makes a new volume
/// at it, in schema_version, and scripts/pg-migrate.sh brings an older one up to it, one numbered migration at a time.
/// A change to the schema bumps it, with a migration of its own in docker/migrations.
inline constexpr int SCHEMA_VERSION = 1;

class PGRequestBuilder final
{
public:
    static InstrumentWrite fromInstrument(const InstrumentEntry &val);
    static AccountWrite fromAccount(const AccountEntry &val);
    static ClearingWrite fromClearing(const ClearingEntry &val);
    /// The order's row with its own figures, which are its record's for an order decoded from storage or restored
    static OrderWrite fromOrder(const OrderEntry &val);
    /// The order's row with the figures a decision left it with (#411), in place of its own
    static OrderWrite fromOrder(const OrderEntry &val, const OrderFigures &figures);
};

} // namespace COP::PG
