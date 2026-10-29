/*
 * gs32link_sim - 模拟器驱动
 *
 * 无硬件开发/测试用。不访问任何真实设备, 按当前时间合成波形数据:
 * 把每个 region 按 4 字节切成 float 通道, 通道 i 返回
 *     v(i, t) = A * sin(2*pi*f_i * t + phase_i)
 * 幅值/频率按通道序号递变, 便于肉眼区分。
 *
 * extra JSON (可选):
 *   { "latency_us": 200, "noise": 0.05 }   // 模拟读取耗时与噪声
 */
#include "../common/gs32link_util.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using gs32::ErrorState;

struct SimState {
    ErrorState err;
    std::mutex mtx;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    uint32_t latency_us = 200;
    double   noise      = 0.05;
    /* 写入回读: 地址 -> 最近写入的 32bit 字。写过的通道保持写入值,
     * 用于上位机写入/滑块联调(未写通道仍按时间合成波形)。 */
    std::unordered_map<uint32_t, uint32_t> held;
    std::atomic<uint32_t> caps{GS32_CAP_POLL | GS32_CAP_WRITE /*写可回读校验*/};
};

std::string g_global_error = "no error";

inline double nowSec(const SimState* s) {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now() - s->t0).count();
}

} // namespace

extern "C" {

GS32LINK_EXPORT const char* gs32_driver_name(void) { return "sim"; }

GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen) {
    static const char* v = "gs32link sim 0.1.0";
    if (!buf) return (int)strlen(v) + 1;
    uint32_t n = buflen < (uint32_t)strlen(v) + 1 ? buflen : (uint32_t)strlen(v) + 1;
    memcpy(buf, v, n);
    return (int)n;
}

GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t h, uint32_t* caps) {
    if (!caps) return GS32_ERR_PARAM;
    *caps = h ? static_cast<SimState*>(h)->caps.load()
              : (GS32_CAP_POLL | GS32_CAP_STREAM | GS32_CAP_WRITE);
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_open(const gs32_config_t* cfg, gs32_handle_t* out) {
    if (!cfg || !out) return GS32_ERR_PARAM;
    auto* s = new SimState();
    if (cfg->extra) {
        /* 极简键值提取, 避免 JSON 依赖; 正式实现时换解析器 */
        const char* p;
        if ((p = strstr(cfg->extra, "\"latency_us\""))) s->latency_us = (uint32_t)atoi(strchr(p, ':') + 1);
        if ((p = strstr(cfg->extra, "\"noise\"")))      s->noise      = atof(strchr(p, ':') + 1);
    }
    g_global_error = "no error";
    s->err.clear();
    *out = s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_close(gs32_handle_t h) {
    delete static_cast<SimState*>(h);
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_read(gs32_handle_t h,
                              const gs32_mem_region_t* regions, uint32_t count,
                              uint8_t* out_buf, uint32_t buf_size,
                              uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<SimState*>(h);
    if (!s || !regions || !count || !out_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "out_buf too small");
        return GS32_ERR_PARAM;
    }

    /* 模拟探针读取耗时 */
    if (s->latency_us)
        std::this_thread::sleep_for(std::chrono::microseconds(s->latency_us));

    const double t  = nowSec(s);
    uint32_t channel = 0;
    uint8_t* p = out_buf;
    for (uint32_t r = 0; r < count; ++r) {
        for (uint32_t off = 0; off + 4 <= regions[r].size; off += 4, ++channel) {
            const uint32_t wordAddr = regions[r].addr + off;
            auto hit = s->held.find(wordAddr);
            if (hit != s->held.end()) {
                const uint32_t w = hit->second;
                memcpy(p + off, &w, 4);
                continue;
            }
            /* 0x20100000..0x20100010: 电机演示信号(供 --demo 使用) */
            if (wordAddr >= 0x20100000u && wordAddr <= 0x20100010u) {
                double dv = 0.0;
                switch (wordAddr) {
                case 0x20100000u: dv = 60.0 + 40.0 * std::sin(2.0 * 3.14159265358979 * 0.2 * t); break;
                case 0x20100004u: dv = 2.0 + 1.5 * std::sin(2.0 * 3.14159265358979 * 1.5 * t); break;
                case 0x20100008u: dv = 80.0; break;
                case 0x2010000Cu: dv = 1.0; break;
                case 0x20100010u: dv = 45.0 + 5.0 * std::sin(2.0 * 3.14159265358979 * 0.05 * t); break;
                default: break;
                }
                const float fv = (float)dv;
                memcpy(p + off, &fv, 4);
                continue;
            }
            const double freq  = 0.5 + 0.7 * channel;              /* Hz */
            const double amp   = (channel % 3 == 0) ? 100.0 : (channel % 3 == 1 ? 1.0 : 50.0);
            double v = amp * std::sin(2.0 * 3.14159265358979 * freq * t + 0.8 * channel);
            /* 慢通道 3: 三角波, 测试非正弦场景 */
            if (channel % 7 == 6) {
                double x = std::fmod(t * (0.2 + 0.05 * channel), 2.0);
                v = amp * (x < 1.0 ? x : 2.0 - x);
            }
            if (s->noise > 0) v += s->noise * amp * ((rand() / (double)RAND_MAX) - 0.5);
            const float fv = (float)v;
            memcpy(p + off, &fv, 4);
        }
        /* region 尾部不足 4 字节的残余填 0 */
        uint32_t tail = regions[r].size & 3u;
        if (tail) memset(p + (regions[r].size & ~3u), 0, tail);
        p += regions[r].size;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_write(gs32_handle_t h,
                               const gs32_mem_region_t* regions, uint32_t count,
                               const uint8_t* in_buf, uint32_t buf_size,
                               uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<SimState*>(h);
    if (!s || !regions || !count || !in_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "in_buf too small");
        return GS32_ERR_PARAM;
    }
    const uint8_t* p = in_buf;
    for (uint32_t r = 0; r < count; ++r) {
        for (uint32_t off = 0; off + 4 <= regions[r].size; off += 4) {
            uint32_t w = 0;
            memcpy(&w, p + off, 4);
            s->held[regions[r].addr + off] = w;
        }
        p += regions[r].size;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h) {
    return h ? static_cast<SimState*>(h)->err.text() : g_global_error.c_str();
}

/* -------- STREAM(P1): 模拟器后续用定时线程攒块, 先返回未实现 -------- */
GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t, uint32_t,
                                       const gs32_mem_region_t*, uint32_t,
                                       const gs32_flush_policy_t*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t, gs32_notify_cb, void*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t)  { return GS32_ERR_NOT_IMPL; }

} /* extern "C" */
