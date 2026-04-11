/**
 Concurrent Order Processor library — Doc-Code Consistency Tests

 Author: Oran Johnson

 Copyright (C) 2026 Oran Johnson

 Distributed under the GNU Affero General Public License (AGPL).

 ----------------------------------------------------------------------------

 This test enforces doc-code consistency for facts that are coupled to code.

 Documentation files (README.md, docs/ARCHITECTURE.md) embed values inline
 using HTML comment markers:

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
   1. Add the fact to README.md or docs/ARCHITECTURE.md wrapped in DOC_CHECK
      markers: `<!-- DOC_CHECK:my_fact -->VALUE<!-- /DOC_CHECK -->`
   2. Add an EXPECT_DOC_EQ() line below pointing to the source-of-truth.
*/

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "DataModelDef.h"
#include "TransactionScope.h"
#include "TransactionScopePool.h"

namespace {

// ----------------------------------------------------------------------------
// DOC_CHECK marker parser
// ----------------------------------------------------------------------------

using DocMap = std::map<std::string, std::string>;

DocMap parseDocCheckMarkers(const std::filesystem::path& filePath) {
    DocMap result;
    std::ifstream f(filePath);
    if (!f) {
        ADD_FAILURE() << "Could not open documentation file: " << filePath;
        return result;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string content = buf.str();

    // Match: <!-- DOC_CHECK:name -->VALUE<!-- /DOC_CHECK -->
    // Name = [A-Za-z0-9_]+, value = anything except markers (non-greedy)
    static const std::regex marker(
        R"(<!--\s*DOC_CHECK:([A-Za-z0-9_]+)\s*-->(.*?)<!--\s*/DOC_CHECK\s*-->)",
        std::regex::ECMAScript);

    auto begin = std::sregex_iterator(content.begin(), content.end(), marker);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string name = (*it)[1].str();
        const std::string value = (*it)[2].str();
        if (result.count(name)) {
            ADD_FAILURE() << "Duplicate DOC_CHECK marker '" << name
                          << "' in " << filePath;
            continue;
        }
        result[name] = value;
    }
    return result;
}

// Locate the project root by walking up from the test binary directory until
// we find README.md (this works regardless of where ctest invokes the binary).
std::filesystem::path projectRoot() {
    auto p = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(p / "README.md") &&
            std::filesystem::exists(p / "CMakeLists.txt")) {
            return p;
        }
        if (!p.has_parent_path()) break;
        p = p.parent_path();
    }
    ADD_FAILURE() << "Could not locate project root from "
                  << std::filesystem::current_path();
    return std::filesystem::current_path();
}

// ----------------------------------------------------------------------------
// Test fixture: parses README.md once and exposes the marker map
// ----------------------------------------------------------------------------

class DocConsistencyTest : public ::testing::Test {
protected:
    static DocMap docs_;

    static void SetUpTestSuite() {
        const auto root = projectRoot();
        docs_ = parseDocCheckMarkers(root / "README.md");
    }

    // Helper: assert a documented value equals an actual code value.
    template <typename T>
    void expectDocEq(const std::string& marker, T actual) {
        ASSERT_TRUE(docs_.count(marker))
            << "Missing DOC_CHECK marker '" << marker << "' in README.md. "
            << "Either add the marker to the doc or remove the assertion.";
        std::ostringstream oss;
        oss << actual;
        EXPECT_EQ(docs_[marker], oss.str())
            << "DOC_CHECK marker '" << marker << "': "
            << "doc says \"" << docs_[marker] << "\", "
            << "code says \"" << oss.str() << "\". "
            << "Update README.md or the code to make them agree.";
    }
};

DocMap DocConsistencyTest::docs_;

// ----------------------------------------------------------------------------
// Marker assertions
// ----------------------------------------------------------------------------

TEST_F(DocConsistencyTest, ParserFoundMarkers) {
    EXPECT_FALSE(docs_.empty())
        << "No DOC_CHECK markers found in README.md. The parser may be broken "
        << "or all markers may have been removed.";
}

TEST_F(DocConsistencyTest, PoolSize) {
    expectDocEq("pool_size", COP::ACID::TransactionScopePool::DEFAULT_POOL_SIZE);
}

TEST_F(DocConsistencyTest, ArenaSize) {
    expectDocEq("arena_size", COP::ACID::TransactionScope::ARENA_SIZE);
}

// Enum cardinality: each enum starts with INVALID_X = 0, so the cardinality
// (excluding INVALID) equals the integer value of the last enum entry.

TEST_F(DocConsistencyTest, SideCount) {
    expectDocEq("side_count", static_cast<int>(COP::CROSS_SIDE));
}

TEST_F(DocConsistencyTest, OrderTypeCount) {
    expectDocEq("order_type_count", static_cast<int>(COP::STOPLIMIT_ORDERTYPE));
}

TEST_F(DocConsistencyTest, TimeInForceCount) {
    expectDocEq("tif_count", static_cast<int>(COP::ATCLOSE_TIF));
}

TEST_F(DocConsistencyTest, CapacityCount) {
    expectDocEq("capacity_count", static_cast<int>(COP::AGENT_FOR_ANOTHER_MEMBER_CAPACITY));
}

TEST_F(DocConsistencyTest, OrderStatusCount) {
    expectDocEq("order_status_count", static_cast<int>(COP::CANCELED_ORDSTATUS));
}

TEST_F(DocConsistencyTest, ExecTypeCount) {
    expectDocEq("exec_type_count", static_cast<int>(COP::PEND_REPLACE_EXECTYPE));
}

}  // namespace
