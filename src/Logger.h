/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "Singleton.h"

namespace aux
{

/// How a log file rotates (#569): at maxMb MiB the file is renamed, and spdlog keeps `files` of them beside the one
/// being written (exchange.1.log the newest, up to exchange.<files>.log), deleting the oldest. maxMb 0 keeps one file
/// that grows without bound, as the log did before.
struct LogRotation
{
    static constexpr unsigned long long DEFAULT_MAX_MB = 256;
    static constexpr unsigned long long DEFAULT_FILES = 5;
    static constexpr unsigned long long MAX_MAX_MB = 1024ULL * 1024ULL; // 1 TiB
    static constexpr unsigned long long MAX_FILES = 1000;
    unsigned long long maxMb = DEFAULT_MAX_MB;
    unsigned long long files = DEFAULT_FILES;
    /// A variable that couldn't be used, its default taken instead: the logger writes each once it exists
    std::vector<std::string> warnings;
};

/// The rotation EXCHANGE_LOG_MAX_MB and EXCHANGE_LOG_FILES give (#569), each null when unset: a whole number from 0
/// to MAX_MAX_MB, and from 1 to MAX_FILES. A value that isn't one, an empty one included, takes its default, and
/// warnings says so.
LogRotation logRotationFrom(const char *maxMb, const char *files);

struct LoggerImpl;

class LogMessage
{
public:
    LogMessage() : binary_(false) {}
    virtual ~LogMessage() {}

    virtual void prepareMessage() = 0;

    const char *getMessage(size_t *size)
    {
        assert(nullptr != size);
        *size = msg.length();
        return msg.c_str();
    }
    std::string &getMessage()
    {
        return msg;
    }

    bool isBinary() const
    {
        return binary_;
    }

protected:
    bool binary_;
    std::string msg;
};

class Logger
{
public:
    Logger(void);
    ~Logger(void);

    void debug(const std::string &msg);
    void debug(const char *msg);
    void debug(const char *msg, size_t size);
    void debug(int val);
    void debug(LogMessage &msg);

    void note(const std::string &msg);
    void note(const char *msg);
    void note(const char *msg, size_t size);
    void note(int val);
    void note(LogMessage &msg);

    void warn(const std::string &msg);
    void warn(const char *msg);
    void warn(const char *msg, size_t size);
    void warn(int val);
    void warn(LogMessage &msg);

    void error(const std::string &msg);
    void error(const char *msg);
    void error(const char *msg, size_t size);
    void error(int val);
    void error(LogMessage &msg);

    void fatal(const std::string &msg);
    void fatal(const char *msg);
    void fatal(const char *msg, size_t size);
    void fatal(int val);
    void fatal(LogMessage &msg);

    void setDebugOn(bool val);
    void setNoteOn(bool val);
    void setWarnOn(bool val);
    void setErrorOn(bool val);
    void setFatalOn(bool val);

    bool isDebugOn() const;
    bool isNoteOn() const;
    bool isWarnOn() const;
    bool isErrorOn() const;
    bool isFatalOn() const;

private:
    std::unique_ptr<LoggerImpl> impl_;
};

typedef Singleton<Logger> ExchLogger;
} // namespace aux