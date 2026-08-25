#pragma once
/* 轻量日志模块：分级输出 + 时间戳 + 线程安全。
 * 用法：
 *   cam::Logger::instance().setLevel(cam::LogLevel::kDebug);
 *   LOG_INFO("client %d connected", fd);
 * 分级：kDebug < kInfo < kWarn < kError < kOff（kOff 关闭所有输出）。 */

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace cam {

enum class LogLevel : int {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
    kOff = 4,   /* 关闭所有日志 */
};

class Logger {
public:
    static Logger& instance();          /* 全局单例 */

    void setLevel(LogLevel level);      /* 只输出 >= 该级别的内容 */
    LogLevel level() const { return level_; }
    static const char* levelName(LogLevel lv);  /* 级别名：DBG/INF/WRN/ERR */

    void enableTimestamp(bool on) { ts_ = on; }

    void debug(const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);

private:
    Logger() = default;
    void log(LogLevel lv, const char* fmt, va_list args);

    LogLevel level_ = LogLevel::kInfo;
    bool     ts_    = true;
    std::mutex mu_;
};

}  /* namespace cam */

#define LOG_DEBUG(...) ::cam::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)  ::cam::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...)  ::cam::Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) ::cam::Logger::instance().error(__VA_ARGS__)
