#ifdef BUILD_FIX

#include "FixRotatingLog.h"

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <filesystem>

using namespace COP::App;

namespace
{
/// This project's settings for the files' bounds (#160). QuickFIX ignores keys it doesn't know.
const char FILE_LOG_MAX_MB[] = "FileLogMaxMB";
const char FILE_LOG_BACKUPS[] = "FileLogBackups";

std::shared_ptr<spdlog::logger> rotatingLogger(const std::string &name, const std::string &file,
                                               std::size_t maxFileBytes, std::size_t backups)
{
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, maxFileBytes, backups);
    // Not registered: spdlog's registry belongs to the server's own log (Logger.cpp), whose shutdown drops what it holds
    auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
    // FileLog's line: the time as FIX writes it, in UTC, then " : " and the text
    logger->set_formatter(
        std::make_unique<spdlog::pattern_formatter>("%Y%m%d-%H:%M:%S.%e : %v", spdlog::pattern_time_type::utc));
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    return logger;
}

/// Writes the text as it is: a FIX message can hold braces, which spdlog would otherwise read as its format syntax
void write(spdlog::logger &logger, const std::string &text)
{
    logger.log(spdlog::source_loc{}, spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
}

/// FileLog's name for a session's files: BeginString-SenderCompID-TargetCompID, and the qualifier when there is one
std::string sessionName(const FIX::SessionID &id)
{
    std::string name = id.getBeginString().getString() + "-" + id.getSenderCompID().getString() + "-" +
                       id.getTargetCompID().getString();
    if (!id.getSessionQualifier().empty())
    {
        name += "-" + id.getSessionQualifier();
    }
    return name;
}

/// The directory FileLogPath names, made if it doesn't exist, as FileLog makes it
std::filesystem::path logDirectory(const FIX::Dictionary &dict)
{
    const std::filesystem::path path = dict.getString(FIX::FILE_LOG_PATH);
    std::filesystem::create_directories(path);
    return path;
}
} // namespace

FixRotatingLog::FixRotatingLog(const std::string &prefix, std::size_t maxFileBytes, std::size_t backups)
    : messages_(rotatingLogger(prefix + ".messages", prefix + ".messages.log", maxFileBytes, backups)),
      events_(rotatingLogger(prefix + ".event", prefix + ".event.log", maxFileBytes, backups))
{
}

FixRotatingLog::~FixRotatingLog() = default;

void FixRotatingLog::clear() {}

void FixRotatingLog::backup() {}

void FixRotatingLog::onIncoming(const std::string &text)
{
    write(*messages_, text);
}

void FixRotatingLog::onOutgoing(const std::string &text)
{
    write(*messages_, text);
}

void FixRotatingLog::onEvent(const std::string &text)
{
    write(*events_, text);
}

FixRotatingLogFactory::FixRotatingLogFactory(const FIX::SessionSettings &settings) : settings_(settings) {}

FIX::Log *FixRotatingLogFactory::create()
{
    const FIX::Dictionary &dict = settings_.get();
    return new FixRotatingLog((logDirectory(dict) / "GLOBAL").string(), maxFileBytes(dict), backups(dict));
}

FIX::Log *FixRotatingLogFactory::create(const FIX::SessionID &sessionId)
{
    const FIX::Dictionary &dict = settings_.get(sessionId);
    return new FixRotatingLog((logDirectory(dict) / sessionName(sessionId)).string(), maxFileBytes(dict),
                              backups(dict));
}

void FixRotatingLogFactory::destroy(FIX::Log *log)
{
    delete log;
}

std::size_t FixRotatingLogFactory::maxFileBytes(const FIX::Dictionary &dict) const
{
    const int mb = dict.has(FILE_LOG_MAX_MB) ? dict.getInt(FILE_LOG_MAX_MB) : static_cast<int>(DEFAULT_MAX_MB);
    if (mb < 1)
    {
        throw FIX::ConfigError(std::string(FILE_LOG_MAX_MB) + " must be at least 1");
    }
    return static_cast<std::size_t>(mb) << 20;
}

std::size_t FixRotatingLogFactory::backups(const FIX::Dictionary &dict) const
{
    const int count = dict.has(FILE_LOG_BACKUPS) ? dict.getInt(FILE_LOG_BACKUPS) : static_cast<int>(DEFAULT_BACKUPS);
    if (count < 0)
    {
        throw FIX::ConfigError(std::string(FILE_LOG_BACKUPS) + " can't be negative");
    }
    return static_cast<std::size_t>(count);
}

#endif // BUILD_FIX
