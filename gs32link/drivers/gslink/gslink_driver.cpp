/*
 * 层3: gs32link C ABI 对外接口(gs32link_gslink.dll 入口)
 *
 * 分层架构:
 *   层1 ftdi_jtag   — FTDI(FT2232H) D2XX/MPSSE 直驱, JTAG TAP 状态机
 *   层2 riscv_debug — RISC-V Debug 0.13.2 DTM/DMI/DM/SBA, 提供内存读写
 *   层3 本文件      — gs32_* C ABI(句柄/锁/诊断编排)
 */
#include "gslink_log.h"
#include "riscv_debug.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using gs32::ErrorState;
using gs32::FtdiJtag;
using gs32::RiscvDebug;

struct GsLinkState {
    ErrorState err;
    std::mutex mtx;
    FtdiJtag jtag;
    RiscvDebug dbg;
    bool sbaDiagDone = false;   /* SBA/Abstract 路径探测只做一次 */
    GsLinkState() : dbg(jtag, err) {}
};

std::string g_global_error = "no error";

/* extra JSON 值提取(无 JSON 库): {"model":"GS32F0039A","idcode":2717717613} */
std::string jsonStr(const char* json, const char* key) {
    if (!json || !key) return {};
    const std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    const char* c = k ? strchr(k + pat.size(), ':') : nullptr;
    const char* q1 = c ? strchr(c + 1, '"') : nullptr;
    const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
    if (!q1 || !q2) return {};
    return std::string(q1 + 1, q2);
}

/* extra JSON 无符号整数值; 支持十进制/0x 前缀 */
uint32_t jsonUint(const char* json, const char* key) {
    if (!json || !key) return 0;
    const std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    const char* c = k ? strchr(k + pat.size(), ':') : nullptr;
    return c ? (uint32_t)strtoul(c + 1, nullptr, 0) : 0;
}

} // namespace

extern "C" {

GS32LINK_EXPORT const char* gs32_driver_name(void) { return "gslink"; }

/* 宿主注册日志回调: 驱动日志(GUI 日志面板)替代控制台弹窗 */
GS32LINK_EXPORT void gs32_set_log_sink(void (*fn)(const char*, void*), void* user) {
    gs32::setLogSink(fn, user);
}

/* 枚举 FTDI 端口描述, '|' 分隔写入 buf; buf=NULL 返回所需字节数 */
GS32LINK_EXPORT int gs32_list_ports(char* buf, uint32_t buflen) {
    gs32::FtdiJtag j;
    std::vector<std::string> ports;
    if (!j.listPorts(ports) || ports.empty()) return 0;
    std::string all;
    for (const auto& p : ports) all += p + "|";
    if (!buf) return (int)all.size() + 1;
    const uint32_t n = buflen < (uint32_t)all.size() + 1 ? buflen
                                                        : (uint32_t)all.size() + 1;
    memcpy(buf, all.c_str(), n);
    return (int)ports.size();
}

GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen) {
    static const char* v = "gs32link gslink 0.5.0 (D2XX, layered+batch)";
    if (!buf) return (int)strlen(v) + 1;
    uint32_t n = buflen < (uint32_t)strlen(v) + 1 ? buflen : (uint32_t)strlen(v) + 1;
    memcpy(buf, v, n);
    return (int)n;
}

GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t h, uint32_t* caps) {
    if (!caps) return GS32_ERR_PARAM;
    (void)h;
    *caps = GS32_CAP_POLL | GS32_CAP_WRITE;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_open(const gs32_config_t* cfg, gs32_handle_t* out) {
    if (!cfg || !out) return GS32_ERR_PARAM;
    gs32::debugInit();
    gs32::logInfo("[open] device=%s interface=%s speed=%u\n",
        cfg->device ? cfg->device : "?", cfg->interface ? cfg->interface : "?",
        cfg->speed_khz);
    auto* s = new GsLinkState();
    std::string err;
    /* extra JSON {"port":"desc"}: 指定打开的 FTDI 端口(无 JSON 库, 手工提取) */
    const char* port = nullptr;
    std::string portStr;
    if (cfg->extra) {
        const char* k = strstr(cfg->extra, "\"port\"");
        const char* q1 = k ? strchr(k + 6, '"') : nullptr;
        const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
        if (q1 && q2 && q2 > q1 + 1) {
            portStr.assign(q1 + 1, q2);
            port = portStr.c_str();
        }
    }
    const uint32_t speed = cfg->speed_khz ? cfg->speed_khz : 4000;
    if (!s->jtag.open(speed, port, err)) {
        /* D2XX 打不开(设备被绑到 WinUSB/Nuclei "Hbird FTDI Driver" 等驱动
         * 时 D2XX 列表里没有这块板): 改用 libftdi+libusb 直接按 VID/PID 打开 */
        std::string e2;
        gs32::logInfo("[open] D2XX open failed (%s), trying libftdi/libusb\n", err.c_str());
        if (!s->jtag.openLibusb(speed, port, e2)) {
            g_global_error = err + " / " + e2;
            gs32::logInfo("[open] %s\n", g_global_error.c_str());
            delete s;
            return GS32_ERR_CONNECT;
        }
        gs32::logInfo("[open] using libftdi/libusb backend\n");
    }

    /* 目标校验: 型号 + 期望 IDCODE 来自宿主 extra JSON */
    std::string model;
    uint32_t expectedId = 0, taps = 1, tapIndex = 0;
    if (cfg->extra) {
        model = jsonStr(cfg->extra, "model");
        expectedId = jsonUint(cfg->extra, "idcode");
        taps = jsonUint(cfg->extra, "taps");
        tapIndex = jsonUint(cfg->extra, "tap");
        if (taps == 0) taps = 1;
    }
    if (expectedId) {
        gs32::logInfo("[open] target=%s expected IDCODE=0x%08X chain=%u/%u\n",
                      model.c_str(), expectedId, taps, tapIndex);
    }
    s->jtag.setChain((int)taps, (int)tapIndex);
    s->dbg.setExpectedChip(expectedId, model.c_str());

    /* 连接即完成 TAP/DM 初始化并校验目标; 失败则连接失败 */
    int initRc = s->dbg.ensureReady();
    if (initRc != GS32_OK && !s->jtag.usingLibusb()) {
        /* D2XX 可能开到了另一台 FTDI 设备(本机插了多个 FTDI 时就是这种情况):
         * 关掉, 用 libftdi 直连 0x0403:0x6010 再试一次. */
        std::string e2;
        gs32::logInfo("[open] D2XX 连接失败, 改用 libftdi/libusb 重试\n");
        s->jtag.close();
        if (s->jtag.openLibusb(speed, port, e2)) {
            initRc = s->dbg.ensureReady();
        } else {
            gs32::logInfo("[open] libftdi fallback failed: %s\n", e2.c_str());
        }
    }
    if (initRc != GS32_OK) {
        g_global_error = s->err.text();
        gs32::logInfo("[open] %s\n", g_global_error.c_str());
        s->jtag.close();
        delete s;
        return initRc;
    }
    /* 精确型号校验: 芯片 OTP 信息块 0x1FFF40D0..
     *   [2] = Flash 档位, [3] = SYS_Freq(MHz), [6..7] = DEV_Seq(大端)
     * 期望值由上位机按型号表放在 extra JSON (pseq/pfreq/pflash);
     * pseq==0 表示该型号未采集样本, 跳过精确校验(仍做 IDCODE 校验). */
    const uint32_t expSeq = jsonUint(cfg->extra, "pseq");
    if (expSeq) {
        const uint32_t expFreq = jsonUint(cfg->extra, "pfreq");
        const uint32_t expFlash = jsonUint(cfg->extra, "pflash");
        uint8_t info[8] = {0};
        const int rc = s->dbg.readMem(0x1FFF40D0u, info, sizeof(info));
        if (rc == GS32_OK) {
            const uint32_t actSeq = (uint32_t(info[6]) << 8) | uint32_t(info[7]);
            const uint32_t actFreq = uint32_t(info[3]) | (uint32_t(info[4]) << 8);
            const uint32_t actFlash = info[2];
            if (actSeq != expSeq || actFreq != expFreq || actFlash != expFlash) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "目标不匹配: 选择 %s (DEV_Seq=0x%04X Flash=%u Freq=%uMHz), "
                              "实际 DEV_Seq=0x%04X Flash=%u Freq=%uMHz",
                              model.c_str(), unsigned(expSeq), unsigned(expFlash),
                              unsigned(expFreq), unsigned(actSeq), unsigned(actFlash),
                              unsigned(actFreq));
                g_global_error = buf;
                s->err.set(GS32_ERR_CONNECT, buf);
                gs32::logInfo("[open] %s\n", buf);
                s->jtag.close();
                delete s;
                return GS32_ERR_CONNECT;
            }
            gs32::logInfo("[open] partnumber ok: DEV_Seq=0x%04X Flash=%u Freq=%uMHz\n",
                          unsigned(actSeq), unsigned(actFlash), unsigned(actFreq));
        } else {
            gs32::logInfo("[open] partnumber read failed rc=%d, skip exact check\n", rc);
        }
    }
    s->err.clear();
    *out = s;
    gs32::logInfo("[open] OK, ready for JTAG\n");
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_close(gs32_handle_t h) {
    auto* s = static_cast<GsLinkState*>(h);
    if (!s) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->jtag.close();
    delete s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_read(gs32_handle_t h,
                              const gs32_mem_region_t* regions, uint32_t count,
                              uint8_t* out_buf, uint32_t buf_size,
                              uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<GsLinkState*>(h);
    if (!s || !regions || !count || !out_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    int rc = s->dbg.ensureReady();
    if (rc != GS32_OK) return rc;

    /* 首次读取时做一次 SBA + Abstract 路径探测(用实际监控地址) */
    if (!s->sbaDiagDone) {
        s->dbg.sbaDiag(regions[0].addr);
        s->dbg.abstractDiag(regions[0].addr);
        s->sbaDiagDone = true;
    }

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "out_buf too small");
        return GS32_ERR_PARAM;
    }
    /* 多变量(多 region)整体批量聚合: 单笔 USB 事务读完全部变量,
     * 消除变量间的 ftd2xx 往返间隔 */
    std::vector<gs32::RiscvDebug::MemRegion> regs(count);
    for (uint32_t i = 0; i < count; ++i)
        regs[i] = {regions[i].addr, regions[i].size};
    if (s->dbg.readRegions(regs.data(), count, out_buf)) {
        s->err.set(GS32_ERR_READ, "SBA multi-region read FAILED");
        return GS32_ERR_READ;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_write(gs32_handle_t h,
                               const gs32_mem_region_t* regions, uint32_t count,
                               const uint8_t* in_buf, uint32_t buf_size,
                               uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<GsLinkState*>(h);
    if (!s || !regions || !count || !in_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    int rc = s->dbg.ensureReady();
    if (rc != GS32_OK) return rc;

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) { s->err.set(GS32_ERR_PARAM, "in_buf too small"); return GS32_ERR_PARAM; }

    const uint8_t* p = in_buf;
    for (uint32_t i = 0; i < count; ++i) {
        if (!regions[i].size) continue;
        if (s->dbg.writeMem(regions[i].addr, p, regions[i].size)) {
            char a[16];
            snprintf(a, sizeof(a), "%X", regions[i].addr);
            s->err.set(GS32_ERR_WRITE, "SBA write FAILED @0x" + std::string(a));
            return GS32_ERR_WRITE;
        }
        p += regions[i].size;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h) {
    return h ? static_cast<GsLinkState*>(h)->err.text() : g_global_error.c_str();
}

/* 诊断统计: 最近一次批量读取的 FT_Write 提交耗时(ms), 采样抖动排查用 */
GS32LINK_EXPORT int gs32_last_write_ms(gs32_handle_t h, double* out_ms) {
    auto* s = static_cast<GsLinkState*>(h);
    if (!s || !out_ms) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    *out_ms = s->dbg.lastWriteMs();
    return GS32_OK;
}

/* STREAM(P1): 无硬件定时采样, 返回未实现(退化为 POLL 模式) */
GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t, uint32_t,
                                       const gs32_mem_region_t*, uint32_t,
                                       const gs32_flush_policy_t*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t, gs32_notify_cb, void*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t)  { return GS32_ERR_NOT_IMPL; }

} /* extern "C" */
