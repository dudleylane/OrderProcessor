/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "PGRequestBuilder.h"
#include "PGEnumStrings.h"
#include "WideDataLazyRef.h"

namespace COP::PG {

namespace {
    const char* safeStr(const char* s) { return s ? s : ""; }
}

InstrumentWrite PGRequestBuilder::fromInstrument(const InstrumentEntry& val) {
    return InstrumentWrite{
        val.symbol_,
        val.securityId_,
        val.securityIdSource_
    };
}

AccountWrite PGRequestBuilder::fromAccount(const AccountEntry& val) {
    return AccountWrite{
        val.account_,
        val.firm_,
        safeStr(toSQL(val.type_))
    };
}

ClearingWrite PGRequestBuilder::fromClearing(const ClearingEntry& val) {
    return ClearingWrite{
        val.firm_
    };
}

OrderWrite PGRequestBuilder::fromOrder(const OrderEntry& val) {
    OrderWrite w;

    w.orderId = val.orderId_.id_;
    w.orderDate = val.orderId_.date_;

    // Resolve lazy references
    const auto& instrument = val.instrument_.get();
    w.instrumentSymbol = instrument.symbol_;

    const auto& account = val.account_.get();
    w.accountName = account.account_;

    const auto& clearing = val.clearing_.get();
    w.clearingFirm = clearing.firm_;

    const auto& dest = val.destination_.get();
    w.destination = dest;

    const auto& source = val.source_.get();
    w.source = source;

    // Resolve RawDataEntry references for client order IDs
    if (val.clOrderId_.getId().isValid()) {
        const auto& clOrdId = val.clOrderId_.get();
        if (clOrdId.data_ && clOrdId.length_ > 0)
            w.clOrderId = std::string(clOrdId.data_, clOrdId.length_);
    }

    if (val.origClOrderId_.getId().isValid()) {
        const auto& origClOrdId = val.origClOrderId_.get();
        if (origClOrdId.data_ && origClOrdId.length_ > 0)
            w.origClOrderId = std::string(origClOrdId.data_, origClOrdId.length_);
    }

    // Map enums to PG strings
    w.side = safeStr(toSQL(val.side_));
    w.ordType = safeStr(toSQL(val.ordType_));
    w.status = safeStr(toSQL(val.status_));
    w.tif = safeStr(toSQL(val.tif_));
    w.capacity = safeStr(toSQL(val.capacity_));
    w.currency = safeStr(toSQL(val.currency_));
    w.settlType = safeStr(toSQL(val.settlType_));

    // Copy scalar fields
    w.price = val.price_;
    w.stopPx = val.stopPx_;
    w.avgPx = val.avgPx_;
    w.dayAvgPx = val.dayAvgPx_;

    w.minQty = val.minQty_;
    w.orderQty = val.orderQty_;
    w.leavesQty = val.leavesQty_;
    w.cumQty = val.cumQty_;
    w.dayOrderQty = val.dayOrderQty_;
    w.dayCumQty = val.dayCumQty_;

    w.expireTime = val.expireTime_;
    w.settlDate = val.settlDate_;

    return w;
}

} // namespace COP::PG
