/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "Logger.h"
#include "ExchUtils.h"
#include <oneapi/tbb/mutex.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

using namespace aux;

namespace
{
const std::string DEBUG_PREFIX(": [Debug][");
const std::string NOTE_PREFIX(": [Note][");
const std::string WARN_PREFIX(": [Warn][");
const std::string ERROR_PREFIX(": [Error][");
const std::string FATAL_PREFIX(": [Fatal][");

std::string getTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_time;
    gmtime_r(&time, &tm_time);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03d", tm_time.tm_year + 1900, tm_time.tm_mon + 1,
                  tm_time.tm_mday, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}

std::string getThreadId()
{
    return std::format("{}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
}
} // namespace

namespace aux
{
const size_t BUFFER_SIZE = 256;
struct LoggerImpl
{
    char buf_[BUFFER_SIZE + 1];
    mutable oneapi::tbb::mutex lock_;
    std::shared_ptr<spdlog::logger> logger_;

    enum LoggingMask
    {
        ALL_DISABLED_FLAG = 0,
        DEBUG_ON_FLAG = 1,
        NOTE_ON_FLAG = 2,
        WARN_ON_FLAG = 4,
        ERROR_ON_FLAG = 8,
        FATAL_ON_FLAG = 16,
        ALL_ENABLED_FLAG = 31
    };
    std::atomic<char> loggingMask_;

    LoggerImpl()
    {
        loggingMask_.store(ALL_ENABLED_FLAG);
        try
        {
            // Use async logging to avoid blocking the hot path.
            // Queue size 8192 slots, single background thread.
            spdlog::init_thread_pool(8192, 1);
            logger_ = spdlog::basic_logger_mt<spdlog::async_factory>("exchange", "exchange.log");
        }
        catch (const spdlog::spdlog_ex &)
        {
            // Fallback to stdout if file logging fails
            logger_ = spdlog::stdout_color_mt("exchange");
        }
        logger_->set_pattern("%v");
        // Every message reaches spdlog at its own level (#65): loggingMask_ decides what is written, so spdlog passes
        // everything down to debug, and writes warnings, errors and fatals out at once rather than when its buffer
        // fills. Before, every level went in as info, so that flush never happened, and a crash lost them.
        logger_->set_level(spdlog::level::trace);
        logger_->flush_on(spdlog::level::warn);
    }

    ~LoggerImpl()
    {
        // Join the async worker thread and clear the global registry before
        // any shared_ptr<logger> member destructs.  Otherwise the worker can
        // still be touching the logger's refcount while the unique_ptr<Logger>
        // tear-down releases its shared_ptr, racing on the atomic in
        // _Sp_counted_base::_M_release (TSan: see Logger.cpp:101 race).
        spdlog::shutdown();
    }

    void setFlag(bool enabled, char flag)
    {
        char before = ALL_DISABLED_FLAG;
        do
        {
            before = loggingMask_.load();
            char newVal = before;
            if (enabled)
            {
                if (0 != (before & flag))
                {
                    return;
                }
                newVal |= flag;
            }
            else
            {
                if (0 == (before & ~flag))
                {
                    return;
                }
                newVal &= ~flag;
            }
            if (loggingMask_.compare_exchange_strong(before, newVal))
            {
                return;
            }
        } while (true);
    }

    bool isFlag(char flag)
    {
        char mask = loggingMask_.load();
        return 0 != (mask & flag);
    }

    /// The spdlog level of a message logged under this flag; the text carries the level in its prefix too
    static spdlog::level::level_enum levelOf(char flag)
    {
        switch (flag)
        {
        case DEBUG_ON_FLAG:
            return spdlog::level::debug;
        case WARN_ON_FLAG:
            return spdlog::level::warn;
        case ERROR_ON_FLAG:
            return spdlog::level::err;
        case FATAL_ON_FLAG:
            return spdlog::level::critical;
        default:
            return spdlog::level::info;
        }
    }

    void logMessage(const std::string &msg, char flag, const std::string &prefix)
    {
        if (!isFlag(flag))
        {
            return;
        }

        oneapi::tbb::mutex::scoped_lock lock(lock_);
        logger_->log(levelOf(flag), "{}{}{}] {}", getTimestamp(), prefix, getThreadId(), msg);
    }

    void logMessage(const char *msg, char flag, const std::string &prefix)
    {
        if (!isFlag(flag))
        {
            return;
        }

        oneapi::tbb::mutex::scoped_lock lock(lock_);
        logger_->log(levelOf(flag), "{}{}{}] {}", getTimestamp(), prefix, getThreadId(), msg);
    }

    void logMessage(int val, char flag, const std::string &prefix)
    {
        if (!isFlag(flag))
        {
            return;
        }

        oneapi::tbb::mutex::scoped_lock lock(lock_);
        logger_->log(levelOf(flag), "{}{}{}] {}", getTimestamp(), prefix, getThreadId(), val);
    }

    void logMessage(LogMessage &msg, char flag, const std::string &prefix)
    {
        if (!isFlag(flag))
        {
            return;
        }

        msg.prepareMessage();

        oneapi::tbb::mutex::scoped_lock lock(lock_);
        logger_->log(levelOf(flag), "{}{}{}] {}", getTimestamp(), prefix, getThreadId(), msg.getMessage());
    }
};
} // namespace aux

Logger::Logger(void)
{
    impl_ = std::make_unique<LoggerImpl>();

    note("----------------------------------------------------------------------");
    note(" LogSystem initialized.");
}

Logger::~Logger(void)
{
    note(" LogSystem stopped.");
    note("----------------------------------------------------------------------");
}

void Logger::setDebugOn(bool val)
{
    assert(nullptr != impl_);
    impl_->setFlag(val, LoggerImpl::DEBUG_ON_FLAG);
    if (val)
    {
        warn("Debug logging turned on.");
    }
    else
    {
        warn("Debug logging turned off.");
    }
}

void Logger::setNoteOn(bool val)
{
    assert(nullptr != impl_);
    impl_->setFlag(val, LoggerImpl::NOTE_ON_FLAG);
    if (val)
    {
        warn("Notification logging turned on.");
    }
    else
    {
        warn("Notification logging turned off.");
    }
}

void Logger::setWarnOn(bool val)
{
    assert(nullptr != impl_);
    impl_->setFlag(val, LoggerImpl::WARN_ON_FLAG);
    if (val)
    {
        warn("Warning logging turned on.");
    }
    else
    {
        warn("Warning logging turned off.");
    }
}

void Logger::setErrorOn(bool val)
{
    assert(nullptr != impl_);
    impl_->setFlag(val, LoggerImpl::ERROR_ON_FLAG);
    if (val)
    {
        warn("Error logging turned on.");
    }
    else
    {
        warn("Error logging turned off.");
    }
}

void Logger::setFatalOn(bool val)
{
    assert(nullptr != impl_);
    impl_->setFlag(val, LoggerImpl::FATAL_ON_FLAG);
    if (val)
    {
        warn("Fatal logging turned on.");
    }
    else
    {
        warn("Fatal logging turned off.");
    }
}

bool Logger::isDebugOn() const
{
    assert(nullptr != impl_);
    return impl_->isFlag(LoggerImpl::DEBUG_ON_FLAG);
}

bool Logger::isNoteOn() const
{
    assert(nullptr != impl_);
    return impl_->isFlag(LoggerImpl::NOTE_ON_FLAG);
}

bool Logger::isWarnOn() const
{
    assert(nullptr != impl_);
    return impl_->isFlag(LoggerImpl::WARN_ON_FLAG);
}

bool Logger::isErrorOn() const
{
    assert(nullptr != impl_);
    return impl_->isFlag(LoggerImpl::ERROR_ON_FLAG);
}

bool Logger::isFatalOn() const
{
    assert(nullptr != impl_);
    return impl_->isFlag(LoggerImpl::FATAL_ON_FLAG);
}

void Logger::debug(const std::string &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::debug(const char *msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::debug(int val)
{
    assert(nullptr != impl_);
    impl_->logMessage(val, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::debug(LogMessage &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::DEBUG_ON_FLAG, DEBUG_PREFIX);
}

void Logger::note(const std::string &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::note(const char *msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::note(int val)
{
    assert(nullptr != impl_);
    impl_->logMessage(val, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::note(LogMessage &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::NOTE_ON_FLAG, NOTE_PREFIX);
}

void Logger::warn(const std::string &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::warn(const char *msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::warn(int val)
{
    assert(nullptr != impl_);
    impl_->logMessage(val, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::warn(LogMessage &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::WARN_ON_FLAG, WARN_PREFIX);
}

void Logger::error(const std::string &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::error(const char *msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::error(int val)
{
    assert(nullptr != impl_);
    impl_->logMessage(val, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::error(LogMessage &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::ERROR_ON_FLAG, ERROR_PREFIX);
}

void Logger::fatal(const std::string &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}

void Logger::fatal(const char *msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}

void Logger::fatal(int val)
{
    assert(nullptr != impl_);
    impl_->logMessage(val, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}

void Logger::fatal(LogMessage &msg)
{
    assert(nullptr != impl_);
    impl_->logMessage(msg, LoggerImpl::FATAL_ON_FLAG, FATAL_PREFIX);
}
