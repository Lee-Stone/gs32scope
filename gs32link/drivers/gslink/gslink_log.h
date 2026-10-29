#ifndef GSLINK_LOG_H
#define GSLINK_LOG_H
/* gslink 驱动日志(三层共用)
 * 等级: 默认 info; 环境变量 GS32LINK_GSLINK_LOG=debug 开启 tx/rx 数据包
 * 输出: 优先走宿主注册的日志回调(gs32_set_log_sink), 未注册时丢弃(GUI 不再弹控制台) */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <windows.h>

namespace gs32 {

enum class LogLevel : int { Info = 0, Debug = 1 };

using LogSinkFn = void (*)(const char* line, void* user);

inline LogSinkFn& logSink() { static LogSinkFn fn = nullptr; return fn; }
inline void*& logSinkUser() { static void* user = nullptr; return user; }

inline void setLogSink(LogSinkFn fn, void* user) {
    logSink() = fn;
    logSinkUser() = user;
}

inline int logLevel() {
    static const int lvl = [] {
        char buf[16] = {};
        const DWORD n = GetEnvironmentVariableA("GS32LINK_GSLINK_LOG", buf, sizeof(buf));
        return (n > 0 && n < sizeof(buf) && _stricmp(buf, "debug") == 0)
                   ? (int)LogLevel::Debug : (int)LogLevel::Info;
    }();
    return lvl;
}

/* 统一格式化后送回调; 无回调直接丢弃(不再 printf, 避免宿主 GUI 弹控制台) */
inline void logEmit(const char* fmt, va_list ap) {
    LogSinkFn fn = logSink();
    if (!fn) return;
    char buf[2048];
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) return;
    fn(buf, logSinkUser());
}

/* info 级: 始终显示 */
inline void logInfo(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logEmit(fmt, ap);
    va_end(ap);
}

/* debug 级: tx/rx 数据包等高频细节, 默认不显示 */
inline void logDbg(const char* fmt, ...) {
    if (logLevel() < (int)LogLevel::Debug) return;
    va_list ap;
    va_start(ap, fmt);
    logEmit(fmt, ap);
    va_end(ap);
}

/* 兼容旧调用点: 不再分配控制台, 输出全部走日志回调 */
inline void debugInit() {}

/* hex 转储(每 16 字节换行缩进) */
inline std::string hexDump(const uint8_t* p, size_t n) {
    std::string s;
    char t[8];
    for (size_t i = 0; i < n; ++i) {
        if (i && (i % 16) == 0) s += "\n        ";
        snprintf(t, sizeof(t), "%02X ", p[i]);
        s += t;
    }
    return s;
}

} // namespace gs32

#endif /* GSLINK_LOG_H */
