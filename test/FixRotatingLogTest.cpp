/**
 Concurrent Order Processor library - FIX log rotation Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#ifdef BUILD_FIX

#include <gtest/gtest.h>
#include <quickfix/SessionSettings.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

#include "FixRotatingLog.h"
#include "TestAux.h"

using namespace COP;

namespace
{
namespace fs = std::filesystem;

const char SESSION[] = "FIX.4.4-ORDER_PROCESSOR-CLIENT_A";

class FixRotatingLogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        dir_ = test::uniqueTestPath("fixlog");
        fs::remove_all(dir_);
    }
    void TearDown() override
    {
        fs::remove_all(dir_);
    }

    /// Settings as fix.cfg gives them, logging under dir_, with extra lines in [DEFAULT]
    FIX::SessionSettings settings(const std::string &extra = "") const
    {
        std::istringstream cfg(
            "[DEFAULT]\nConnectionType=acceptor\nFileLogPath=" + dir_.string() + "\n" + extra +
            "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=ORDER_PROCESSOR\nTargetCompID=CLIENT_A\n");
        return FIX::SessionSettings(cfg);
    }

    static FIX::SessionID session()
    {
        return FIX::SessionID("FIX.4.4", "ORDER_PROCESSOR", "CLIENT_A");
    }

    static std::string read(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    fs::path dir_;
};

TEST_F(FixRotatingLogTest, ASessionsFilesAreBounded)
{
    // QuickFIX's FileLog appended for as long as the session lived: nothing it does backs the files up or clears them
    // (#160). With 1 MiB a file and two backups, 4 MiB of messages leave three files of at most 1 MiB each.
    App::FixRotatingLogFactory factory(settings("FileLogMaxMB=1\nFileLogBackups=2"));
    std::unique_ptr<FIX::Log> log(factory.create(session()));
    const std::string line(1000, 'x');
    for (int i = 0; i < 4 * 1024; ++i)
    {
        log->onIncoming(line);
    }
    log->onEvent("one event");
    log.reset();

    const fs::path base = dir_ / SESSION;
    for (const char *suffix : { ".messages.log", ".messages.1.log", ".messages.2.log" })
    {
        const fs::path file = base.string() + suffix;
        ASSERT_TRUE(fs::exists(file)) << file;
        EXPECT_LE(fs::file_size(file), 1u << 20) << file;
    }
    EXPECT_FALSE(fs::exists(base.string() + ".messages.3.log")) << "more backups than FileLogBackups";
    EXPECT_TRUE(fs::exists(base.string() + ".event.log"));
}

TEST_F(FixRotatingLogTest, LinesAreWrittenAsFileLogWritesThem)
{
    // "<UTC timestamp> : <text>", messages in and out to one file, events to the other. A message's braces are text, not
    // format syntax, and its SOH separators stay as they are.
    App::FixRotatingLogFactory factory(settings());
    std::unique_ptr<FIX::Log> log(factory.create(session()));
    log->onIncoming("8=FIX.4.4\x01"
                    "35=0\x01"
                    "58={not a format}\x01");
    log->onOutgoing("8=FIX.4.4\x01"
                    "35=1\x01");
    log->onEvent("Created session {x}");
    log.reset();

    const std::string stamp = R"(\d{8}-\d{2}:\d{2}:\d{2}\.\d{3} : )";
    const std::string messages = read(dir_ / (std::string(SESSION) + ".messages.log"));
    EXPECT_TRUE(std::regex_match(messages, std::regex(stamp +
                                                      "8=FIX\\.4\\.4\x01"
                                                      "35=0\x01"
                                                      "58=\\{not a format\\}\x01\n" +
                                                      stamp +
                                                      "8=FIX\\.4\\.4\x01"
                                                      "35=1\x01\n")))
        << messages;
    const std::string events = read(dir_ / (std::string(SESSION) + ".event.log"));
    EXPECT_TRUE(std::regex_match(events, std::regex(stamp + "Created session \\{x\\}\n"))) << events;
}

TEST_F(FixRotatingLogTest, TheLogOutsideASessionIsNamedGlobal)
{
    // As FileLogFactory names it
    App::FixRotatingLogFactory factory(settings());
    std::unique_ptr<FIX::Log> log(factory.create());
    log->onEvent("started");
    log.reset();
    EXPECT_TRUE(fs::exists(dir_ / "GLOBAL.event.log"));
}

TEST_F(FixRotatingLogTest, LimitsThatMakeNoSenseAreRefused)
{
    App::FixRotatingLogFactory zeroSize(settings("FileLogMaxMB=0"));
    EXPECT_THROW(std::unique_ptr<FIX::Log>(zeroSize.create(session())), FIX::ConfigError);
    App::FixRotatingLogFactory negativeBackups(settings("FileLogBackups=-1"));
    EXPECT_THROW(std::unique_ptr<FIX::Log>(negativeBackups.create(session())), FIX::ConfigError);
}

} // namespace

#endif // BUILD_FIX
