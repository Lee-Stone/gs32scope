#ifndef GSLINK_LOG_H
#define GSLINK_LOG_H
/* gslink 驱动日志(三层共用)
 * 等级: 默认 info; 环境变量 GS32LINK_GSLINK_LOG=debug 开启 tx/rx 数据包 */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <windows.h>

namespace gs32 {

enum class LogLevel : int { Info = 0, Debug = 1 };

inline int logLevel() {
    static const int lvl = [] {
        char buf[16] = {};
        const DWORD n = GetEnvironmentVariableA("GS32LINK_GSLINK_LOG", buf, sizeof(buf));
        return (n > 0 && n < sizeof(buf) && _stricmp(buf, "debug") == 0)
                   ? (int)LogLevel::Debug : (int)LogLevel::Info;
    }();
    return lvl;
}

/* info 级: 始终显示 */
inline void logInfo(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

/* debug 级: tx/rx 数据包等高频细节, 默认不显示 */
inline void logDbg(const char* fmt, ...) {
    if (logLevel() < (int)LogLevel::Debug) return;
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

/* 调试控制台(宿主为 GUI 程序时分配) */
inline void debugInit() {
    static bool done = false;
    if (done) return;
    done = true;
    AllocConsole();
    FILE* f = freopen("CONOUT$", "w", stdout);
    if (f) setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== gs32link_gslink debug console (level=%s) ===\n",
                logLevel() == (int)LogLevel::Debug ? "debug" : "info");
}

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
