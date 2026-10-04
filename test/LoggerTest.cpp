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
