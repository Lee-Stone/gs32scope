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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

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
    std::atomic<uint32_t> caps{GS32_CAP_POLL | GS32_CAP_STREAM | GS32_CAP_WRITE};

    /* STREAM: 按 period 生成样本, 攒块后通过 notify 推送(模拟硬件定时流) */
    std::thread streamThread;
    std::atomic<bool> streamRunning{false};
    gs32_notify_cb notifyCb = nullptr;
    void* notifyUser = nullptr;
    std::vector<gs32_mem_region_t> streamRegions;
    uint32_t streamPeriodNs = 100000;          /* 默认 100us -> 10kHz */
    gs32_flush_policy_t streamPolicy{100, 10000000};  /* 100 点 / 10ms */
    uint64_t streamSeq = 0;
};

std::string g_global_error = "no error";

inline double nowSec(const SimState* s) {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now() - s->t0).count();
}

/* 生成一个 region 的一拍数据; 调用方持有 s->mtx.
 * channel 在同一采样内跨 region 递增, 与 POLL 读路径保持一致。 */
void fillRegion(SimState* s, uint32_t addr, uint32_t size, uint8_t* out,
                double t, uint32_t& channel) {
    for (uint32_t off = 0; off + 4 <= size; off += 4, ++channel) {
        const uint32_t wordAddr = addr + off;
        auto hit = s->held.find(wordAddr);
        if (hit != s->held.end()) {
            const uint32_t w = hit->second;
            memcpy(out + off, &w, 4);
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
            memcpy(out + off, &fv, 4);
            continue;
        }
        const double freq = 0.5 + 0.7 * channel;              /* Hz */
        const double amp = (channel % 3 == 0) ? 100.0 : (channel % 3 == 1 ? 1.0 : 50.0);
        double v = amp * std::sin(2.0 * 3.14159265358979 * freq * t + 0.8 * channel);
        if (channel % 7 == 6) {
            double x = std::fmod(t * (0.2 + 0.05 * channel), 2.0);
            v = amp * (x < 1.0 ? x : 2.0 - x);
        }
        if (s->noise > 0) v += s->noise * amp * ((rand() / (double)RAND_MAX) - 0.5);
        const float fv = (float)v;
        memcpy(out + off, &fv, 4);
    }
    uint32_t tail = size & 3u;
    if (tail) memset(out + (size & ~3u), 0, tail);
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
    auto* s = static_cast<SimState*>(h);
    if (!s) return GS32_OK;
    s->streamRunning.store(false);
    if (s->streamThread.joinable()) s->streamThread.join();
    delete s;
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
        fillRegion(s, regions[r].addr, regions[r].size, p, t, channel);
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

GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t h, uint32_t period_ns,
                                       const gs32_mem_region_t* regions, uint32_t count,
                                       const gs32_flush_policy_t* policy) {
    auto* s = static_cast<SimState*>(h);
    if (!s || !regions || !count || !period_ns) return GS32_ERR_PARAM;
    if (s->streamRunning.load()) return GS32_ERR_STATE;
    s->streamRegions.assign(regions, regions + count);
    s->streamPeriodNs = period_ns;
    s->streamPolicy = policy ? *policy : gs32_flush_policy_t{100, 10000000};
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t h, gs32_notify_cb cb, void* user) {
    auto* s = static_cast<SimState*>(h);
    if (!s) return GS32_ERR_PARAM;
    s->notifyCb = cb;
    s->notifyUser = user;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t h) {
    auto* s = static_cast<SimState*>(h);
    if (!s) return GS32_ERR_PARAM;
    if (s->streamRunning.load()) return GS32_OK;
    if (!s->notifyCb || s->streamRegions.empty()) {
        s->err.set(GS32_ERR_STATE, "STREAM 未配置");
        return GS32_ERR_STATE;
    }
    uint32_t regionBytes = 0;
    for (const auto& r : s->streamRegions) regionBytes += r.size;
    if (!regionBytes) return GS32_ERR_PARAM;

    const uint32_t period = std::max<uint32_t>(1, s->streamPeriodNs);
    uint32_t perBlock = s->streamPolicy.max_samples ? s->streamPolicy.max_samples : 100;
    if (perBlock == 0) perBlock = 100;
    if (perBlock > 10000) perBlock = 10000;
    if (s->streamPolicy.max_latency_ns) {
        const uint32_t byLatency = std::max<uint32_t>(1, s->streamPolicy.max_latency_ns / period);
        perBlock = std::min(perBlock, byLatency);
    }
    const uint64_t blockPeriodNs = uint64_t(perBlock) * period;

    s->streamSeq = 0;
    s->streamRunning.store(true);
    s->streamThread = std::thread([s, perBlock, blockPeriodNs, regionBytes, period] {
        using namespace std::chrono;
#ifdef _WIN32
        /* 仿真是软件定时; 提高优先级抵抗 GUI/绘图线程的抢占抖动.
         * 真实调试器由硬件定时, 不需要这段. */
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        std::vector<uint8_t> payload(size_t(perBlock) * regionBytes);
        const auto wallStart = steady_clock::now();
        const uint64_t baseNs = uint64_t(duration_cast<nanoseconds>(
                                             wallStart.time_since_epoch()).count());
        const double baseSec = nowSec(s);
        /* 连续虚拟采样时钟: 块间不因 Windows 调度抖动产生时间戳空隙.
         * 真实调试器的 t0 由硬件提供. */
        uint64_t virtualNs = baseNs;
        auto next = wallStart;
        while (s->streamRunning.load()) {
            const double t0Sec = baseSec + double(virtualNs - baseNs) * 1e-9;
            {
                std::lock_guard<std::mutex> lk(s->mtx);
                for (uint32_t i = 0; i < perBlock; ++i) {
                    uint8_t* base = payload.data() + size_t(i) * regionBytes;
                    const double t = t0Sec + double(i) * double(period) * 1e-9;
                    uint32_t off = 0, channel = 0;
                    for (const auto& r : s->streamRegions) {
                        fillRegion(s, r.addr, r.size, base + off, t, channel);
                        off += r.size;
                    }
                }
            }
            if (s->notifyCb) {
                gs32_block_hdr_t hdr{};
                hdr.magic = GS32_BLOCK_MAGIC;
                hdr.version = GS32_BLOCK_VERSION;
                hdr.header_size = sizeof(gs32_block_hdr_t);
                hdr.seq = s->streamSeq++;
                hdr.t0_ns = virtualNs;
                hdr.period_ns = period;
                hdr.sample_count = uint16_t(perBlock);
                hdr.region_count = uint16_t(s->streamRegions.size());
                hdr.flags = 0;
                hdr.payload_size = uint32_t(payload.size());
                s->notifyCb(reinterpret_cast<gs32_handle_t>(s), &hdr,
                            payload.data(), s->notifyUser);
            }
            virtualNs += blockPeriodNs;
            next += nanoseconds(blockPeriodNs);
            /* 长睡眠 + 末段自旋: 保持约 10ms 块节拍, 同时避免整段忙等占满 CPU */
            /* 纯自旋保证 10ms 块节拍: Windows Sleep/定时器在此线程上会放大到
             * ~20–31ms, 导致墙钟只产出 3–5kHz. 真实调试器由硬件定时. */
            while (s->streamRunning.load() && steady_clock::now() < next)
                std::atomic_signal_fence(std::memory_order_seq_cst);
            if (next < steady_clock::now())
                next = steady_clock::now() + nanoseconds(blockPeriodNs);
        }
    });
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t h) {
    auto* s = static_cast<SimState*>(h);
    if (!s) return GS32_ERR_PARAM;
    s->streamRunning.store(false);
    if (s->streamThread.joinable()) s->streamThread.join();
    return GS32_OK;
}

} /* extern "C" */
