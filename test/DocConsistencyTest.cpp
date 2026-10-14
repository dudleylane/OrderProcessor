/**
 Concurrent Order Processor library — Doc-Code Consistency Tests

 Author: Oran Johnson

 Copyright (C) 2026 Oran Johnson

 Distributed under the GNU Affero General Public License (AGPL).

 ----------------------------------------------------------------------------

 This test enforces doc-code consistency for facts that are coupled to code.

 Documentation files (README.md, docs/ARCHITECTURE.md, docker/README.md) embed
 values inline using HTML comment markers:

     ...the pool has <!-- DOC_CHECK:pool_size -->1024<!-- /DOC_CHECK --> slots...

 The parser extracts each (name, value) pair, and the test below asserts that
 every documented value matches the corresponding C++ source-of-truth.

 When this test fails:
   1. Read the failure message — it names the marker, expected value (from
      doc), and actual value (from code).
   2. Decide which is correct (the code change or the doc value).
   3. Update whichever is wrong. Both must agree before the test passes.
   4. Do NOT delete or bypass the DOC_CHECK marker to "fix" the test.

 When adding a new code-coupled fact:
   1. Add the fact to README.md, docs/ARCHITECTURE.md or docker/README.md wrapped
      in DOC_CHECK markers: `<!-- DOC_CHECK:my_fact -->VALUE<!-- /DOC_CHECK -->`
   2. Add an EXPECT_DOC_EQ() line below pointing to the source-of-truth.
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <boost/mpl/size.hpp>

#include "DataModelDef.h"
#include "ServerDefaults.h"
#include "StateMachine.h"
#include "StateMachineDef.h"
#include "TransactionScope.h"
#include "TransactionScopePool.h"
#include "WsAuth.h"

namespace
{

// ----------------------------------------------------------------------------
// DOC_CHECK marker parser
// ----------------------------------------------------------------------------

// A marker's value in one documentation file. A fact may be marked in more than one file, and each is checked.
struct DocValue
{
    std::string file;
    std::string value;
};
using DocMap = std::map<std::string, std::vector<DocValue>>;

// Adds the markers in root/file to result
void parseDocCheckMarkers(const std::filesystem::path &root, const std::string &file, DocMap &result)
{
    std::ifstream f(root / file);
    if (!f)
    {
        ADD_FAILURE() << "Could not open documentation file: " << root / file;
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string content = buf.str();

    // Match: <!-- DOC_CHECK:name -->VALUE<!-- /DOC_CHECK -->
    // Name = [A-Za-z0-9_]+, value = anything except markers (non-greedy)
    static const std::regex marker(R"(<!--\s*DOC_CHECK:([A-Za-z0-9_]+)\s*-->(.*?)<!--\s*/DOC_CHECK\s*-->)",
                                   std::regex::ECMAScript);

    auto begin = std::sregex_iterator(content.begin(), content.end(), marker);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it)
    {
        const std::string name = (*it)[1].str();
        const std::string value = (*it)[2].str();
        auto &values = result[name];
        if (std::any_of(values.begin(), values.end(),
                        [&](const DocValue &v)
                        {
                            return v.file == file;
                        }))
        {
            ADD_FAILURE() << "Duplicate DOC_CHECK marker '" << name << "' in " << file;
            continue;
        }
        values.push_back({ file, value });
    }
}

// Locate the project root by walking up from the test binary directory until
// we find README.md (this works regardless of where ctest invokes the binary).
std::filesystem::path projectRoot()
{
    auto p = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(p / "README.md") && std::filesystem::exists(p / "CMakeLists.txt"))
        {
            return p;
        }
        if (!p.has_parent_path())
        {
            break;
        }
        p = p.parent_path();
    }
    ADD_FAILURE() << "Could not locate project root from " << std::filesystem::current_path();
    return std::filesystem::current_path();
}

// ----------------------------------------------------------------------------
// Test fixture: parses README.md, docs/ARCHITECTURE.md and docker/README.md once and exposes the marker map
// ----------------------------------------------------------------------------

class DocConsistencyTest : public ::testing::Test
{
protected:
    static DocMap docs_;

    static void SetUpTestSuite()
    {
        const auto root = projectRoot();
        parseDocCheckMarkers(root, "README.md", docs_);
        parseDocCheckMarkers(root, "docs/ARCHITECTURE.md", docs_);
        parseDocCheckMarkers(root, "docker/README.md", docs_);
    }

    // Helper: assert that every documented value of a marker equals an actual code value.
    template <typename T> void expectDocEq(const std::string &marker, T actual)
    {
        ASSERT_TRUE(docs_.count(marker)) << "Missing DOC_CHECK marker '" << marker
                                         << "' in README.md, docs/ARCHITECTURE.md and docker/README.md. "
                                         << "Either add the marker to the doc or remove the assertion.";
        std::ostringstream oss;
        oss << actual;
        for (const DocValue &doc : docs_[marker])
        {
            EXPECT_EQ(doc.value, oss.str()) << "DOC_CHECK marker '" << marker << "' in " << doc.file << ": "
                                            << "doc says \"" << doc.value << "\", "
                                            << "code says \"" << oss.str() << "\". "
                                            << "Update " << doc.file << " or the code to make them agree.";
        }
    }
};

DocMap DocConsistencyTest::docs_;

// ----------------------------------------------------------------------------
// Marker assertions
// ----------------------------------------------------------------------------

TEST_F(DocConsistencyTest, ParserFoundMarkers)
{
    EXPECT_FALSE(docs_.empty()) << "No DOC_CHECK markers found in README.md or docs/ARCHITECTURE.md. The parser may "
                                << "be broken or all markers may have been removed.";
}

// The order state machine's size. Both README.md and docs/ARCHITECTURE.md give it, and both had drifted (#171).
TEST_F(DocConsistencyTest, OrderStateCount)
{
    expectDocEq("order_state_count", COP::OrdState::ORDER_STATE_COUNT);
}

TEST_F(DocConsistencyTest, TransitionCount)
{
    expectDocEq("transition_count", boost::mpl::size<COP::OrdState::OrderState::stt>::value);
}

TEST_F(DocConsistencyTest, PoolSize)
{
    expectDocEq("pool_size", COP::ACID::TransactionScopePool::DEFAULT_POOL_SIZE);
}

TEST_F(DocConsistencyTest, ArenaSize)
{
    expectDocEq("arena_size", COP::ACID::TransactionScope::ARENA_SIZE);
}

// Enum cardinality: each enum starts with INVALID_X = 0, so the cardinality
// (excluding INVALID) equals the integer value of the last enum entry.

TEST_F(DocConsistencyTest, SideCount)
{
    expectDocEq("side_count", static_cast<int>(COP::CROSS_SIDE));
}

TEST_F(DocConsistencyTest, OrderTypeCount)
{
    expectDocEq("order_type_count", static_cast<int>(COP::FXSWAP_ORDERTYPE));
}

TEST_F(DocConsistencyTest, TimeInForceCount)
{
    expectDocEq("tif_count", static_cast<int>(COP::ATCLOSE_TIF));
}

TEST_F(DocConsistencyTest, CapacityCount)
{
    expectDocEq("capacity_count", static_cast<int>(COP::AGENT_FOR_ANOTHER_MEMBER_CAPACITY));
}

TEST_F(DocConsistencyTest, OrderStatusCount)
{
    expectDocEq("order_status_count", static_cast<int>(COP::CANCELED_ORDSTATUS));
}

TEST_F(DocConsistencyTest, ExecTypeCount)
{
    expectDocEq("exec_type_count", static_cast<int>(COP::PEND_REPLACE_EXECTYPE));
}

// What WebSocket clients may hold of the server (#159)
TEST_F(DocConsistencyTest, WebSocketLimits)
{
    expectDocEq("ws_max_connections", COP::App::DEFAULT_MAX_CONNECTIONS);
    expectDocEq("ws_max_session_queue_mb", COP::App::DEFAULT_MAX_SESSION_QUEUE_MB);
    expectDocEq("ws_max_client_message_kib", COP::App::MAX_CLIENT_MESSAGE_BYTES / 1024);
}

TEST_F(DocConsistencyTest, WebSocketAccessToken)
{
    expectDocEq("ws_min_token_length", COP::App::WS_MIN_TOKEN_LENGTH);
}

} // namespace
