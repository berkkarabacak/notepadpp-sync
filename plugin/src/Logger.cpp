#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ctime>
#include <fstream>

namespace npsync {

std::wstring Logger::dir_;
LogLevel Logger::level_ = LogLevel::Info;
std::mutex Logger::mu_;

void Logger::init(const std::wstring& logDir, LogLevel level) {
    std::lock_guard<std::mutex> lk(mu_);
    dir_ = logDir;
    level_ = level;
    CreateDirectoryW(dir_.c_str(), nullptr);
}

void Logger::setLevel(LogLevel level) { level_ = level; }

void Logger::log(LogLevel level, const std::string& msg) {
    if (level > level_ || dir_.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);

    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t name[64];
    swprintf(name, 64, L"npsync-%04d%02d.log", lt.tm_year + 1900, lt.tm_mon + 1);

    static const char* lvl[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
    char line[2048];
    snprintf(line, sizeof(line), "%02d:%02d:%02d [%s] %s\n",
             lt.tm_hour, lt.tm_min, lt.tm_sec, lvl[static_cast<int>(level)], msg.c_str());

    std::ofstream f(dir_ + L"\\" + name, std::ios::app);
    if (f) f << line;
}

} // namespace npsync
