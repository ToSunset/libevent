#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "logger.h"

#include <chrono>
#include <cstring>
#include <ctime>

namespace cam {

static const char* levelAbbr(LogLevel lv)
{
    switch (lv) {
        case LogLevel::kDebug: return "DBG";
        case LogLevel::kInfo:  return "INF";
        case LogLevel::kWarn:  return "WRN";
        case LogLevel::kError: return "ERR";
        default:               return "???";
    }
}

const char* Logger::levelName(LogLevel lv)
{
    switch (lv) {
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo:  return "info";
        case LogLevel::kWarn:  return "warn";
        case LogLevel::kError: return "error";
        case LogLevel::kOff:   return "off";
        default:               return "unknown";
    }
}

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

void Logger::setLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lk(mu_);
    level_ = level;
}

void Logger::log(LogLevel lv, const char* fmt, va_list args)
{
    if (lv < level_) return;

    std::lock_guard<std::mutex> lk(mu_);

    if (ts_) {
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto tt  = system_clock::to_time_t(now);
        const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
        std::fprintf(stdout, "[%s.%03d] ", stamp, static_cast<int>(ms.count()));
    }

    std::fprintf(stdout, "[%s] ", levelAbbr(lv));
    std::vfprintf(stdout, fmt, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void Logger::debug(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log(LogLevel::kDebug, fmt, ap); va_end(ap); }
void Logger::info(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); log(LogLevel::kInfo,  fmt, ap); va_end(ap); }
void Logger::warn(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); log(LogLevel::kWarn,  fmt, ap); va_end(ap); }
void Logger::error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log(LogLevel::kError, fmt, ap); va_end(ap); }

}  /* namespace cam */
