#pragma once

#ifdef BUILD_FIX

#include <quickfix/MessageStore.h>
#include <quickfix/SessionID.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/TimeRange.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace FIX
{
class Session;
}

namespace COP::App
{

class FixGateway;

/// A FIX session's store, which keeps for the next session day what its client hasn't received (#513).
///
/// QuickFIX starts each session day, at config/fix.cfg's StartTime, from MsgSeqNum 1 with an empty store, and a
/// ResendRequest gets back only what the store holds. A message sent while its client wasn't logged on was only
/// stored, not sent, so one stored before the day ended went with the store when QuickFIX reset it for the next day:
/// as the client came back, or as the server started, whichever was first.
///
/// This store keeps the mark: the first application message stored while the session's client wasn't logged on, until
/// a ResendRequest reaches it. When QuickFIX resets the store for a new session day, the application messages from the
/// mark on go into the new day's store first, numbered from 1, each with PossResend (97) Y, and the session numbers
/// what it sends next after them, so the client's ResendRequest at its logon gets them. They keep the mark, at 1, until
/// one does. A reset within the day carries nothing: a Logon with ResetSeqNumFlag (141) Y asks to start again from 1,
/// and does, as it always has. A message QuickFIX sent while the client was logged on isn't carried, even if the
/// connection had died before it arrived: within the day a ResendRequest gets it back.
///
/// QuickFIX calls set(), get() and reset() under its session state's lock, so the mark needs no lock of its own.
class FixCarryingStore final : public FIX::MessageStore
{
public:
    /// inner: where the messages are kept, which FixCarryingStoreFactory gives back to its own factory. sessionTime:
    /// the session's, as QuickFIX's SessionFactory reads it from the settings, none for a session that never ends
    /// (NonStopSession). markFile: where the mark is kept between runs, empty for nowhere. name: the session's, for the
    /// log
    FixCarryingStore(FIX::MessageStore *inner, std::optional<FIX::TimeRange> sessionTime,
                     std::filesystem::path markFile, std::string name);

    /// The session whose store this is, as QuickFIX makes it (FixGateway::onCreate()). Until then no client can have
    /// logged on, so a message stored before counts as unsent.
    void attach(FIX::Session *session);

    bool set(FIX::SEQNUM seq, const std::string &message) override;
    void get(FIX::SEQNUM begin, FIX::SEQNUM end, std::vector<std::string> &messages) const override;

    FIX::SEQNUM getNextSenderMsgSeqNum() const override;
    FIX::SEQNUM getNextTargetMsgSeqNum() const override;
    void setNextSenderMsgSeqNum(FIX::SEQNUM seq) override;
    void setNextTargetMsgSeqNum(FIX::SEQNUM seq) override;
    void incrNextSenderMsgSeqNum() override;
    void incrNextTargetMsgSeqNum() override;

    FIX::UtcTimeStamp getCreationTime() const override;

    void reset(const FIX::UtcTimeStamp &now) override;
    void refresh() override;

    /// The first message the client hasn't received, as far as this store knows, or 0 for none
    FIX::SEQNUM unsentFrom() const;
    /// Where the messages are kept: what get() would give, without moving the mark
    FIX::MessageStore *inner() const;

    /// message, a whole FIX message as a session stores it, numbered seq and with PossResend (97) Y, and with the
    /// BodyLength (9) and CheckSum (10) those make; none if message isn't whole
    static std::optional<std::string> restamped(const std::string &message, FIX::SEQNUM seq);
    /// Whether message, as a session stores it, is an application message, not a session one
    static bool isApplication(const std::string &message);

private:
    bool loggedOn() const;
    /// The mark markFile_ keeps for this store's day, or 0
    FIX::SEQNUM readMark() const;
    /// Keeps the mark in markFile_, or removes the file when there is none. A failure is logged, not thrown: QuickFIX
    /// would send nothing if a store's set() threw.
    void keepMark() const;

    FIX::MessageStore *inner_;
    std::optional<FIX::TimeRange> sessionTime_;
    const std::filesystem::path markFile_;
    const std::string name_;
    std::atomic<FIX::Session *> session_{ nullptr };
    mutable FIX::SEQNUM unsentFrom_ = 0; // get(), a const reader, moves it
};

/// Makes a FixCarryingStore for each session, over the store inner makes for it, with the session time the session's
/// settings give. With keepMarks each keeps its mark in a file beside FileStore's, in the session's FileStorePath:
/// <BeginString>-<SenderCompID>-<TargetCompID>.unsent, as FileStore names its own. A gateway given here hands each
/// store its session as QuickFIX makes it, until this goes.
class FixCarryingStoreFactory final : public FIX::MessageStoreFactory
{
public:
    FixCarryingStoreFactory(const FIX::SessionSettings &settings, std::unique_ptr<FIX::MessageStoreFactory> inner,
                            bool keepMarks, FixGateway *gateway = nullptr);
    ~FixCarryingStoreFactory() override;
    FixCarryingStoreFactory(const FixCarryingStoreFactory &) = delete;
    FixCarryingStoreFactory &operator=(const FixCarryingStoreFactory &) = delete;

    FIX::MessageStore *create(const FIX::UtcTimeStamp &now, const FIX::SessionID &sid) override;
    void destroy(FIX::MessageStore *store) override;

    /// Hands sid's store its session
    void attach(const FIX::SessionID &sid, FIX::Session *session);
    /// sid's store, while there is one
    FixCarryingStore *storeOf(const FIX::SessionID &sid);

    /// The session time QuickFIX's SessionFactory gives a session with these settings, which decides when its day
    /// ends; none for a session that never ends
    static std::optional<FIX::TimeRange> sessionTime(const FIX::Dictionary &settings);

private:
    const FIX::SessionSettings settings_;
    const std::unique_ptr<FIX::MessageStoreFactory> inner_;
    const bool keepMarks_;
    FixGateway *const gateway_;
    std::mutex lock_; // guards stores_
    std::map<FIX::SessionID, FixCarryingStore *> stores_;
};

/// The stores the FIX sessions keep what they send in, for a ResendRequest: FileStore's, in FileStorePath, each in a
/// FixCarryingStore, which keeps what a client hasn't received for the next session day (#513). The server makes them
/// here, and so does BM_FixSendExecutionReportStored, which times a report stored by them. gateway is the sessions'
/// application, which hands each store its session.
std::unique_ptr<FIX::MessageStoreFactory> makeFixStores(const FIX::SessionSettings &settings, FixGateway &gateway);

} // namespace COP::App

#endif // BUILD_FIX
