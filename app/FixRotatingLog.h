#pragma once

#ifdef BUILD_FIX

#include <quickfix/Log.h>
#include <quickfix/SessionID.h>
#include <quickfix/SessionSettings.h>

#include <cstddef>
#include <memory>
#include <string>

namespace spdlog
{
class logger;
}

namespace COP::App
{

/// A QuickFIX log whose files are bounded (#160). QuickFIX's FileLog appends to a session's two files, its messages and
/// its events, for as long as the files exist: no QuickFIX code calls the log's backup() or clear(), so they grew
/// without limit on the data volume, beside LMDB.
///
/// This writes the same lines, "<UTC timestamp> : <text>" in the same two files per session, and rotates each one when
/// it reaches maxFileBytes: <name>.log becomes <name>.1.log, and so on, and the oldest of `backups` goes. A session's
/// two files hold at most 2 x maxFileBytes x (backups + 1). Each line is written out at once, as FileLog's are, so a
/// crash keeps the audit trail up to it.
class FixRotatingLog final : public FIX::Log
{
public:
    /// prefix: the files' path and name before ".messages.log" and ".event.log"
    FixRotatingLog(const std::string &prefix, std::size_t maxFileBytes, std::size_t backups);
    ~FixRotatingLog() override;

    /// Both do nothing: rotation bounds the files, and QuickFIX doesn't call either
    void clear() override;
    void backup() override;

    void onIncoming(const std::string &text) override;
    void onOutgoing(const std::string &text) override;
    void onEvent(const std::string &text) override;

private:
    std::shared_ptr<spdlog::logger> messages_;
    std::shared_ptr<spdlog::logger> events_;
};

/// Makes a FixRotatingLog for each session, in the directory FileLogPath names, as FileLogFactory does. Two settings of
/// this project's own bound the files: FileLogMaxMB, each file's size in MiB (default 64), and FileLogBackups, how many
/// rotated files each keeps (default 9).
class FixRotatingLogFactory final : public FIX::LogFactory
{
public:
    static const std::size_t DEFAULT_MAX_MB = 64;
    static const std::size_t DEFAULT_BACKUPS = 9;

    explicit FixRotatingLogFactory(const FIX::SessionSettings &settings);

    FIX::Log *create() override;
    FIX::Log *create(const FIX::SessionID &sessionId) override;
    void destroy(FIX::Log *log) override;

private:
    std::size_t maxFileBytes(const FIX::Dictionary &dict) const;
    std::size_t backups(const FIX::Dictionary &dict) const;

    FIX::SessionSettings settings_;
};

} // namespace COP::App

#endif // BUILD_FIX
