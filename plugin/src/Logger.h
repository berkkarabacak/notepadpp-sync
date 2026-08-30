// Logger.h — leveled file logging to %APPDATA%\Notepad++Sync\logs.
// Never logs decrypted content, keys, passwords, recovery keys, or tokens.
#pragma once

#include <mutex>
#include <string>

namespace npsync
{

enum class LogLevel
{
    Error = 0,
    Warning = 1,
    Info = 2,
    Debug = 3
};

class Logger {
  public:
    static void init(const std::wstring& logDir, LogLevel level);
    static void setLevel(LogLevel level);
    static void log(LogLevel level, const std::string& msg);

    static void error(const std::string& msg) {
        log(LogLevel::Error, msg);
    }
    static void warn(const std::string& msg) {
        log(LogLevel::Warning, msg);
    }
    static void info(const std::string& msg) {
        log(LogLevel::Info, msg);
    }
    static void debug(const std::string& msg) {
        log(LogLevel::Debug, msg);
    }

  private:
    static std::wstring dir_;
    static LogLevel level_;
    static std::mutex mu_;
};

} // namespace npsync
