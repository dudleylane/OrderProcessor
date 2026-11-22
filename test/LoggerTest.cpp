/**
 Concurrent Order Processor library - Logger Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include "Logger.h"

namespace
{

/// The logger writes exchange.log in the working directory, which every test process shares, and which can be large
const char *const LOG_FILE = "exchange.log";

std::uintmax_t logSize()
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(LOG_FILE, ec);
    return ec ? 0 : size;
}

/// A text no other line in the log has: test processes running in parallel append to the same file
std::string uniqueMark(const char *what)
{
    return std::string("logger-test-") + what + "-" + std::to_string(::getpid()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

/// Whether the log shows the text, after the offset, within the timeout. Nothing flushes or destroys the logger here:
/// this is what someone reading the file sees while the process runs.
bool reachesTheFile(const std::string &text, std::uintmax_t from,
                    std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        std::ifstream file(LOG_FILE, std::ios::binary);
        file.seekg(static_cast<std::streamoff>(from));
        const std::string appended((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (std::string::npos != appended.find(text))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

} // namespace

TEST(LoggerTest, AWarningReachesTheFileWhileTheProcessRuns)
{
    // The bug this covers: every level went to spdlog as info, so its flush on warnings never fired, and a warning
    // reached the file only when the buffer filled or the logger was destroyed at exit. A crash lost it (#65).
    const std::uintmax_t from = logSize();
    const std::string mark = uniqueMark("warn");
    aux::ExchLogger::instance()->warn(mark);
    EXPECT_TRUE(reachesTheFile(mark, from));
}

TEST(LoggerTest, AnErrorReachesTheFileWhileTheProcessRuns)
{
    const std::uintmax_t from = logSize();
    const std::string mark = uniqueMark("error");
    aux::ExchLogger::instance()->error(mark);
    EXPECT_TRUE(reachesTheFile(mark, from));
}

TEST(LoggerTest, DebugLinesAreStillWritten)
{
    // spdlog drops what is below its own level, info by default. The logger's mask decides what is written, so a debug
    // line must still get through; the warning after it flushes both.
    const bool debugWasOn = aux::ExchLogger::instance()->isDebugOn();
    aux::ExchLogger::instance()->setDebugOn(true);
    const std::uintmax_t from = logSize();
    const std::string mark = uniqueMark("debug");
    aux::ExchLogger::instance()->debug(mark);
    aux::ExchLogger::instance()->warn(uniqueMark("flush"));
    EXPECT_TRUE(reachesTheFile(mark, from));
    aux::ExchLogger::instance()->setDebugOn(debugWasOn);
}

TEST(LoggerTest, TheRotationTakesItsDefaultsUnlessGivenAWholeNumberInRange)
{
    // EXCHANGE_LOG_MAX_MB and EXCHANGE_LOG_FILES (#569): unset, the defaults, which delete the oldest files
    aux::LogRotation rotation = aux::logRotationFrom(nullptr, nullptr);
    EXPECT_EQ(aux::LogRotation::DEFAULT_MAX_MB, rotation.maxMb);
    EXPECT_EQ(aux::LogRotation::DEFAULT_FILES, rotation.files);
    EXPECT_TRUE(rotation.warnings.empty());

    rotation = aux::logRotationFrom("64", "2");
    EXPECT_EQ(64u, rotation.maxMb);
    EXPECT_EQ(2u, rotation.files);
    EXPECT_TRUE(rotation.warnings.empty());

    // 0 keeps one file that grows without bound, as before; the largest values in range are taken
    rotation = aux::logRotationFrom("0", "1000");
    EXPECT_EQ(0u, rotation.maxMb);
    EXPECT_EQ(1000u, rotation.files);
    EXPECT_TRUE(rotation.warnings.empty());
    EXPECT_EQ(aux::LogRotation::MAX_MAX_MB, aux::logRotationFrom("1048576", nullptr).maxMb);

    // Anything else takes the default, and says so, naming the variable and the value
    for (const char *bad : { "", "-1", "+5", " 5", "5 ", "abc", "1.5", "1048577", "99999999999999999999999" })
    {
        rotation = aux::logRotationFrom(bad, nullptr);
        EXPECT_EQ(aux::LogRotation::DEFAULT_MAX_MB, rotation.maxMb) << "'" << bad << "'";
        ASSERT_EQ(1u, rotation.warnings.size()) << "'" << bad << "'";
        EXPECT_NE(std::string::npos, rotation.warnings[0].find("EXCHANGE_LOG_MAX_MB is '" + std::string(bad) + "'"))
            << rotation.warnings[0];
    }
    for (const char *bad : { "", "0", "-2", "x", "1001" })
    {
        rotation = aux::logRotationFrom(nullptr, bad);
        EXPECT_EQ(aux::LogRotation::DEFAULT_FILES, rotation.files) << "'" << bad << "'";
        ASSERT_EQ(1u, rotation.warnings.size()) << "'" << bad << "'";
        EXPECT_NE(std::string::npos, rotation.warnings[0].find("EXCHANGE_LOG_FILES is '" + std::string(bad) + "'"))
            << rotation.warnings[0];
    }
    // Each variable on its own: a bad one leaves the other as given
    rotation = aux::logRotationFrom("x", "3");
    EXPECT_EQ(aux::LogRotation::DEFAULT_MAX_MB, rotation.maxMb);
    EXPECT_EQ(3u, rotation.files);
    EXPECT_EQ(1u, rotation.warnings.size());
}
