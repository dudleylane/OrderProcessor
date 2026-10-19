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

 It also checks that docs/ARCHITECTURE.md's file reference, section 10, names
 every source, test and benchmark file (#261).
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <iterator>
#include <regex>
#include <set>
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

// docs/FIX_TEST_SUITE.md says it lists the tests by suite and what each one checks. It missed the 13 #145 added, and
// #228's, because nothing compared it with the tests (#257). Every TEST and TEST_F in test/Fix*.cpp is named on it in
// backticks now, and so is every test/Fix*.py, the ctests that run a real server.
TEST_F(DocConsistencyTest, FixTestSuiteNamesEveryFixTest)
{
    const auto root = projectRoot();
    std::ifstream in(root / "docs/FIX_TEST_SUITE.md");
    ASSERT_TRUE(in) << "can't read docs/FIX_TEST_SUITE.md";
    const std::string page((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> named;
    // A name between backticks, and nothing else: a code fence's backticks would pair with the wrong ones
    const std::regex ticked("`([A-Za-z_][A-Za-z0-9_./-]*)`");
    for (std::sregex_iterator it(page.begin(), page.end(), ticked), end; it != end; ++it)
    {
        named.insert((*it)[1].str());
    }

    const std::regex test(R"(^\s*TEST(?:_F|_P)?\(\s*(\w+)\s*,\s*(\w+)\s*\))");
    size_t tests = 0;
    for (const auto &entry : std::filesystem::directory_iterator(root / "test"))
    {
        const std::string file = entry.path().filename().string();
        if (0 != file.rfind("Fix", 0))
        {
            continue;
        }
        if (".py" == entry.path().extension())
        {
            EXPECT_TRUE(named.count("test/" + file)) << "test/" << file << " isn't on docs/FIX_TEST_SUITE.md";
            continue;
        }
        if (".cpp" != entry.path().extension())
        {
            continue;
        }
        std::ifstream source(entry.path());
        std::string line;
        std::smatch m;
        while (std::getline(source, line))
        {
            if (std::regex_search(line, m, test))
            {
                ++tests;
                EXPECT_TRUE(named.count(m[2].str()))
                    << "test/" << file << ": " << m[1] << "." << m[2] << " isn't on docs/FIX_TEST_SUITE.md";
            }
        }
    }
    EXPECT_LT(100u, tests) << "found only " << tests << " FIX tests: the pattern or the directory is wrong";
}

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
    expectDocEq("exec_type_count", static_cast<int>(COP::TRIGGERED_EXECTYPE));
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

// docs/ARCHITECTURE.md's section 10 names the sources, tests and benchmarks one by one, and nothing compared it with
// the tree, so files went unlisted as they came (#261). Every .h, .cpp, .py and .sh under src/, test/ and bench/ is
// named there, in backticks, by its path under that directory; `X.h/cpp` names X.h and X.cpp. The frontend, under
// src/oms-frontend, and the fuzz corpus's units are left out: the section doesn't list them file by file.
TEST_F(DocConsistencyTest, FileReferenceNamesEveryFile)
{
    const auto root = projectRoot();
    std::ifstream in(root / "docs/ARCHITECTURE.md");
    ASSERT_TRUE(in) << "can't read docs/ARCHITECTURE.md";
    const std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t start = doc.find("## 10. Appendix: File Reference");
    ASSERT_NE(std::string::npos, start) << "docs/ARCHITECTURE.md has no section 10";
    const size_t end = doc.find("\n## ", start + 1);
    const std::string section = doc.substr(start, (std::string::npos == end) ? std::string::npos : end - start);

    std::set<std::string> named;
    const std::regex ticked("`([A-Za-z0-9_./-]+)`");
    const std::regex both(R"(^(.*)\.(\w+(?:/\w+)+)$)");
    for (std::sregex_iterator it(section.begin(), section.end(), ticked), last; it != last; ++it)
    {
        const std::string name = (*it)[1].str();
        named.insert(name);
        std::smatch m;
        if (std::regex_match(name, m, both))
        {
            std::istringstream extensions(m[2].str());
            std::string extension;
            while (std::getline(extensions, extension, '/'))
            {
                named.insert(m[1].str() + "." + extension);
            }
        }
    }

    size_t files = 0;
    for (const char *dir : { "src", "test", "bench" })
    {
        for (auto it = std::filesystem::recursive_directory_iterator(root / dir);
             std::filesystem::recursive_directory_iterator() != it; ++it)
        {
            const std::string path = std::filesystem::relative(it->path(), root / dir).generic_string();
            // Not walked at all: the frontend holds node_modules once it is installed
            if (it->is_directory() && (("oms-frontend" == path) || ("fuzz/corpus" == path)))
            {
                it.disable_recursion_pending();
                continue;
            }
            const std::string extension = it->path().extension().string();
            if (!it->is_regular_file() ||
                ((".h" != extension) && (".cpp" != extension) && (".py" != extension) && (".sh" != extension)))
            {
                continue;
            }
            ++files;
            EXPECT_TRUE(named.count(path)) << dir << "/" << path << " isn't in docs/ARCHITECTURE.md's section 10";
        }
    }
    EXPECT_LT(100u, files) << "found only " << files << " files: the directories or the pattern are wrong";
}

} // namespace
