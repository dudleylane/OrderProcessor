/**
 Concurrent Order Processor library - FIX session stores Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#ifdef BUILD_FIX

#include <gtest/gtest.h>
#include <quickfix/Application.h>
#include <quickfix/DataDictionaryProvider.h>
#include <quickfix/FieldConvertors.h>
#include <quickfix/FileStore.h>
#include <quickfix/FixFields.h>
#include <quickfix/FixValues.h>
#include <quickfix/Responder.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/TimeRange.h>
#include <quickfix/fix50sp2/BusinessMessageReject.h>
#include <quickfix/fix50sp2/ExecutionReport.h>
#include <quickfix/fixt11/Heartbeat.h>
#include <quickfix/fixt11/Logon.h>
#include <quickfix/fixt11/Reject.h>
#include <quickfix/fixt11/ResendRequest.h>
#include <quickfix/fixt11/SequenceReset.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "FixServer.h"
#include "FixStores.h"
#include "TestAux.h"

using namespace COP;
using namespace COP::App;

namespace
{
namespace fs = std::filesystem;

/// day October 2026 at hour:minute:second.millisecond in New York, which keeps daylight time all month, four hours
/// behind UTC
FIX::UtcTimeStamp newYork(int day, int hour, int minute, int second = 0, int millisecond = 0)
{
    FIX::UtcTimeStamp time(hour, minute, second, millisecond, day, 10, 2026);
    time += 4 * 60 * 60;
    return time;
}

/// The application of a session made here: it takes everything
class Counterparty final : public FIX::NullApplication
{
};

/// CLIENT_A's session with config/fix.cfg's settings, its day 17:00 to 16:59:59 New York, made as the server makes it
/// but on a clock of the test's own, so that a test can step it across the day's end. Its store is the server's,
/// FixCarryingStore over FileStore, in a directory of its own (#513).
class FixStoresTest : public ::testing::Test, public FIX::Responder
{
protected:
    void SetUp() override
    {
        useBusinessDayLocalTime(); // New York's, as the server's
        dir_ = test::uniqueTestPath("fixstores");
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        const FIX::SessionSettings cfg((test::sourceRoot() / "config" / "fix.cfg").string());
        for (const auto &[key, value] : cfg.get())
        {
            settings_.setString(key, value);
        }
        for (const char *dictionary : { FIX::TRANSPORT_DATA_DICTIONARY, FIX::APP_DATA_DICTIONARY })
        {
            // The file of its name in this tree's config/spec, as FixtSessionTest finds it
            const fs::path file = fs::path(settings_.getString(dictionary)).filename();
            settings_.setString(dictionary, (test::sourceRoot() / "config" / "spec" / file).string());
        }
        settings_.setString(FIX::FILE_STORE_PATH, dir_.string());
        sid_ =
            FIX::SessionID(settings_.getString(FIX::BEGINSTRING), settings_.getString(FIX::SENDERCOMPID), "CLIENT_A");
        provider_.addTransportDataDictionary(
            sid_.getBeginString(),
            std::make_shared<FIX::DataDictionary>(settings_.getString(FIX::TRANSPORT_DATA_DICTIONARY)));
        provider_.addApplicationDataDictionary(
            FIX::ApplVerID(FIX::ApplVerID_FIX50_SP2),
            std::make_shared<FIX::DataDictionary>(settings_.getString(FIX::APP_DATA_DICTIONARY)));
    }

    void TearDown() override
    {
        stop();
        fs::remove_all(dir_);
    }

    /// The session's day, as QuickFIX's SessionFactory makes it from the settings, and as FixSessionTimeTest's
    /// ASessionsDayEndsAt1700NewYork does
    FIX::TimeRange sessionTime() const
    {
        const FIX::UtcTimeOnly start = FIX::UtcTimeOnlyConvertor::convert(settings_.getString(FIX::START_TIME));
        const FIX::UtcTimeOnly end = FIX::UtcTimeOnlyConvertor::convert(settings_.getString(FIX::END_TIME));
        return FIX::TimeRange(FIX::LocalTimeOnly(start.getHour(), start.getMinute(), start.getSecond()),
                              FIX::LocalTimeOnly(end.getHour(), end.getMinute(), end.getSecond()));
    }

    /// The server starts at the time given, with the session and its store
    void start(const FIX::UtcTimeStamp &at)
    {
        now_ = at;
        FIX::SessionSettings sessions;
        sessions.set(sid_, settings_);
        stores_ = std::make_unique<FixCarryingStoreFactory>(sessions, std::make_unique<FIX::FileStoreFactory>(sessions),
                                                            true);
        // An acceptor's heartbeat interval is 0 until the Logon gives one, as SessionFactory makes it
        session_ = new FIX::Session(
            [this]()
            {
                return now_;
            },
            counterparty_, *stores_, sid_, provider_, sessionTime(), 0, nullptr);
        session_->setSenderDefaultApplVerID(FIX::ApplVerID_FIX50_SP2);
        stores_->attach(sid_, session_); // as FixGateway::onCreate() does
    }

    /// The server stops
    void stop()
    {
        delete session_;
        session_ = nullptr;
        stores_.reset();
    }

    /// CLIENT_A connects, at now_
    void connect()
    {
        sent_.clear();
        disconnected_ = false;
        session_->setResponder(this);
    }

    // FIX::Responder: the connection to CLIENT_A, which gets everything it is sent
    bool send(const std::string &raw) override
    {
        sent_.push_back(raw);
        const FIX::SEQNUM number = parsed(raw).getHeader().getField<FIX::MsgSeqNum>();
        expectedSeqNum_ = std::max(expectedSeqNum_, number + 1);
        return true;
    }
    void disconnect() override
    {
        disconnected_ = true;
    }

    /// Hands the session msg as CLIENT_A sends it at now_, with its next MsgSeqNum
    void receive(FIX::Message msg)
    {
        FIX::Header &header = msg.getHeader();
        header.setField(FIX::BeginString(sid_.getBeginString().getValue()));
        header.setField(FIX::SenderCompID(sid_.getTargetCompID().getValue()));
        header.setField(FIX::TargetCompID(sid_.getSenderCompID().getValue()));
        header.setField(FIX::MsgSeqNum(clientSeqNum_++));
        header.setField(FIX::SendingTime(now_));
        session_->next(msg.toString(), now_);
    }

    /// CLIENT_A logs on, asking to start again from 1 if resetSeqNum, and asks for what it hasn't received when the
    /// session's Logon is numbered past what it expects, as a counterparty does
    void logOn(bool resetSeqNum = false)
    {
        const FIX::SEQNUM expected = resetSeqNum ? 1 : expectedSeqNum_;
        FIXT11::Logon logon(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER), FIX::HeartBtInt(30),
                            FIX::DefaultApplVerID(FIX::ApplVerID_FIX50_SP2));
        if (resetSeqNum)
        {
            logon.set(FIX::ResetSeqNumFlag(true));
            clientSeqNum_ = 1;
        }
        receive(logon);
        ASSERT_TRUE(session_->isLoggedOn()) << sentText();
        const std::vector<FIX::Message> answer = sentOfType(FIX::MsgType_Logon);
        ASSERT_EQ(1u, answer.size()) << sentText();
        if (expected < answer.front().getHeader().getField<FIX::MsgSeqNum>())
        {
            receive(FIXT11::ResendRequest(FIX::BeginSeqNo(expected), FIX::EndSeqNo(0)));
        }
    }

    /// The session sends CLIENT_A a report, whose ExecID is execId, at now_
    void report(const std::string &execId)
    {
        FIX50SP2::ExecutionReport report(FIX::OrderID("ORDER-1"), FIX::ExecID(execId), FIX::ExecType(FIX::ExecType_NEW),
                                         FIX::OrdStatus(FIX::OrdStatus_NEW), FIX::Side(FIX::Side_BUY),
                                         FIX::LeavesQty(100), FIX::CumQty(0));
        report.set(FIX::ClOrdID("CL-" + execId));
        session_->send(report);
    }

    /// A message the session sent, as a counterparty parses it, checking its length and checksum
    FIX::Message parsed(const std::string &raw) const
    {
        return FIX::Message(raw, provider_.getSessionDataDictionary(sid_.getBeginString()),
                            provider_.getApplicationDataDictionary(FIX::ApplVerID(FIX::ApplVerID_FIX50_SP2)), true);
    }

    /// What the session sent of MsgType type, parsed
    std::vector<FIX::Message> sentOfType(const std::string &type) const
    {
        std::vector<FIX::Message> found;
        for (const std::string &raw : sent_)
        {
            FIX::Message msg = parsed(raw);
            if (type == msg.getHeader().getField(FIX::FIELD::MsgType))
            {
                found.push_back(msg);
            }
        }
        return found;
    }

    /// The reports the session sent, in order: each one's ExecID and MsgSeqNum, with " 43=Y" when it was sent again
    /// (PossDupFlag, with its OrigSendingTime), and " 97=Y" when it may have been sent under another MsgSeqNum
    /// (PossResend)
    std::vector<std::string> reportsSent() const
    {
        std::vector<std::string> reports;
        for (const FIX::Message &msg : sentOfType(FIX::MsgType_ExecutionReport))
        {
            const FIX::Header &header = msg.getHeader();
            std::string report =
                msg.getField(FIX::FIELD::ExecID) + " " + std::to_string(header.getField<FIX::MsgSeqNum>().getValue());
            if (header.isSetField(FIX::FIELD::PossDupFlag) && header.getField<FIX::PossDupFlag>() &&
                header.isSetField(FIX::FIELD::OrigSendingTime))
            {
                report += " 43=Y";
            }
            if (header.isSetField(FIX::FIELD::PossResend) && header.getField<FIX::PossResend>())
            {
                report += " 97=Y";
            }
            reports.push_back(report);
        }
        return reports;
    }

    /// Every message the session sent, one a line with '|' for SOH, for a failure to show
    std::string sentText() const
    {
        std::string text;
        for (std::string raw : sent_)
        {
            std::replace(raw.begin(), raw.end(), '\x01', '|');
            text += raw + "\n";
        }
        return text;
    }

    FixCarryingStore *store()
    {
        return stores_->storeOf(sid_);
    }

    fs::path markFile() const
    {
        return dir_ / "FIXT.1.1-ORDER_PROCESSOR-CLIENT_A.unsent";
    }

    /// CLIENT_A's session day ends too, and it starts again from 1, as the session does
    void clientDayEnds()
    {
        clientSeqNum_ = 1;
        expectedSeqNum_ = 1;
    }

    fs::path dir_;
    FIX::Dictionary settings_; // config/fix.cfg's, with this tree's dictionaries, storing in dir_
    FIX::SessionID sid_;
    FIX::DataDictionaryProvider provider_;
    Counterparty counterparty_;
    std::unique_ptr<FixCarryingStoreFactory> stores_;
    FIX::Session *session_ = nullptr;
    FIX::UtcTimeStamp now_ = FIX::UtcTimeStamp::now(); // the clock of the session, which start() sets
    std::vector<std::string> sent_;
    bool disconnected_ = false;
    FIX::SEQNUM clientSeqNum_ = 1;   // the next MsgSeqNum CLIENT_A sends
    FIX::SEQNUM expectedSeqNum_ = 1; // the next MsgSeqNum CLIENT_A expects of the session
};

TEST_F(FixStoresTest, AReportItsClientDidntGetIsResentOnTheNextSessionDay)
{
    // CLIENT_A is logged on in the morning, and gets a report
    start(newYork(15, 9, 0));
    connect();
    logOn();
    report("E1");
    EXPECT_EQ((std::vector<std::string>{ "E1 2" }), reportsSent());
    EXPECT_EQ(0u, store()->unsentFrom());
    // Its connection goes, and two reports are stored for it, 3 and 4, not sent
    session_->disconnect();
    now_ = newYork(15, 10, 0);
    report("E2");
    report("E3");
    EXPECT_EQ(3u, store()->unsentFrom());
    EXPECT_TRUE(fs::exists(markFile()));

    // It connects the next morning: QuickFIX begins the session day that began at 17:00 as it does, and the two go
    // into it first, as 1 and 2, and the session's Logon is 3
    now_ = newYork(16, 8, 0);
    clientDayEnds();
    connect();
    EXPECT_EQ(1u, store()->unsentFrom());
    EXPECT_EQ(3u, session_->getExpectedSenderNum());
    logOn();
    EXPECT_EQ((std::vector<std::string>{ "E2 1 43=Y 97=Y", "E3 2 43=Y 97=Y" }), reportsSent()) << sentText();
    const std::vector<FIX::Message> resent = sentOfType(FIX::MsgType_ExecutionReport);
    ASSERT_EQ(2u, resent.size());
    EXPECT_EQ("CL-E2", resent[0].getField(FIX::FIELD::ClOrdID));
    // It has them, and the next day carries nothing
    EXPECT_EQ(0u, store()->unsentFrom());
    EXPECT_FALSE(fs::exists(markFile()));
    session_->disconnect();
    now_ = newYork(17, 8, 0);
    clientDayEnds();
    connect();
    EXPECT_EQ(1u, session_->getExpectedSenderNum());
}

TEST_F(FixStoresTest, AServerThatStartsAfterTheSessionDayEndedCarriesThemToo)
{
    start(newYork(15, 9, 0));
    connect();
    logOn();
    report("E1");
    session_->disconnect();
    now_ = newYork(15, 10, 0);
    report("E2");
    report("E3");
    // A restart within the day keeps the mark, from its file
    stop();
    start(newYork(15, 11, 0));
    EXPECT_EQ(3u, store()->unsentFrom());
    stop();

    // The server starts the next morning, before CLIENT_A connects: QuickFIX begins the new session day as it makes the
    // session, and the two go into it
    start(newYork(16, 8, 0));
    EXPECT_EQ(1u, store()->unsentFrom());
    EXPECT_EQ(3u, session_->getExpectedSenderNum());
    clientDayEnds();
    connect();
    logOn();
    EXPECT_EQ((std::vector<std::string>{ "E2 1 43=Y 97=Y", "E3 2 43=Y 97=Y" }), reportsSent()) << sentText();
}

TEST_F(FixStoresTest, AClientLoggedOnAsTheDayEndsGetsWhatWasSentBeforeItLoggedOnAgain)
{
    start(newYork(15, 9, 0));
    connect();
    logOn();
    report("E1");
    // QuickFIX ends the session day at its first check in 16:59:59, and logs CLIENT_A out: its arithmetic makes that
    // second a day of its own, which ends at 17:00
    now_ = newYork(15, 16, 59, 59, 200);
    session_->next(now_);
    EXPECT_FALSE(session_->isLoggedOn());
    EXPECT_TRUE(disconnected_);
    EXPECT_EQ(0u, store()->unsentFrom()); // E1 reached it

    // The business day's first report, at 17:00, is stored for it, not sent: a restatement, say (#244)
    now_ = newYork(15, 17, 0, 0, 500);
    report("E2");
    EXPECT_EQ(1u, store()->unsentFrom());
    // It connects ten seconds later, and QuickFIX begins the day again, the store it began at 16:59:59 not being
    // 17:00's: E2 goes into it
    now_ = newYork(15, 17, 0, 10);
    clientDayEnds();
    connect();
    EXPECT_EQ(2u, session_->getExpectedSenderNum());
    logOn();
    EXPECT_EQ((std::vector<std::string>{ "E2 1 43=Y 97=Y" }), reportsSent()) << sentText();
}

TEST_F(FixStoresTest, ALogonThatAsksToStartAgainFrom1GetsNothingCarried)
{
    start(newYork(15, 9, 0));
    connect();
    logOn();
    session_->disconnect();
    now_ = newYork(15, 10, 0);
    report("E2");
    report("E3");
    now_ = newYork(16, 8, 0);
    clientDayEnds();
    connect();
    EXPECT_EQ(1u, store()->unsentFrom());
    // ResetSeqNumFlag (141) Y: both sides start again from 1, and what the store held goes, as within a day
    logOn(true);
    EXPECT_TRUE(reportsSent().empty()) << sentText();
    EXPECT_EQ(0u, store()->unsentFrom());
    EXPECT_EQ(2u, session_->getExpectedSenderNum()); // its Logon, 1
    EXPECT_FALSE(fs::exists(markFile()));
}

TEST_F(FixStoresTest, AReportResentWithinTheDayIsntCarried)
{
    start(newYork(15, 9, 0));
    connect();
    logOn();
    report("E1");
    session_->disconnect();
    now_ = newYork(15, 10, 0);
    report("E2");
    report("E3");
    // CLIENT_A comes back within the day, and asks for them
    now_ = newYork(15, 11, 0);
    connect();
    logOn();
    EXPECT_EQ((std::vector<std::string>{ "E2 3 43=Y", "E3 4 43=Y" }), reportsSent()) << sentText();
    EXPECT_EQ(0u, store()->unsentFrom());
    EXPECT_FALSE(fs::exists(markFile()));
    // So the next day carries nothing
    session_->disconnect();
    now_ = newYork(16, 8, 0);
    clientDayEnds();
    connect();
    EXPECT_EQ(1u, session_->getExpectedSenderNum());
}

TEST_F(FixStoresTest, ASessionThatNeverEndsCarriesNothing)
{
    settings_.setBool(FIX::NON_STOP_SESSION, true);
    EXPECT_FALSE(FixCarryingStoreFactory::sessionTime(settings_).has_value());
    // Its store is never reset for a new day, and one reset otherwise carries nothing
    start(newYork(15, 9, 0));
    session_->setIsNonStopSession(true);
    report("E1");
    EXPECT_EQ(1u, store()->unsentFrom());
    store()->reset(newYork(16, 8, 0));
    EXPECT_EQ(0u, store()->unsentFrom());
    EXPECT_EQ(1u, store()->getNextSenderMsgSeqNum());
}

TEST_F(FixStoresTest, TheSessionTimeIsTheSessionFactorys)
{
    // As QuickFIX's SessionFactory reads config/fix.cfg's: 17:00 to 16:59:59 New York, its local time
    std::optional<FIX::TimeRange> time = FixCarryingStoreFactory::sessionTime(settings_);
    ASSERT_TRUE(time.has_value());
    FIX::TimeRange expected = sessionTime();
    const FIX::UtcTimeStamp checks[][2] = {
        { newYork(15, 17, 0, 1), newYork(15, 16, 59, 58) }, { newYork(16, 16, 59, 0), newYork(15, 17, 0, 1) },
        { newYork(16, 0, 1), newYork(15, 23, 59) },         { newYork(16, 16, 59, 59), newYork(15, 17, 0, 0) },
        { newYork(15, 17, 0, 0), newYork(15, 16, 59, 59) },
    };
    for (const auto &[now, created] : checks)
    {
        EXPECT_EQ(expected.isInSameRange(now, created), time->isInSameRange(now, created))
            << FIX::UtcTimeStampConvertor::convert(now) << " " << FIX::UtcTimeStampConvertor::convert(created);
    }
    EXPECT_FALSE(time->isInSameRange(newYork(15, 17, 0, 1), newYork(15, 16, 59, 58)));
    EXPECT_TRUE(time->isInSameRange(newYork(16, 16, 59, 0), newYork(15, 17, 0, 1)));
}

TEST(FixCarryingStoreTest, ARenumberedMessageIsWholeAndMayHaveBeenSentBefore)
{
    const fs::path spec = test::sourceRoot() / "config" / "spec";
    const FIX::DataDictionary transport((spec / "FIXT11.xml").string());
    const FIX::DataDictionary application((spec / "FIX50SP2.xml").string());
    FIX50SP2::ExecutionReport report(FIX::OrderID("ORDER-1"), FIX::ExecID("E1"), FIX::ExecType(FIX::ExecType_TRADE),
                                     FIX::OrdStatus(FIX::OrdStatus_FILLED), FIX::Side(FIX::Side_SELL),
                                     FIX::LeavesQty(0), FIX::CumQty(100));
    report.set(FIX::ClOrdID("CL-1"));
    report.set(FIX::LastPx(1.2650));
    FIX::Header &header = report.getHeader();
    header.setField(FIX::BeginString(FIX::BeginString_FIXT11));
    header.setField(FIX::SenderCompID("ORDER_PROCESSOR"));
    header.setField(FIX::TargetCompID("CLIENT_A"));
    header.setField(FIX::MsgSeqNum(4711));
    header.setField(FIX::SendingTime(newYork(15, 10, 0)));
    const std::string stored = report.toString();

    const std::optional<std::string> renumbered = FixCarryingStore::restamped(stored, 3);
    ASSERT_TRUE(renumbered.has_value());
    // As QuickFIX reads it back to resend it: its BodyLength and CheckSum checked
    const FIX::Message carried(*renumbered, transport, application, true);
    EXPECT_EQ(3, carried.getHeader().getField<FIX::MsgSeqNum>());
    EXPECT_TRUE(carried.getHeader().getField<FIX::PossResend>());
    EXPECT_EQ("E1", carried.getField(FIX::FIELD::ExecID));
    EXPECT_EQ("CL-1", carried.getField(FIX::FIELD::ClOrdID));
    // The body as it was: from ClOrdID (11), its first field, to CheckSum
    auto body = [](const std::string &message)
    {
        const std::size_t begin = message.find("\00111=");
        return message.substr(begin, message.rfind("\00110=") - begin);
    };
    EXPECT_EQ(body(stored), body(*renumbered));
    // Carried again, from a day it was carried into: still one PossResend
    const std::optional<std::string> again = FixCarryingStore::restamped(*renumbered, 1);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(1, FIX::Message(*again, transport, application, true).getHeader().getField<FIX::MsgSeqNum>());
    EXPECT_EQ(std::string::npos, again->find("\00197=", again->find("\00197=") + 1));

    // Not a whole message: none
    EXPECT_FALSE(FixCarryingStore::restamped(stored.substr(0, stored.size() - 8), 3).has_value());
    std::string unnumbered = stored;
    unnumbered.erase(unnumbered.find("\00134="), std::string("\00134=4711").size());
    EXPECT_FALSE(FixCarryingStore::restamped(unnumbered, 3).has_value());
}

TEST(FixCarryingStoreTest, OnlyAnApplicationMessageIsCarried)
{
    auto stored = [](FIX::Message msg)
    {
        FIX::Header &header = msg.getHeader();
        header.setField(FIX::BeginString(FIX::BeginString_FIXT11));
        header.setField(FIX::SenderCompID("ORDER_PROCESSOR"));
        header.setField(FIX::TargetCompID("CLIENT_A"));
        header.setField(FIX::MsgSeqNum(1));
        return msg.toString();
    };
    EXPECT_TRUE(FixCarryingStore::isApplication(stored(FIX50SP2::ExecutionReport())));
    EXPECT_TRUE(FixCarryingStore::isApplication(stored(FIX50SP2::BusinessMessageReject())));
    EXPECT_FALSE(FixCarryingStore::isApplication(stored(FIXT11::Logon())));
    EXPECT_FALSE(FixCarryingStore::isApplication(stored(FIXT11::Heartbeat())));
    EXPECT_FALSE(FixCarryingStore::isApplication(stored(FIXT11::Reject())));
    EXPECT_FALSE(FixCarryingStore::isApplication(stored(FIXT11::ResendRequest())));
    EXPECT_FALSE(FixCarryingStore::isApplication(stored(FIXT11::SequenceReset())));
    EXPECT_FALSE(FixCarryingStore::isApplication("8=FIXT.1.1\0019=5\001"));
}

} // namespace

#endif // BUILD_FIX
