/**
 Concurrent Order Processor library - fuzz target for parseClientMessage()

 Contract under test: parseClientMessage() returns for every input, never
 throws and never crashes. WsSession::handleMessage() calls it from a Beast
 read handler with no try/catch, so an escaping exception would terminate the
 server on a single bad client message.

 Built two ways (see test/fuzz/CMakeLists.txt):
   - Clang: linked with -fsanitize=fuzzer for a coverage-guided campaign.
   - Any compiler: linked with standalone_main.cpp, which replays the files
     under test/fuzz/corpus/ so the same contract runs as a regression test.

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "ClientMessageParser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    try
    {
        const auto msg = COP::App::parseClientMessage(std::string(reinterpret_cast<const char *>(data), size));

        // Keep the result observable so the call cannot be optimised away.
        volatile size_t sink = msg.type.size() + msg.symbol.size() + msg.newOrder.symbol.size() +
                               msg.newOrder.account.size() + msg.cancelOrder.clOrderId.size();
        (void)sink;
    }
    catch (...)
    {
        // Anything escaping the parser is a server-killing bug; make it a crash the
        // fuzzer records instead of a silently swallowed exception.
        std::abort();
    }
    return 0;
}
