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

} // namespace

/// The IO thread's parse of a client's new order, as each message arrives: nlohmann's parse, then the parser's reads
/// and checks. Nothing timed the parser until #516, which checks one more field here. The order is one the parser
/// accepts, as nearly every client's is: a refusal ends the checks early.
static void BM_ParseNewOrder(benchmark::State &state)
{
    const App::ParsedClientMessage check = App::parseClientMessage(NEW_ORDER);
    if (("new_order" != check.type) || !check.error.empty())
    {
        state.SkipWithError(("the parser refused the order: " + check.error).c_str());
        return;
    }
    for (auto _ : state)
    {
        App::ParsedClientMessage msg = App::parseClientMessage(NEW_ORDER);
        benchmark::DoNotOptimize(msg);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNewOrder);
