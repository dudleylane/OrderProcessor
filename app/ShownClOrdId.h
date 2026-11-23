#pragma once

#include <string>

#include "DataModelDef.h"

namespace COP
{
namespace App
{

/// FIX stores an order's ClOrdID under its session's key, `<source>|<ClOrdID>`, so that two sessions may use one
/// ClOrdID (#71). Its client's own is what follows the exact key the order's source gives: a client's ClOrdID may
/// itself contain '|'. A ClOrdID stored without the key, as a WebSocket order's is, is its own.
inline std::string clientClOrdId(const std::string &stored, const std::string &source)
{
    if (!source.empty() && (stored.size() > source.size()) && ('|' == stored[source.size()]) &&
        (0 == stored.compare(0, source.size(), source)))
    {
        return stored.substr(source.size() + 1);
    }
    return stored;
}

/// The ClOrdID a replacement's original is shown with (#645): its own, as clientClOrdId() takes it off with the
/// replacement's source. When a WebSocket client replaced a FIX order the sources differ, so a key FIX made,
/// `FIX:<SenderCompID>-><TargetCompID>|<ClOrdID>` (FixGateway::makeSourceString()), is read for the ClOrdID after it.
inline std::string shownOrigClOrdId(const std::string &stored, const std::string &source)
{
    const std::string own = clientClOrdId(stored, source);
    if ((own != stored) || (0 != stored.rfind("FIX:", 0)))
    {
        return own;
    }
    const size_t arrow = stored.find("->");
    const size_t bar = (std::string::npos == arrow) ? std::string::npos : stored.find('|', arrow + 2);
    return (std::string::npos == bar) ? stored : stored.substr(bar + 1);
}

/// The ClOrdID an order is shown with, to WebSocket clients as to FIX ones: its client's own (#645). An order's
/// ClOrdID and source never change, so this reads them without the order's lock, on any thread: a WideDataLazyRef
/// loads its value once, under a mutex of its own (#455).
inline std::string shownClOrdId(const OrderEntry &order)
{
    std::string stored;
    if (order.clOrderId_.isSet())
    {
        const RawDataEntry &raw = order.clOrderId_.get();
        if ((nullptr != raw.data_) && (0 < raw.length_))
        {
            stored.assign(raw.data_, raw.length_);
        }
    }
    return clientClOrdId(stored, order.source_.isSet() ? order.source_.get() : StringT());
}

} // namespace App
} // namespace COP
