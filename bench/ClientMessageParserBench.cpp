/**
 Concurrent Order Processor library - WebSocket client message parser Benchmarks

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <benchmark/benchmark.h>
#include <string>

#include "ClientMessageParser.h"

using namespace COP;

namespace
{

/// A limit order as the order entry panel sends one: the fields it always sends, the price, and an account
const std::string NEW_ORDER = R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                              R"("orderQty":100,"tif":"DAY","currency":"USD","capacity":"AGENCY","price":150.25,)"
                              R"("account":"ACC1"}})";

/// The same order good till a date: the panel sends its expireTime, which the parser checks (#240, #511)
const std::string NEW_GTD_ORDER = R"({"type":"new_order","data":{"symbol":"AAPL","side":"BUY","ordType":"LIMIT",)"
                                  R"("orderQty":100,"tif":"GTD","expireTime":1792022400000,"currency":"USD",)"
                                  R"("capacity":"AGENCY","price":150.25,"account":"ACC1"}})";

/// A replace as the replace form sends one for a GTD order: a new price, quantity and expiry, each checked (#511)
const std::string REPLACE_ORDER = R"({"type":"replace_order","data":{"orderId":42,"price":150.5,"orderQty":200,)"
                                  R"("tif":"GTD","expireTime":1792022400000}})";

/// A cancel as the blotter sends one: the order's number
const std::string CANCEL_ORDER = R"({"type":"cancel_order","data":{"orderId":42}})";

/// The IO thread's parse of a message as it arrives, timed: nlohmann's parse, then the parser's reads and checks. The
/// message must be one the parser accepts, as nearly every client's is: a refusal ends the checks early.
void parseEach(benchmark::State &state, const std::string &message, const char *type)
{
    const App::ParsedClientMessage check = App::parseClientMessage(message);
    if ((type != check.type) || !check.error.empty())
    {
        state.SkipWithError(("the parser refused the message: " + check.error).c_str());
        return;
    }
    for (auto _ : state)
    {
        App::ParsedClientMessage msg = App::parseClientMessage(message);
        benchmark::DoNotOptimize(msg);
    }
    state.SetItemsProcessed(state.iterations());
}

} // namespace

/// The IO thread's parse of a client's new order. Nothing timed the parser until #516, which checks one more field
/// here.
static void BM_ParseNewOrder(benchmark::State &state)
{
    parseEach(state, NEW_ORDER, "new_order");
}
BENCHMARK(BM_ParseNewOrder);

/// A GTD order's parse, which reaches the expiry's checks BM_ParseNewOrder's DAY order skips: #511's latest time among
/// them, which went in untimed (#668)
static void BM_ParseNewGtdOrder(benchmark::State &state)
{
    parseEach(state, NEW_GTD_ORDER, "new_order");
}
BENCHMARK(BM_ParseNewGtdOrder);

/// A replace's parse, its new expiry checked as a new order's is (#511, #668)
static void BM_ParseReplaceOrder(benchmark::State &state)
{
    parseEach(state, REPLACE_ORDER, "replace_order");
}
BENCHMARK(BM_ParseReplaceOrder);

/// A cancel's parse, the lightest request a client sends
static void BM_ParseCancelOrder(benchmark::State &state)
{
    parseEach(state, CANCEL_ORDER, "cancel_order");
}
BENCHMARK(BM_ParseCancelOrder);
